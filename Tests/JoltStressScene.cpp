#include "JoltTestSupport.h"
#include <Foundation/Time/Stopwatch.h>
#include <JoltPlugin/Character/JoltDefaultCharacterComponent.h>
#include <JoltPlugin/Components/JoltRagdollComponent.h>
#include <JoltPlugin/Constraints/JoltFixedConstraintComponent.h>

using namespace JoltTest;

/// \brief The soak scene behind the R0 gate. It runs against the Debug allocator and asserts so that corrupt state is caught early.
///
/// Every frame it spawns and deletes dynamic bodies, hangs boxes from breakable joints, creates and deletes ragdoll components,
/// keeps a character walking, and sweeps box overlaps and ray casts over a 200 m square the way the AI voxelizer does.
/// Glass shatters, destructible contact damage and real ragdoll skeletons need mesh and skeleton assets, which a headless
/// package test does not have; those parts of the gate are run from a project scene.
class StressScene
{
public:
  StressScene()
  {
    m_World.AddFloor();
    m_World.AddStaticBox(plVec3(0, 0, 1.0f), plVec3(100.0f, 0.5f, 1.0f));

    m_pWalkerObject = m_World.CreateObject(plVec3(-90.0f, 5.0f, 0.02f));
    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    plJoltDefaultCharacterComponent::CreateComponent(m_pWalkerObject, m_pWalker);
  }

  void Frame()
  {
    SpawnAndDeleteBodies();
    ChurnJoints();
    ChurnRagdolls();
    DriveCharacter();
    VoxelizeSlice();

    m_World.Step(1);
    ++m_uiFrame;
  }

