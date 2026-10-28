#include "JoltTestSupport.h"
#include <JoltPlugin/Constraints/JoltFixedConstraintComponent.h>

using namespace JoltTest;

/// A box that hangs from a kinematic anchor on a fixed constraint with a configurable break threshold.
class HangingBox
{
public:
  explicit HangingBox(float fBreakForce)
  {
    m_pAnchor = m_World.AddDynamicBox(plVec3(0, 0, 10.0f), plVec3(0.5f));
    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      plJoltDynamicActorComponent* pActor = nullptr;
      m_pAnchor->TryGetComponentOfBaseType(pActor);
      pActor->SetKinematic(true);
    }

    m_pBox = m_World.AddDynamicBox(plVec3(0, 0, 8.0f), plVec3(0.5f));
    m_World.Step(3);

    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    plGameObjectDesc desc;
    plGameObject* pOwner = nullptr;
    m_World.GetWorld().CreateObject(desc, pOwner);

    plJoltFixedConstraintComponent::CreateComponent(pOwner, m_pConstraint);
    m_pConstraint->SetBreakForce(fBreakForce);
    m_pConstraint->SetActors(m_pAnchor->GetHandle(), plTransform(plVec3(0, 0, -1.0f)), m_pBox->GetHandle(), plTransform(plVec3(0, 0, 1.0f)));
  }

  float BoxHeight() { return m_World.GetPosition(m_pBox->GetHandle()).z; }

  World m_World;
  plGameObject* m_pAnchor = nullptr;
  plGameObject* m_pBox = nullptr;
  plJoltFixedConstraintComponent* m_pConstraint = nullptr;
};

/// A joint with a break force far above the load keeps holding.
static void TestConstraintHoldsBelowThreshold()
{
  HangingBox scene(100000.0f);
  scene.m_World.StepSeconds(3.0f);

  CheckNear(scene.BoxHeight(), 8.0f, 0.15f, "the hanging box stays at the anchor");
}

/// A joint without a break force never breaks.
static void TestConstraintWithoutBreakForceHolds()
{
  HangingBox scene(0.0f);
  scene.m_World.StepSeconds(3.0f);

  CheckNear(scene.BoxHeight(), 8.0f, 0.15f, "a constraint without break force holds");
}

/// A joint whose break force is below the weight of the box lets go.
static void TestConstraintBreaksAboveThreshold()
{
  HangingBox scene(5.0f);
  scene.m_World.StepSeconds(3.0f);

  Check(scene.BoxHeight() < 6.0f, "the box falls once the joint exceeds its break force");
}

/// Breaking a constraint through the API releases the box and stepping afterwards stays stable.
static void TestExplicitBreak()
{
  HangingBox scene(0.0f);
  scene.m_World.StepSeconds(1.0f);

  {
    PL_LOCK(scene.m_World.GetWorld().GetWriteMarker());
    scene.m_pConstraint->BreakConstraint();
    scene.m_pConstraint->BreakConstraint();
  }

  scene.m_World.StepSeconds(2.0f);
  Check(scene.BoxHeight() < 6.0f, "the box falls after BreakConstraint()");
}

/// Deleting a body that is joined to a constraint does not leave the constraint behind.
static void TestDeletingJoinedBodyKeepsWorldStable()
{
  HangingBox scene(0.0f);
  scene.m_World.StepSeconds(1.0f);

  {
    PL_LOCK(scene.m_World.GetWorld().GetWriteMarker());
    scene.m_World.GetWorld().DeleteObjectNow(scene.m_pBox->GetHandle());
  }

  scene.m_World.StepSeconds(2.0f);
  Check(scene.m_World.GetPosition(scene.m_pAnchor->GetHandle()).z > 9.0f, "the anchor is unaffected when the joined body is deleted");
}

void RunBreakTests()
{
  BeginGroup("Breaks");

  TestConstraintHoldsBelowThreshold();
  TestConstraintWithoutBreakForceHolds();
  TestConstraintBreaksAboveThreshold();
  TestExplicitBreak();
  TestDeletingJoinedBodyKeepsWorldStable();
}
