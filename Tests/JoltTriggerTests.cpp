#include "JoltTestSupport.h"
#include <Core/Messages/TriggerMessage.h>
#include <JoltPlugin/Actors/JoltTriggerComponent.h>

using namespace JoltTest;

using TriggerRecorderManager = plComponentManager<class TriggerRecorderComponent, plBlockStorageType::FreeList>;

/// \brief Counts the trigger messages that arrive at its object.
class TriggerRecorderComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(TriggerRecorderComponent, plComponent, TriggerRecorderManager);

public:
  void OnTriggered(plMsgTriggerTriggered& ref_msg)
  {
    if (ref_msg.m_TriggerState == plTriggerState::Activated)
      ++m_uiActivated;
    else if (ref_msg.m_TriggerState == plTriggerState::Deactivated)
      ++m_uiDeactivated;
  }

  plUInt32 m_uiActivated = 0;
  plUInt32 m_uiDeactivated = 0;
};

// clang-format off
PL_BEGIN_COMPONENT_TYPE(TriggerRecorderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgTriggerTriggered, OnTriggered),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

/// Creates a trigger actor with a recorder on the given object.
static void MakeTrigger(World& ref_test, plGameObject* pObject, plUInt8 uiLayer = 0)
{
  PL_LOCK(ref_test.GetWorld().GetWriteMarker());

  plJoltTriggerComponent* pTrigger = nullptr;
  plJoltTriggerComponent::CreateComponent(pObject, pTrigger);
  pTrigger->SetTriggerMessage("zone");
  pTrigger->m_uiCollisionLayer = uiLayer;

  TriggerRecorderComponent* pRecorder = nullptr;
  TriggerRecorderComponent::CreateComponent(pObject, pRecorder);
}

static void AddBoxShape(World& ref_test, plGameObject* pObject, const plVec3& vHalfExtents)
{
  PL_LOCK(ref_test.GetWorld().GetWriteMarker());

  plJoltShapeBoxComponent* pBox = nullptr;
  plJoltShapeBoxComponent::CreateComponent(pObject, pBox);
  pBox->SetHalfExtents(vHalfExtents);
}

static TriggerRecorderComponent* GetRecorder(World& ref_test, plGameObject* pObject)
{
  PL_LOCK(ref_test.GetWorld().GetReadMarker());

  TriggerRecorderComponent* pRecorder = nullptr;
  pObject->TryGetComponentOfBaseType(pRecorder);
  return pRecorder;
}

/// A body that falls through a trigger is reported on entering and leaving, and is not stopped by it.
static void TestTriggerEnterAndLeave()
{
  World test;
  test.AddFloor();

  plGameObject* pTrigger = test.CreateObject(plVec3(0, 0, 3.0f));
  AddBoxShape(test, pTrigger, plVec3(1.0f, 1.0f, 0.5f));
  MakeTrigger(test, pTrigger);

  const plGameObjectHandle hBox = test.AddDynamicBox(plVec3(0, 0, 6.0f), plVec3(0.25f))->GetHandle();

  test.StepSeconds(4.0f);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr, "the trigger has a recorder");
  if (pRecorder == nullptr)
    return;

  Check(pRecorder->m_uiActivated == 1, "the trigger reports the body entering once");
  Check(pRecorder->m_uiDeactivated == 1, "the trigger reports the body leaving once");
  CheckNear(test.GetPosition(hBox).z, 0.25f, 0.05f, "the body fell through the trigger onto the floor");
}

/// A body that never touches a trigger is never reported.
static void TestTriggerIgnoresBodiesOutsideOfIt()
{
  World test;
  test.AddFloor();

  plGameObject* pTrigger = test.CreateObject(plVec3(10.0f, 0, 3.0f));
  AddBoxShape(test, pTrigger, plVec3(1.0f, 1.0f, 0.5f));
  MakeTrigger(test, pTrigger);

  test.AddDynamicBox(plVec3(0, 0, 6.0f), plVec3(0.25f));
  test.StepSeconds(3.0f);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr && pRecorder->m_uiActivated == 0 && pRecorder->m_uiDeactivated == 0, "a distant body does not fire the trigger");
}

