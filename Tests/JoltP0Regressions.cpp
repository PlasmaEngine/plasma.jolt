#include "JoltTestSupport.h"
#include <JoltPlugin/Shapes/JoltShapeCapsuleComponent.h>
#include <JoltPlugin/System/JoltTempAlloc.h>

using namespace JoltTest;

static void* AllocBytes(plJoltTempAlloc& ref_alloc, plUInt32 uiSize)
{
  return ref_alloc.Allocate(uiSize);
}

/// PH-B08: a request that skips an existing, too-small chunk must unwind through that chunk again.
static void TestTempAllocatorUnwinding()
{
  plJoltTempAlloc alloc("JoltTestsTempAlloc");

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

/// PH-B07: more than 256 hits must all be returned.
static void TestRaycastAllBeyond256Hits()
{
  World test;

  const plUInt32 uiBoxes = 300;
  for (plUInt32 i = 0; i < uiBoxes; ++i)
    test.AddStaticBox(plVec3(static_cast<float>(i) * 2.0f, 0, 0), plVec3(0.25f));

  test.Step(3);

  plPhysicsCastResultArray results;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  const bool bHit = test.GetJolt().RaycastAll(results, plVec3(-5, 0, 0), plVec3(1, 0, 0), static_cast<float>(uiBoxes) * 2.0f + 10.0f, params);

  Check(bHit, "ray through 300 boxes reports a hit");
  Check(results.m_Results.GetCount() == uiBoxes, "ray through 300 boxes returns every hit");
}

/// PH-B11: a capsule with zero cylinder height is a valid sphere, an invalid radius is rejected.
static void TestCapsuleDimensions()
{
  World test;

  auto addCapsule = [&](const plVec3& vPosition, float fRadius, float fHeight)
  {
    plGameObject* pObject = test.CreateObject(vPosition);

    PL_LOCK(test.GetWorld().GetWriteMarker());
    plJoltStaticActorComponent* pActor = nullptr;
    plJoltStaticActorComponent::CreateComponent(pObject, pActor);

    plJoltShapeCapsuleComponent* pCapsule = nullptr;
    plJoltShapeCapsuleComponent::CreateComponent(pObject, pCapsule);
    pCapsule->SetRadius(fRadius);
    pCapsule->SetHeight(fHeight);
  };

  addCapsule(plVec3::MakeZero(), 0.5f, 0.0f);
  test.Step(3);

  plPhysicsCastResult result;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  Check(test.GetJolt().Raycast(result, plVec3(0, 0, 3), plVec3(0, 0, -1), 10.0f, params), "zero-height capsule collides as a sphere");

  addCapsule(plVec3(10, 0, 0), 0.0f, 1.0f);

  // must log an error and produce no collider, but never crash
  test.Step(3);
  Check(!test.GetJolt().Raycast(result, plVec3(10, 0, 3), plVec3(0, 0, -1), 10.0f, params), "capsule without radius creates no collider");
}

/// PH-B27: impulses for bodies that will never be added are retired, not retained forever.
static void TestUndeliverableImpulsesAreRetired()
{
  World test;

  for (plUInt32 i = 0; i < 100; ++i)
    test.GetJolt().AddImpulse(0x00FF0000u + i, plVec3(0, 0, 1));

  test.Step(3);
  Check(test.GetJolt().GetQueuedImpulseCount() == 0, "impulses for unknown bodies do not stay queued");
}

/// PH-B10 / PH-B09: deleting an actor with a shape and stepping again keeps the world stable.
static void TestActorTeardownKeepsWorldStable()
{
  World test;

  plHybridArray<plGameObjectHandle, 16> objects;
  for (plUInt32 i = 0; i < 16; ++i)
    objects.PushBack(test.AddStaticBox(plVec3(static_cast<float>(i) * 3.0f, 0, 0), plVec3(0.5f))->GetHandle());

  test.Step(3);

  // delete in the same frame some actors were created, then recreate shapes in the freed user data slots
  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    for (plUInt32 i = 0; i < objects.GetCount(); i += 2)
      test.GetWorld().DeleteObjectNow(objects[i]);
  }

  for (plUInt32 i = 0; i < 8; ++i)
    test.AddStaticBox(plVec3(static_cast<float>(i) * 3.0f + 100.0f, 0, 0), plVec3(0.5f));

  test.Step(3);

  plPhysicsCastResult result;
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);
  Check(!test.GetJolt().Raycast(result, plVec3(-1, 0, 0), plVec3(1, 0, 0), 1.0f, params), "no hit is reported for the deleted actor at the origin");
  Check(test.GetJolt().Raycast(result, plVec3(2.0f, 0, 0), plVec3(1, 0, 0), 5.0f, params), "retained actor is still hit");
}

/// R1: the gravity the module reports is the gravity of the world settings.
static void TestGravityIsReported()
{
  World test;

  const plVec3 vGravity = test.GetJolt().GetGravity();
  CheckNear(vGravity.z, test.GetJolt().GetObjectGravity().z, 0.001f, "GetGravity() returns the configured object gravity", Category::R1Gate);

  test.GetJolt().SetGravity(plVec3(0, 0, -3.0f), plVec3(0, 0, -3.0f));
  CheckNear(test.GetJolt().GetGravity().z, -3.0f, 0.001f, "GetGravity() follows SetGravity()", Category::R1Gate);
}

void RunP0Regressions()
{
  BeginGroup("P0 regressions");

  TestTempAllocatorUnwinding();
  TestRaycastAllBeyond256Hits();
  TestCapsuleDimensions();
  TestUndeliverableImpulsesAreRetired();
  TestActorTeardownKeepsWorldStable();
  TestGravityIsReported();
}
