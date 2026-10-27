#include <Core/Interfaces/PhysicsQuery.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/Startup.h>
#include <JoltPlugin/Actors/JoltStaticActorComponent.h>
#include <JoltPlugin/Shapes/JoltShapeBoxComponent.h>
#include <JoltPlugin/Shapes/JoltShapeCapsuleComponent.h>
#include <JoltPlugin/System/JoltTempAlloc.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <cstdio>

static int s_Failures = 0;
static int s_Checks = 0;

static void Check(bool value, const char* message)
{
  ++s_Checks;
  if (!value)
  {
    std::printf("FAIL: %s\n", message);
    ++s_Failures;
  }
}

/// \brief A world that updates at a fixed 60 Hz timestep.
class JoltTestWorld
{
public:
  JoltTestWorld()
  {
    plWorldDesc desc("JoltP0Tests");
    m_pWorld = PL_DEFAULT_NEW(plWorld, desc);

    PL_LOCK(m_pWorld->GetWriteMarker());
    m_pWorld->SetWorldSimulationEnabled(true);
    m_pWorld->GetClock().SetFixedTimeStep(plTime::MakeFromSeconds(1.0 / 60.0));
  }

  ~JoltTestWorld() { m_pWorld.Clear(); }

  plWorld& GetWorld() { return *m_pWorld; }

  void Step(plUInt32 uiFrames)
  {
    for (plUInt32 i = 0; i < uiFrames; ++i)
    {
      PL_LOCK(m_pWorld->GetWriteMarker());
      m_pWorld->Update();
    }
  }

  plGameObject* CreateObject(const plVec3& vPosition)
  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    plGameObjectDesc desc;
    desc.m_LocalPosition = vPosition;

    plGameObject* pObject = nullptr;
    m_pWorld->CreateObject(desc, pObject);
    return pObject;
  }

private:
  plUniquePtr<plWorld> m_pWorld;
};

static void* AllocBytes(plJoltTempAlloc& ref_alloc, plUInt32 uiSize)
{
  return ref_alloc.Allocate(uiSize);
}

/// PH-B08: a request that skips an existing, too-small chunk must unwind through that chunk again.
static void TestTempAllocatorUnwinding()
{
  plJoltTempAlloc alloc("JoltP0TempAlloc");

  // grow a second 1 MB chunk, then release it again so it stays allocated but empty
  void* pP = AllocBytes(alloc, 900 * 1024);
  void* pQ = AllocBytes(alloc, 500 * 1024);
  alloc.Free(pQ, 500 * 1024);
  alloc.Free(pP, 900 * 1024);

  // X lives in the first chunk; Y does not fit the empty 1 MB chunk and skips it
  void* pX = AllocBytes(alloc, 300 * 1024);
  void* pY = AllocBytes(alloc, 2 * 1024 * 1024);
  Check(alloc.m_Chunks.GetCount() == 3, "oversized request skipped the existing chunk and added a third one");

  alloc.Free(pY, 2 * 1024 * 1024);
  alloc.Free(pX, 300 * 1024);

  for (const auto& chunk : alloc.m_Chunks)
    Check(chunk.m_uiLastOffset == 0, "every chunk is empty after freeing in reverse order");

  void* pAgain = AllocBytes(alloc, 64);
  Check(pAgain == alloc.m_Chunks[0].m_pPtr, "allocation after full unwind starts at the first chunk");
  alloc.Free(pAgain, 64);

  // randomized changing sizes: the allocator must stay inside its chunks and end empty
  struct LiveAllocation
  {
    void* m_pPtr;
    plUInt32 m_uiSize;
    PL_DECLARE_POD_TYPE();
  };

  plHybridArray<LiveAllocation, 64> live;
  plUInt32 uiSeed = 12345;
  for (plUInt32 i = 0; i < 400; ++i)
  {
    uiSeed = uiSeed * 1664525u + 1013904223u;
    if (!live.IsEmpty() && (uiSeed >> 24) % 3 == 0)
    {
      alloc.Free(live.PeekBack().m_pPtr, live.PeekBack().m_uiSize);
      live.PopBack();
    }
    else
    {
      const plUInt32 uiSize = 16 + ((uiSeed >> 8) % (3 * 1024 * 1024));
      live.PushBack({alloc.Allocate(uiSize), uiSize});
    }
  }

  while (!live.IsEmpty())
  {
    alloc.Free(live.PeekBack().m_pPtr, live.PeekBack().m_uiSize);
    live.PopBack();
  }

  bool bAllEmpty = true;
  for (const auto& chunk : alloc.m_Chunks)
    bAllEmpty = bAllEmpty && chunk.m_uiLastOffset == 0;
  Check(bAllEmpty, "changing-size allocations unwind back to empty chunks");
}

static plGameObject* AddStaticBox(JoltTestWorld& ref_world, const plVec3& vPosition, const plVec3& vHalfExtents)
{
  plGameObject* pObject = ref_world.CreateObject(vPosition);

  PL_LOCK(ref_world.GetWorld().GetWriteMarker());
  plJoltStaticActorComponent* pActor = nullptr;
  plJoltStaticActorComponent::CreateComponent(pObject, pActor);

  plJoltShapeBoxComponent* pBox = nullptr;
  plJoltShapeBoxComponent::CreateComponent(pObject, pBox);
  pBox->SetHalfExtents(vHalfExtents);
  return pObject;
}

