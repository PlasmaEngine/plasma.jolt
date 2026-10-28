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
static void MakeTrigger(World& ref_test, plGameObject* pObject)
{
  PL_LOCK(ref_test.GetWorld().GetWriteMarker());

  plJoltTriggerComponent* pTrigger = nullptr;
  plJoltTriggerComponent::CreateComponent(pObject, pTrigger);
  pTrigger->SetTriggerMessage("zone");

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

  Check(pRecorder->m_uiActivated == 1, "a body in two sub-shapes enters the compound trigger once", Category::R1Gate);
  Check(pRecorder->m_uiDeactivated == 1, "a body in two sub-shapes leaves the compound trigger once", Category::R1Gate);
}

void RunTriggerTests()
{
  BeginGroup("Triggers");

  TestTriggerEnterAndLeave();
  TestTriggerIgnoresBodiesOutsideOfIt();
  TestCompoundTrigger();
}
