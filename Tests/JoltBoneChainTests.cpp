#include "JoltTestSupport.h"
#include <Core/ResourceManager/ResourceManager.h>
#include <JoltPlugin/CharacterCloth/JoltBoneChainComponent.h>
#include <JoltPlugin/CharacterCloth/JoltBoneChainSolver.h>
#include <RendererCore/AnimationSystem/SkeletonBuilder.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>

using namespace JoltTest;

namespace
{
  constexpr plUInt32 s_uiChainJoints = 5;
  constexpr float s_fBoneLength = 0.1f;

  /// A model pose with a chain of five joints that sticks out along +X at a height of 1.5 m, and one more joint
  /// that hangs off the middle of the chain.
  void MakePose(plDynamicArray<plMat4>& out_pose)
  {
    out_pose.Clear();

    for (plUInt32 j = 0; j < s_uiChainJoints; ++j)
    {
      out_pose.PushBack(plMat4::MakeTranslation(plVec3(j * s_fBoneLength, 0, 1.5f)));
    }

    out_pose.PushBack(plMat4::MakeTranslation(plVec3(2 * s_fBoneLength, 0.05f, 1.5f)));
  }

  void SetUpChain(plJoltBoneChainSolver& ref_solver)
  {
    const plUInt16 joints[s_uiChainJoints] = {0, 1, 2, 3, 4};

    plJoltBoneChainSolver::Follower follower;
    follower.m_uiJoint = 5;
    follower.m_uiChainIndex = 2;

    ref_solver.SetChain(plMakeArrayPtr(joints), plMakeArrayPtr(&follower, 1));
  }

  bool BonesKeepLength(plArrayPtr<const plVec3> points)
  {
    for (plUInt32 i = 1; i < points.GetCount(); ++i)
    {
      if (!points[i].IsValid() || plMath::Abs((points[i] - points[i - 1]).GetLength() - s_fBoneLength) > 1.0e-3f)
        return false;
    }

    return true;
  }
} // namespace

/// Without stiffness a chain falls and hangs from its first joint; its bones keep their length.
static void TestChainHangs()
{
  plJoltBoneChainSolver solver;
  SetUpChain(solver);

  plJoltBoneChainSettings settings;
  settings.m_fStiffness = 0.0f;
  settings.m_MaxAngle = plAngle::MakeFromDegree(180);

  plJoltBoneChainSolver::StepInput input;
  plDynamicArray<plMat4> pose;

  bool bLengths = true;
  for (plUInt32 uiFrame = 0; uiFrame < 240; ++uiFrame)
  {
    MakePose(pose);
    solver.Step(settings, input, pose);
    bLengths = bLengths && BonesKeepLength(solver.GetPoints());
  }

  const plArrayPtr<const plVec3> points = solver.GetPoints();
  Check(points.GetCount() == s_uiChainJoints + 1, "a chain has a point per joint and one for the end of its last bone");
  Check(bLengths, "the bones keep their length while the chain falls");
  CheckNear(points[0].z, 1.5f, 1.0e-5f, "the first joint stays where the animation has it");
  Check(points[s_uiChainJoints].z < 1.5f - 0.45f, "without stiffness the chain hangs straight down");

  // the pose follows the points, and the joint that hangs off the chain is carried along rigidly
  CheckNear((pose[3].GetTranslationVector() - points[3]).GetLength(), 0.0f, 1.0e-4f, "the joints of the pose are where the chain's points are");
  CheckNear((pose[5].GetTranslationVector() - pose[2].GetTranslationVector()).GetLength(), 0.05f, 1.0e-4f, "a joint that hangs off the chain keeps its distance to it");

  // the joint has turned with its bone: its X axis, which pointed along the chain, now points down
  const plVec3 vAxis = pose[2].TransformDirection(plVec3(1, 0, 0));
  Check(vAxis.z < -0.9f, "a joint turns the way its bone turned");
}