  plUInt64 GetFrame() const { return m_uiFrame; }

private:
  void SpawnAndDeleteBodies()
  {
    for (plUInt32 i = 0; i < 3 && m_Bodies.GetCount() < 300; ++i)
    {
      const float x = Random(-40.0f, 40.0f);
      const float y = Random(-40.0f, 40.0f);
      m_Bodies.PushBack(m_World.AddDynamicBox(plVec3(x, y, Random(3.0f, 12.0f)), plVec3(0.3f, 0.3f, 0.3f))->GetHandle());
    }

    while (m_Bodies.GetCount() >= 300)
    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      m_World.GetWorld().DeleteObjectNow(m_Bodies[m_uiNextDelete % m_Bodies.GetCount()]);
      m_Bodies.RemoveAtAndSwap(m_uiNextDelete % m_Bodies.GetCount());
      m_uiNextDelete += 7;
    }
  }

  /// Boxes that hang from a kinematic anchor on a joint that breaks under their weight, and joints that are deleted with their bodies.
  void ChurnJoints()
  {
    JoinPendingJoints();

    if (m_uiFrame % 45 != 0)
      return;

    if (m_Joints.GetCount() >= 12)
    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      m_World.GetWorld().DeleteObjectNow(m_Joints[0].m_hAnchor);
      m_World.GetWorld().DeleteObjectNow(m_Joints[0].m_hBox);
      if (!m_Joints[0].m_hJoint.IsInvalidated())
        m_World.GetWorld().DeleteObjectNow(m_Joints[0].m_hJoint);
      m_Joints.RemoveAtAndCopy(0);
    }

    const float x = Random(-40.0f, 40.0f);
    plGameObject* pAnchor = m_World.AddDynamicBox(plVec3(x, 20.0f, 10.0f), plVec3(0.5f));
    plGameObject* pBox = m_World.AddDynamicBox(plVec3(x, 20.0f, 8.0f), plVec3(0.5f));

    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      plJoltDynamicActorComponent* pActor = nullptr;
      pAnchor->TryGetComponentOfBaseType(pActor);
      pActor->SetKinematic(true);
    }

    JointSet set;
    set.m_hAnchor = pAnchor->GetHandle();
    set.m_hBox = pBox->GetHandle();
    set.m_uiCreatedFrame = m_uiFrame;
    m_Joints.PushBack(set);
  }

  /// The joint can only be made once the bodies of both actors exist.
  void JoinPendingJoints()
  {
    for (JointSet& set : m_Joints)
    {
      if (set.m_hJoint.IsInvalidated() == false || m_uiFrame < set.m_uiCreatedFrame + 3)
        continue;

      PL_LOCK(m_World.GetWorld().GetWriteMarker());

      plGameObjectDesc desc;
      desc.m_bDynamic = true;
      plGameObject* pJoint = nullptr;
      set.m_hJoint = m_World.GetWorld().CreateObject(desc, pJoint);

      plJoltFixedConstraintComponent* pConstraint = nullptr;
      plJoltFixedConstraintComponent::CreateComponent(pJoint, pConstraint);
      pConstraint->SetBreakForce(Random(20.0f, 400.0f));
      pConstraint->SetActors(set.m_hAnchor, plTransform(plVec3(0, 0, -1.0f)), set.m_hBox, plTransform(plVec3(0, 0, 1.0f)));
    }
  }

  void ChurnRagdolls()
  {
    if (m_uiFrame % 30 != 0)
      return;

    if (m_Ragdolls.GetCount() >= 16)
    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      m_World.GetWorld().DeleteObjectNow(m_Ragdolls[0]);
      m_Ragdolls.RemoveAtAndCopy(0);
    }

    plGameObject* pObject = m_World.CreateObject(plVec3(Random(-30.0f, 30.0f), Random(-30.0f, 30.0f), 2.0f));
    m_Ragdolls.PushBack(pObject->GetHandle());

    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    plJoltRagdollComponent* pRagdoll = nullptr;
    plJoltRagdollComponent::CreateComponent(pObject, pRagdoll);
  }

  void DriveCharacter()
  {
    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    m_pWalker->Move(1.0f, 0.0f);

    if (m_pWalkerObject->GetGlobalPosition().x > 90.0f)
      m_pWalker->TeleportCharacter(plVec3(-90.0f, 5.0f, 0.02f));
  }

  /// Checks one column of the 200 m square per frame: a box overlap for every cell and a ray for every row.
  void VoxelizeSlice()
  {
    const float x = -100.0f + static_cast<float>(m_uiFrame % 200) + 0.5f;

    for (plUInt32 uiRow = 0; uiRow < 200; ++uiRow)
    {
      const float y = -100.0f + static_cast<float>(uiRow) + 0.5f;
      const plTransform cell = plTransform(plVec3(x, y, 1.0f));

      if (m_World.GetJolt().OverlapTestBox(plVec3(1.0f), cell.m_vPosition, cell, AllShapes()))
        ++m_uiBlockedCells;
    }

    plPhysicsCastResultArray hits;
    m_World.GetJolt().RaycastAll(hits, plVec3(x, -100.0f, 1.0f), plVec3(0, 1, 0), 200.0f, AllShapes());
    m_uiRayHits += hits.m_Results.GetCount();
  }

  float Random(float fMin, float fMax)
  {
    m_uiSeed = m_uiSeed * 1664525u + 1013904223u;
    return fMin + (fMax - fMin) * static_cast<float>(m_uiSeed >> 8) / static_cast<float>(1u << 24);
  }

  World m_World;
  plGameObject* m_pWalkerObject = nullptr;
  plJoltDefaultCharacterComponent* m_pWalker = nullptr;
  plHybridArray<plGameObjectHandle, 320> m_Bodies;
  struct JointSet
  {
    plGameObjectHandle m_hAnchor;
    plGameObjectHandle m_hBox;
    plGameObjectHandle m_hJoint;
    plUInt64 m_uiCreatedFrame = 0;
  };

  plDynamicArray<JointSet> m_Joints;
  plHybridArray<plGameObjectHandle, 16> m_Ragdolls;
  plUInt32 m_uiNextDelete = 0;
  plUInt32 m_uiSeed = 99;
  plUInt64 m_uiFrame = 0;
  plUInt64 m_uiBlockedCells = 0;
  plUInt64 m_uiRayHits = 0;
};

plUInt64 RunStressScene(double fSeconds)
{
  BeginGroup("Stress");
  std::printf("  running the soak scene for %.0f s\n", fSeconds);

  StressScene scene;
  plStopwatch watch;

  while (watch.GetRunningTotal().GetSeconds() < fSeconds)
    scene.Frame();

  Check(true, "the soak scene finished without an assert or crash");
  return scene.GetFrame();
}
