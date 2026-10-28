#include "JoltTestSupport.h"
#include <JoltPlugin/Character/JoltDefaultCharacterComponent.h>

using namespace JoltTest;

/// A default character that stands on the origin and can be walked along +x.
class Walker
{
public:
  explicit Walker(const plVec3& vFootPosition = plVec3(0, 0, 0.02f))
  {
    m_pObject = m_World.CreateObject(vFootPosition);

    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    plJoltDefaultCharacterComponent::CreateComponent(m_pObject, m_pCharacter);
  }

  /// Holds the forward input for the given amount of seconds. Calls the optional per-frame callback.
  template <typename Callback>
  void Walk(float fSeconds, float fForward, Callback&& callback)
  {
    const plUInt32 uiFrames = static_cast<plUInt32>(fSeconds * m_World.GetFrameRate());
    for (plUInt32 i = 0; i < uiFrames; ++i)
    {
      callback(i);

      {
        PL_LOCK(m_World.GetWorld().GetWriteMarker());
        m_pCharacter->Move(fForward, 0.0f);
      }

      m_World.Step(1);
    }
  }

  void Walk(float fSeconds, float fForward)
  {
    Walk(fSeconds, fForward, [](plUInt32) {});
  }

  plVec3 Position() { return m_World.GetPosition(m_pObject->GetHandle()); }

  bool IsOnGround()
  {
    PL_LOCK(m_World.GetWorld().GetReadMarker());
    return m_pCharacter->IsStandingOnGround();
  }

  World m_World;
  plGameObject* m_pObject = nullptr;
  plJoltDefaultCharacterComponent* m_pCharacter = nullptr;
};

/// Builds a thin ramp that rises along +x by the given angle. Its top surface crosses the floor at x = 3.
///
/// The engine's handedness decides which sign of the rotation rises along +x, so both are tried and the one that rises is kept.
static void AddRamp(World& ref_world, float fAngleDegree)
{
  const float fLength = 4.0f;
  const plAngle angle = plAngle::MakeFromDegree(fAngleDegree);
  const float fCenterHeight = plMath::Tan(angle) * fLength - 0.1f / plMath::Cos(angle);

  for (const float fSign : {1.0f, -1.0f})
  {
    const plQuat qRot = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(fSign * fAngleDegree));
    plGameObject* pRamp = ref_world.AddStaticBox(plVec3(3.0f + fLength, 0, fCenterHeight), plVec3(fLength, 2.0f, 0.1f), 0, qRot);
    ref_world.Step(2);

    plPhysicsCastResult low, high;
    ref_world.GetJolt().Raycast(low, plVec3(4.5f, 0, 20.0f), plVec3(0, 0, -1), 40.0f, AllShapes(), plPhysicsHitCollection::Closest);
    ref_world.GetJolt().Raycast(high, plVec3(8.0f, 0, 20.0f), plVec3(0, 0, -1), 40.0f, AllShapes(), plPhysicsHitCollection::Closest);

    const float fLowZ = 20.0f - low.m_fDistance;
    const float fHighZ = 20.0f - high.m_fDistance;
    if (fHighZ > fLowZ + 0.2f)
      return;

    PL_LOCK(ref_world.GetWorld().GetWriteMarker());
    ref_world.GetWorld().DeleteObjectNow(pRamp->GetHandle());
  }
}

static void TestStandsOnFloor()
{
  Walker walker;
  walker.m_World.AddFloor();
  walker.m_World.Step(120);

  Check(walker.IsOnGround(), "the character stands on the floor");
  CheckNear(walker.Position().z, 0.0f, 0.05f, "the character's feet are on the floor");
}

static void TestWalksForward()
{
  Walker walker;
  walker.m_World.AddFloor();
  walker.m_World.Step(30);
  walker.Walk(2.0f, 1.0f);

  const plVec3 vPos = walker.Position();
  Check(vPos.x > 1.5f && vPos.x < 4.0f, "the character walked about two seconds at walking speed");
  Check(walker.IsOnGround(), "the character is still on the ground after walking");
}