/// A trigger made of two overlapping child shapes. A body that crosses both shapes enters and leaves the trigger once.
static void TestCompoundTrigger()
{
  World test;

  plGameObject* pTrigger = test.CreateObject(plVec3::MakeZero());
  MakeTrigger(test, pTrigger);

  for (const float fOffset : {-0.25f, 0.25f})
  {
    plGameObject* pChild = nullptr;
    {
      PL_LOCK(test.GetWorld().GetWriteMarker());
      plGameObjectDesc desc;
      desc.m_bDynamic = true;
      desc.m_hParent = pTrigger->GetHandle();
      desc.m_LocalPosition = plVec3(fOffset, 0, 0);
      test.GetWorld().CreateObject(desc, pChild);
    }

    AddBoxShape(test, pChild, plVec3(0.5f));
  }

  plGameObject* pMover = test.AddDynamicBox(plVec3(-3.0f, 0, 0), plVec3(0.2f), 0, 0.0f);
  test.Step(3);
  test.SetVelocity(pMover, plVec3(3.0f, 0, 0));

  test.StepSeconds(3.0f);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr, "the compound trigger has a recorder");
  if (pRecorder == nullptr)
    return;

  Check(pRecorder->m_uiActivated >= 1, "the compound trigger reports the body entering");
  Check(pRecorder->m_uiDeactivated >= 1, "the compound trigger reports the body leaving");
  Check(test.GetPosition(pMover->GetHandle()).x > 2.0f, "the body crossed the compound trigger");

  OpenDefect("PH-B14", pRecorder->m_uiActivated == 1, "a body in two sub-shapes enters the compound trigger once");
  OpenDefect("PH-B14", pRecorder->m_uiDeactivated == 1, "a body in two sub-shapes leaves the compound trigger once");
}

PL_CREATE_SIMPLE_TEST_GROUP(JoltTriggers);

PL_CREATE_SIMPLE_TEST(JoltTriggers, TriggerEnterAndLeave)
{
  TestTriggerEnterAndLeave();
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, TriggerIgnoresBodiesOutsideOfIt)
{
  TestTriggerIgnoresBodiesOutsideOfIt();
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, CompoundTrigger)
{
  TestCompoundTrigger();
}

static plGameObject* CreateChild(World& ref_test, plGameObject* pParent, const plVec3& vLocalPosition)
{
  PL_LOCK(ref_test.GetWorld().GetWriteMarker());

  plGameObjectDesc desc;
  desc.m_bDynamic = true;
  desc.m_hParent = pParent->GetHandle();
  desc.m_LocalPosition = vLocalPosition;

  plGameObject* pChild = nullptr;
  ref_test.GetWorld().CreateObject(desc, pChild);
  return pChild;
}

/// A trigger from one box shape, with a recorder.
static plGameObject* AddBoxTrigger(World& ref_test, const plVec3& vPosition, const plVec3& vHalfExtents, plUInt8 uiLayer = 0)
{
  plGameObject* pTrigger = ref_test.CreateObject(vPosition);
  AddBoxShape(ref_test, pTrigger, vHalfExtents);
  MakeTrigger(ref_test, pTrigger, uiLayer);
  return pTrigger;
}

/// A dynamic body without gravity that is made of two box shapes at +-0.3 m along its x axis.
static plGameObject* AddCompoundMover(World& ref_test, const plVec3& vPosition, plUInt8 uiLayer = 0)
{
  plGameObject* pBody = ref_test.CreateObject(vPosition);

  {
    PL_LOCK(ref_test.GetWorld().GetWriteMarker());
    plJoltDynamicActorComponent* pActor = nullptr;
    plJoltDynamicActorComponent::CreateComponent(pBody, pActor);
    pActor->m_uiCollisionLayer = uiLayer;
    pActor->SetGravityFactor(0.0f);
  }

  for (const float fOffset : {-0.3f, 0.3f})
    AddBoxShape(ref_test, CreateChild(ref_test, pBody, plVec3(fOffset, 0, 0)), plVec3(0.2f));

  return pBody;
}