static void AddStaticCapsule(JoltTestWorld& ref_world, const plVec3& vPosition, float fRadius, float fHeight)
{
  plGameObject* pObject = ref_world.CreateObject(vPosition);

  PL_LOCK(ref_world.GetWorld().GetWriteMarker());
  plJoltStaticActorComponent* pActor = nullptr;
  plJoltStaticActorComponent::CreateComponent(pObject, pActor);

  plJoltShapeCapsuleComponent* pCapsule = nullptr;
  plJoltShapeCapsuleComponent::CreateComponent(pObject, pCapsule);
  pCapsule->SetRadius(fRadius);
  pCapsule->SetHeight(fHeight);
}

/// PH-B07: more than 256 hits must all be returned.
static void TestRaycastAllBeyond256Hits()
{
  JoltTestWorld test;
  auto* pModule = test.GetWorld().GetOrCreateModule<plJoltWorldModule>();

  const plUInt32 uiBoxes = 300;
  for (plUInt32 i = 0; i < uiBoxes; ++i)
    AddStaticBox(test, plVec3(static_cast<float>(i) * 2.0f, 0, 0), plVec3(0.25f));

  test.Step(3);

  plPhysicsCastResultArray results;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  const bool bHit = pModule->RaycastAll(results, plVec3(-5, 0, 0), plVec3(1, 0, 0), static_cast<float>(uiBoxes) * 2.0f + 10.0f, params);

  Check(bHit, "ray through 300 boxes reports a hit");
  Check(results.m_Results.GetCount() == uiBoxes, "ray through 300 boxes returns every hit");
}

/// PH-B11: a capsule with zero cylinder height is a valid sphere, an invalid radius is rejected.
static void TestCapsuleDimensions()
{
  JoltTestWorld test;
  auto* pModule = test.GetWorld().GetOrCreateModule<plJoltWorldModule>();

  AddStaticCapsule(test, plVec3::MakeZero(), 0.5f, 0.0f);
  test.Step(3);

  plPhysicsCastResult result;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  Check(pModule->Raycast(result, plVec3(0, 0, 3), plVec3(0, 0, -1), 10.0f, params), "zero-height capsule collides as a sphere");

  AddStaticCapsule(test, plVec3(10, 0, 0), 0.0f, 1.0f);

  // must log an error and produce no collider, but never crash
  test.Step(3);
  Check(!pModule->Raycast(result, plVec3(10, 0, 3), plVec3(0, 0, -1), 10.0f, params), "capsule without radius creates no collider");
}

/// PH-B27: impulses for bodies that will never be added are retired, not retained forever.
static void TestUndeliverableImpulsesAreRetired()
{
  JoltTestWorld test;
  auto* pModule = test.GetWorld().GetOrCreateModule<plJoltWorldModule>();

  for (plUInt32 i = 0; i < 100; ++i)
    pModule->AddImpulse(0x00FF0000u + i, plVec3(0, 0, 1));

  test.Step(3);
  Check(pModule->GetQueuedImpulseCount() == 0, "impulses for unknown bodies do not stay queued");
}

/// PH-B10 / PH-B09: deleting an actor with a shape and stepping again keeps the world stable.
static void TestActorTeardownKeepsWorldStable()
{
  JoltTestWorld test;
  auto* pModule = test.GetWorld().GetOrCreateModule<plJoltWorldModule>();

  plHybridArray<plGameObjectHandle, 16> objects;
  for (plUInt32 i = 0; i < 16; ++i)
  {
    plGameObject* pObject = AddStaticBox(test, plVec3(static_cast<float>(i) * 3.0f, 0, 0), plVec3(0.5f));
    objects.PushBack(pObject->GetHandle());
  }

  test.Step(3);

  // delete in the same frame some actors were created, then recreate shapes in the freed user data slots
  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    for (plUInt32 i = 0; i < objects.GetCount(); i += 2)
      test.GetWorld().DeleteObjectNow(objects[i]);
  }

  for (plUInt32 i = 0; i < 8; ++i)
    AddStaticBox(test, plVec3(static_cast<float>(i) * 3.0f + 100.0f, 0, 0), plVec3(0.5f));

  test.Step(3);

  plPhysicsCastResult result;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  Check(!pModule->Raycast(result, plVec3(-1, 0, 0), plVec3(1, 0, 0), 1.0f, params), "no hit is reported for the deleted actor at the origin");
  Check(pModule->Raycast(result, plVec3(2.0f, 0, 0), plVec3(1, 0, 0), 5.0f, params), "retained actor is still hit");
}

int main(int argc, char** argv)
{
  plStartup::StartupCoreSystems();
  plStartup::StartupHighLevelSystems();

  TestTempAllocatorUnwinding();
  TestRaycastAllBeyond256Hits();
  TestCapsuleDimensions();
  TestUndeliverableImpulsesAreRetired();
  TestActorTeardownKeepsWorldStable();

  plStartup::ShutdownHighLevelSystems();
  plStartup::ShutdownCoreSystems();
  std::printf("Jolt P0: %d checks, %d failure(s)\n", s_Checks, s_Failures);
  return s_Failures == 0 ? 0 : 1;
}