static void TestStepsUp()
{
  Walker walker;
  walker.m_World.AddFloor();
  walker.m_World.AddStaticBox(plVec3(5.0f, 0, 0.1f), plVec3(3.0f, 5.0f, 0.1f));
  walker.m_World.Step(30);
  walker.Walk(4.0f, 1.0f);

  const plVec3 vPos = walker.Position();
  Check(vPos.x > 3.5f, "the character climbed a low step");
  CheckNear(vPos.z, 0.2f, 0.06f, "the character stands on top of the low step");
}

static void TestHighStepBlocks()
{
  Walker walker;
  walker.m_World.AddFloor();
  walker.m_World.AddStaticBox(plVec3(3.0f, 0, 0.6f), plVec3(1.0f, 5.0f, 0.6f));
  walker.m_World.Step(30);
  walker.Walk(5.0f, 1.0f);

  const plVec3 vPos = walker.Position();
  Check(vPos.x < 2.2f, "the character does not walk through a high wall");
  Check(vPos.z < 0.3f, "the character does not climb a high step");
}

static void TestWalksUpGentleSlope()
{
  Walker walker;
  walker.m_World.AddFloor();
  AddRamp(walker.m_World, 20.0f);
  walker.m_World.Step(30);
  walker.Walk(5.0f, 1.0f);

  const plVec3 vPos = walker.Position();
  Check(vPos.x > 4.5f, "the character walked onto the gentle ramp");
  Check(vPos.z > 0.4f, "the character climbs a 20 degree ramp");
  Check(walker.IsOnGround(), "the character stands on the gentle ramp");
}

static void TestSteepSlopeBlocks()
{
  Walker walker;
  walker.m_World.AddFloor();
  AddRamp(walker.m_World, 70.0f);
  walker.m_World.Step(30);
  walker.Walk(5.0f, 1.0f);

  Check(walker.Position().z < 0.8f, "the character does not climb a 70 degree ramp");
}

/// A kinematic platform that moves at one meter per second carries a standing character along.
static void TestMovingPlatformCarriesCharacter()
{
  Walker walker(plVec3(0, 0, 0.52f));

  plGameObject* pPlatform = walker.m_World.AddDynamicBox(plVec3(0, 0, 0.25f), plVec3(2.0f, 2.0f, 0.25f), 0, 0.0f);
  {
    PL_LOCK(walker.m_World.GetWorld().GetWriteMarker());
    plJoltDynamicActorComponent* pActor = nullptr;
    pPlatform->TryGetComponentOfBaseType(pActor);
    pActor->SetKinematic(true);
  }

  walker.m_World.Step(60);
  const float fStartX = walker.Position().x;

  const float fSpeed = 1.0f;
  const float fDelta = fSpeed / walker.m_World.GetFrameRate();
  walker.Walk(3.0f, 0.0f, [&](plUInt32)
    {
      PL_LOCK(walker.m_World.GetWorld().GetWriteMarker());
      pPlatform->SetGlobalPosition(pPlatform->GetGlobalPosition() + plVec3(fDelta, 0, 0));
    });

  const float fPlatformX = walker.m_World.GetPosition(pPlatform->GetHandle()).x;
  Check(fPlatformX > 2.5f, "the platform moved");
  CheckNear(walker.Position().x - fStartX, 3.0f, 0.6f, "the character rides the moving platform", Category::R1Gate);
}

/// Teleporting a character puts it at the new position and keeps it on the ground there.
static void TestTeleport()
{
  Walker walker;
  walker.m_World.AddFloor();
  walker.m_World.Step(30);

  {
    PL_LOCK(walker.m_World.GetWorld().GetWriteMarker());
    walker.m_pCharacter->TeleportCharacter(plVec3(20.0f, 20.0f, 0.02f));
  }

  walker.m_World.Step(60);

  const plVec3 vPos = walker.Position();
  CheckNear(vPos.x, 20.0f, 0.2f, "the character arrived at the teleport target", Category::R1Gate);
  Check(walker.IsOnGround(), "the character is on the ground after teleporting", Category::R1Gate);
}

void RunCharacterTests()
{
  BeginGroup("Character");

  TestStandsOnFloor();
  TestWalksForward();
  TestStepsUp();
  TestHighStepBlocks();
  TestWalksUpGentleSlope();
  TestSteepSlopeBlocks();
  TestMovingPlatformCarriesCharacter();
  TestTeleport();
}