/// Lets a body fly along +x through the middle of the trigger and past it.
static void FlyThrough(World& ref_test, plGameObject* pMover)
{
  ref_test.Step(3);
  ref_test.SetVelocity(pMover, plVec3(3.0f, 0, 0));
  ref_test.StepSeconds(3.0f);
}

/// A body made of two shapes crosses a single-shape trigger: one enter and one final exit.
static void TestCompoundBodyEntersOnce()
{
  World test;
  plGameObject* pTrigger = AddBoxTrigger(test, plVec3::MakeZero(), plVec3(0.5f));

  plGameObject* pMover = AddCompoundMover(test, plVec3(-3.0f, 0, 0));
  FlyThrough(test, pMover);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr, "the trigger has a recorder");
  if (pRecorder == nullptr)
    return;

  Check(pRecorder->m_uiActivated >= 1, "the trigger reports the compound body entering");
  Check(pRecorder->m_uiDeactivated >= 1, "the trigger reports the compound body leaving");
  Check(test.GetPosition(pMover->GetHandle()).x > 2.0f, "the compound body crossed the trigger");

  OpenDefect("PH-B14", pRecorder->m_uiActivated == 1, "a body of two shapes enters a trigger once");
  OpenDefect("PH-B14", pRecorder->m_uiDeactivated == 1, "a body of two shapes leaves a trigger once");
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, CompoundBodyEntersOnce)
{
  TestCompoundBodyEntersOnce();
}

/// A body that leaves one shape of a compound trigger but is still inside the other has not left the trigger.
static void TestPartialDepartureKeepsInside()
{
  World test;

  plGameObject* pTrigger = test.CreateObject(plVec3::MakeZero());
  MakeTrigger(test, pTrigger);
  for (const float fOffset : {-0.5f, 0.5f})
    AddBoxShape(test, CreateChild(test, pTrigger, plVec3(fOffset, 0, 0)), plVec3(0.5f));

  // starts inside the first shape only, ends inside the second shape only
  plGameObject* pMover = test.AddDynamicBox(plVec3(-0.5f, 0, 0), plVec3(0.1f), 0, 0.0f);
  test.Step(3);
  test.SetVelocity(pMover, plVec3(1.1f, 0, 0));
  test.StepSeconds(1.0f);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr, "the compound trigger has a recorder");
  if (pRecorder == nullptr)
    return;

  const float fX = test.GetPosition(pMover->GetHandle()).x;
  Check(fX > 0.3f && fX < 0.9f, "the body moved into the second shape of the trigger");
  Check(pRecorder->m_uiActivated >= 1, "the body entered the trigger");

  OpenDefect("PH-B14", pRecorder->m_uiDeactivated == 0, "leaving one shape of a compound trigger does not leave the trigger");
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, PartialDepartureKeepsInside)
{
  TestPartialDepartureKeepsInside();
}

/// Two bodies enter, one leaves: two enters, one leave.
static void TestTwoBodiesCountedSeparately()
{
  World test;
  plGameObject* pTrigger = AddBoxTrigger(test, plVec3::MakeZero(), plVec3(1.0f, 1.0f, 0.5f));

  test.AddDynamicBox(plVec3(0, 0.6f, 0), plVec3(0.2f), 0, 0.0f);
  plGameObject* pPasser = test.AddDynamicBox(plVec3(-3.0f, -0.5f, 0), plVec3(0.2f), 0, 0.0f);
  FlyThrough(test, pPasser);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr, "the trigger has a recorder");
  if (pRecorder == nullptr)
    return;

  Check(pRecorder->m_uiActivated == 2, "each of the two bodies is reported entering");
  Check(pRecorder->m_uiDeactivated == 1, "only the body that left is reported leaving");
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, TwoBodiesCountedSeparately)
{
  TestTwoBodiesCountedSeparately();
}