/// With full stiffness the chain holds the animated pose; the max angle limits how far it bends.
static void TestChainStiffnessAndAngle()
{
  plJoltBoneChainSettings settings;
  plJoltBoneChainSolver::StepInput input;
  plDynamicArray<plMat4> pose;

  {
    plJoltBoneChainSolver solver;
    SetUpChain(solver);
    settings.m_fStiffness = 1.0f;

    for (plUInt32 uiFrame = 0; uiFrame < 120; ++uiFrame)
    {
      MakePose(pose);
      solver.Step(settings, input, pose);
    }

    CheckNear(solver.GetPoints()[s_uiChainJoints].z, 1.5f, 2.0e-3f, "with full stiffness the chain holds the animated pose");
  }

  {
    plJoltBoneChainSolver solver;
    SetUpChain(solver);
    settings.m_fStiffness = 0.0f;
    settings.m_MaxAngle = plAngle::MakeFromDegree(30);

    for (plUInt32 uiFrame = 0; uiFrame < 240; ++uiFrame)
    {
      MakePose(pose);
      solver.Step(settings, input, pose);
    }

    const plArrayPtr<const plVec3> points = solver.GetPoints();

    bool bWithinAngle = true;
    for (plUInt32 i = 1; i < points.GetCount(); ++i)
    {
      const plVec3 vDirection = (points[i] - points[i - 1]).GetNormalized();
      if (plMath::ACos(plMath::Clamp(vDirection.Dot(plVec3(1, 0, 0)), -1.0f, 1.0f)) > plAngle::MakeFromDegree(30.5f))
        bWithinAngle = false;
    }

    Check(bWithinAngle, "no bone bends further from its animated direction than the max angle");
    Check(BonesKeepLength(points), "the bones keep their length at the angle limit");
  }
}

/// The chain lags when its character speeds up, catches up at a steady speed, and starts over after a teleport.
static void TestChainMotion()
{
  plJoltBoneChainSolver solver;
  SetUpChain(solver);

  plJoltBoneChainSettings settings;
  settings.m_fStiffness = 0.2f;
  settings.m_fDrag = 0.0f;

  plJoltBoneChainSolver::StepInput input;
  plDynamicArray<plMat4> pose;

  for (plUInt32 uiFrame = 0; uiFrame < 120; ++uiFrame)
  {
    MakePose(pose);
    solver.Step(settings, input, pose);
  }

  const float fRestOffset = solver.GetPoints()[s_uiChainJoints].y - solver.GetPoints()[0].y;

  // off to the side at 6 m/s
  float fLargestLag = 0.0f;
  plVec3 vPosition = plVec3::MakeZero();

  for (plUInt32 uiFrame = 0; uiFrame < 300; ++uiFrame)
  {
    vPosition.y += 6.0f / 60.0f;
    input.m_mModelToWorld = plMat4::MakeTranslation(vPosition);

    MakePose(pose);
    solver.Step(settings, input, pose);

    fLargestLag = plMath::Max(fLargestLag, fRestOffset - (solver.GetPoints()[s_uiChainJoints].y - solver.GetPoints()[0].y));
  }

  const float fSteadyLag = fRestOffset - (solver.GetPoints()[s_uiChainJoints].y - solver.GetPoints()[0].y);

  Check(fLargestLag > 0.05f, "the chain is left behind when the character speeds up");
  Check(plMath::Abs(fSteadyLag) < 0.03f, "at a steady speed the chain has caught up");
  Check(BonesKeepLength(solver.GetPoints()), "the bones keep their length while the character moves");

  // a teleport starts the chain over from the pose instead of dragging it across the level
  vPosition.y += 50.0f;
  input.m_mModelToWorld = plMat4::MakeTranslation(vPosition);

  MakePose(pose);
  solver.Step(settings, input, pose);

  const plArrayPtr<const plVec3> points = solver.GetPoints();
  CheckNear(points[s_uiChainJoints].y - points[0].y, 0.0f, 0.01f, "after a teleport the chain is back on the pose");
  Check(BonesKeepLength(points), "nothing stretches across a teleport");
}

/// The chain stays out of the body's shapes.
static void TestChainCollides()
{
  plJoltBoneChainSolver solver;
  SetUpChain(solver);

  plJoltBoneChainSettings settings;
  settings.m_fStiffness = 0.0f;
  settings.m_fRadius = 0.02f;
  settings.m_MaxAngle = plAngle::MakeFromDegree(180);

  // a ball right below the first joint, where the chain would hang if nothing were in the way
  plJoltClothCollider ball;
  ball.m_Type = plJoltClothCollider::Type::Sphere;
  ball.m_fRadius = 0.12f;

  const plTransform ballJoint(plVec3(0, 0, 1.25f));

  plJoltBoneChainSolver::StepInput input;
  input.m_Shapes = plMakeArrayPtr(&ball, 1);
  input.m_ShapeJoints = plMakeArrayPtr(&ballJoint, 1);

  plDynamicArray<plMat4> pose;
  float fClosest = plMath::MaxValue<float>();

  for (plUInt32 uiFrame = 0; uiFrame < 240; ++uiFrame)
  {
    MakePose(pose);
    solver.Step(settings, input, pose);

    for (plUInt32 i = 1; i <= s_uiChainJoints; ++i)
    {
      fClosest = plMath::Min(fClosest, (solver.GetPoints()[i] - ballJoint.m_vPosition).GetLength());
    }
  }

  Check(fClosest > 0.12f + 0.02f - 0.01f, "the chain's joints stay out of the body's shapes");
  Check(solver.GetPoints()[2].x > 0.05f, "the chain comes to rest against the side of the ball instead of hanging through it");
}