/// Deleting a body that is inside a trigger leaves the trigger usable for the next body.
static void TestBodyDeletedInside()
{
  World test;
  plGameObject* pTrigger = AddBoxTrigger(test, plVec3::MakeZero(), plVec3(1.0f, 1.0f, 0.5f));

  const plGameObjectHandle hInside = test.AddDynamicBox(plVec3(0, 0.6f, 0), plVec3(0.2f), 0, 0.0f)->GetHandle();
  test.Step(10);

  const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
  Check(pRecorder != nullptr && pRecorder->m_uiActivated == 1, "the body inside the trigger was reported");
  if (pRecorder == nullptr)
    return;

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    test.GetWorld().DeleteObjectNow(hInside);
  }

  test.Step(10);
  plTestFramework::Output(plTestOutput::Message, "after deleting the body inside the trigger: %u leave message(s) (not asserted)", pRecorder->m_uiDeactivated);

  FlyThrough(test, test.AddDynamicBox(plVec3(-3.0f, -0.5f, 0), plVec3(0.2f), 0, 0.0f));
  Check(pRecorder->m_uiActivated == 2, "a body entering after the deletion is reported");
}

PL_CREATE_SIMPLE_TEST(JoltTriggers, BodyDeletedInside)
{
  TestBodyDeletedInside();
}

/// A trigger on a layer that does not collide with the body's layer never fires. Allowing the pair makes it fire.
static void TestTriggerRespectsLayers()
{
  LayerConfig layers;

  {
    World test;
    plGameObject* pTrigger = AddBoxTrigger(test, plVec3::MakeZero(), plVec3(1.0f, 1.0f, 0.5f), 1);
    FlyThrough(test, test.AddDynamicBox(plVec3(-3.0f, 0, 0), plVec3(0.2f), 2, 0.0f));

    const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
    Check(pRecorder != nullptr && pRecorder->m_uiActivated == 0, "a trigger does not see a body on a layer it does not collide with");
  }

  layers.Allow(1, 2, true);

  {
    World test;
    plGameObject* pTrigger = AddBoxTrigger(test, plVec3::MakeZero(), plVec3(1.0f, 1.0f, 0.5f), 1);
    FlyThrough(test, test.AddDynamicBox(plVec3(-3.0f, 0, 0), plVec3(0.2f), 2, 0.0f));

    const TriggerRecorderComponent* pRecorder = GetRecorder(test, pTrigger);
    Check(pRecorder != nullptr && pRecorder->m_uiActivated == 1, "a trigger sees a body once the layers collide");
  }
}

PL_CREATE_SIMPLE_TEST(JoltLayers, TriggerRespectsLayers)
{
  TestTriggerRespectsLayers();
}

/// Queries skip triggers unless the Trigger shape type is asked for.
static void TestTriggersAndQueryShapeTypes()
{
  World test;
  AddBoxTrigger(test, plVec3::MakeZero(), plVec3(1.0f));
  test.Step(3);

  plPhysicsCastResult result;
  plBitflags<plPhysicsShapeType> solid = plPhysicsShapeType::Static | plPhysicsShapeType::Dynamic;
  Check(!test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, plPhysicsQueryParameters(0, solid)), "a ray for static and dynamic shapes passes through a trigger");
  Check(test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, plPhysicsQueryParameters(0, plPhysicsShapeType::Trigger)), "a ray for trigger shapes hits the trigger");
}

PL_CREATE_SIMPLE_TEST(JoltQueries, TriggersAndQueryShapeTypes)
{
  TestTriggersAndQueryShapeTypes();
}