/// The component finds its chains in the skeleton, simulates them on the pose it is sent and sends the result on.
static void TestBoneChainComponent()
{
  World test;

  plSkeletonResourceDescriptor skeleton;
  plDynamicArray<plMat4> restPose;

  {
    plSkeletonBuilder builder;
    // a tail of two joints behind the hips, and a bell that hangs off its first joint
    const plUInt16 uiHips = builder.AddJoint("Hips", plTransform(plVec3(0, 0, 1.0f)));
    const plUInt16 uiTail1 = builder.AddJoint("Tail1", plTransform(plVec3(-0.1f, 0, 0)), uiHips);
    builder.AddJoint("Tail2", plTransform(plVec3(-0.1f, 0, 0)), uiTail1);
    builder.AddJoint("Bell", plTransform(plVec3(0, 0.05f, 0)), uiTail1);
    builder.BuildSkeleton(skeleton.m_Skeleton);
    skeleton.m_Skeleton.m_BoneDirection = plBasisAxis::PositiveX;

    restPose.PushBack(plMat4::MakeTranslation(plVec3(0, 0, 1.0f)));
    restPose.PushBack(plMat4::MakeTranslation(plVec3(-0.1f, 0, 1.0f)));
    restPose.PushBack(plMat4::MakeTranslation(plVec3(-0.2f, 0, 1.0f)));
    restPose.PushBack(plMat4::MakeTranslation(plVec3(-0.1f, 0.05f, 1.0f)));
  }

  const plTransform rootTransform = skeleton.m_RootTransform;
  const plSkeletonResourceHandle hSkeleton = plResourceManager::CreateResource<plSkeletonResource>("JoltTests_TailSkeleton", std::move(skeleton));

  plGameObject* pObject = test.CreateObject(plVec3::MakeZero());
  plComponentHandle hChain;

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltBoneChainComponent* pChain = nullptr;
    hChain = plJoltBoneChainComponent::CreateComponent(pObject, pChain);
    pChain->m_Chains.PushBack("Tail1");
    pChain->m_Chains.PushBack("NoSuchJoint");
    pChain->m_fStiffness = 0.0f;
    pChain->m_MaxAngle = plAngle::MakeFromDegree(180);
    pChain->SetSkeleton(hSkeleton);
  }

  // the animation sends the rest pose every frame
  for (plUInt32 uiFrame = 0; uiFrame < 120; ++uiFrame)
  {
    {
      PL_LOCK(test.GetWorld().GetWriteMarker());

      plResourceLock<plSkeletonResource> pSkeleton(hSkeleton, plResourceAcquireMode::BlockTillLoaded);

      plMsgAnimationPoseUpdated msg;
      msg.m_pRootTransform = &rootTransform;
      msg.m_pSkeleton = &pSkeleton->GetDescriptor().m_Skeleton;
      msg.m_ModelTransforms = restPose;
      pObject->SendMessage(msg);
    }

    test.Step(1);
  }

  PL_LOCK(test.GetWorld().GetWriteMarker());

  plJoltBoneChainComponent* pChain = nullptr;
  Check(test.GetWorld().TryGetComponent(hChain, pChain), "the bone chain component is there");

  if (pChain == nullptr)
    return;

  Check(pChain->GetChainCount() == 1, "the chain whose joint the skeleton has is built, the other one is left out");

  const plArrayPtr<const plVec3> points = pChain->GetChainPoints(0);
  Check(points.GetCount() == 3, "the tail runs from its first joint along the first child to the last joint");

  if (points.GetCount() == 3)
  {
    CheckNear((points[0] - plVec3(-0.1f, 0, 1.0f)).GetLength(), 0.0f, 1.0e-4f, "the tail starts where the animation has its first joint");
    Check(points[2].z < 0.85f, "the tail hangs down under gravity");
  }
}

void RunBoneChainTests()
{
  BeginGroup("bonechain");

  TestChainHangs();
  TestChainStiffnessAndAngle();
  TestChainMotion();
  TestChainCollides();
  TestBoneChainComponent();
}
