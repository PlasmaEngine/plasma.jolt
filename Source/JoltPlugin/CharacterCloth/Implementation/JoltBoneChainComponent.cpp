#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Interfaces/WindWorldModule.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <JoltPlugin/CharacterCloth/JoltBoneChainComponent.h>
#include <JoltPlugin/CharacterCloth/JoltCharacterClothComponent.h>
#include <JoltPlugin/CharacterCloth/JoltClothBuilder.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
plCVarBool cvar_JoltBoneChainVisualize("Jolt.BoneChain.Visualize", false, plCVarFlags::Default, "Draws the simulated bone chains.");

PL_BEGIN_COMPONENT_TYPE(plJoltBoneChainComponent, 2, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Chains", m_Chains),
    PL_MEMBER_PROPERTY("Stiffness", m_fStiffness)->AddAttributes(new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Damping", m_fDamping)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("LinearInertia", m_fLinearInertia)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("WindInfluence", m_fWindInfluence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("MaxAngle", m_MaxAngle)->AddAttributes(new plDefaultValueAttribute(plAngle::MakeFromDegree(60)), new plClampValueAttribute(plAngle::MakeFromDegree(0), plAngle::MakeFromDegree(180))),
    PL_MEMBER_PROPERTY("CollideWithBody", m_bCollideWithBody)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.03f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("TeleportDistance", m_fTeleportDistance)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.1f, 1000.0f)),
    PL_MEMBER_PROPERTY("StandInForCloth", m_bStandInForCloth),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgAnimationPoseUpdated, OnAnimationPoseUpdated),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Reset),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Animation"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  constexpr float s_fLongestChainTimeStep = 1.0f / 15.0f;
} // namespace

//////////////////////////////////////////////////////////////////////////

plJoltBoneChainComponentManager::plJoltBoneChainComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

void plJoltBoneChainComponentManager::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltBoneChainComponentManager::Update, this);
  desc.m_Phase = plWorldUpdatePhase::PostAsync;
  desc.m_bOnlyUpdateWhenSimulating = true;
  desc.m_fPriority = -500.0f; // after animation has set the pose of the frame, before character cloth reads it

  RegisterUpdateFunction(desc);
}

void plJoltBoneChainComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  const plTime timeStep = GetWorld()->GetClock().GetTimeDiff();

  for (auto it = GetComponents(); it.IsValid(); ++it)
  {
    if (it->IsActiveAndSimulating())
      it->Update(timeStep);
  }
}

//////////////////////////////////////////////////////////////////////////

plJoltBoneChainComponent::plJoltBoneChainComponent() = default;
plJoltBoneChainComponent::~plJoltBoneChainComponent() = default;

void plJoltBoneChainComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s.WriteArray(m_Chains).AssertSuccess();
  s << m_fStiffness;
  s << m_fDamping;
  s << m_fGravityFactor;
  s << m_fLinearInertia;
  s << m_fWindInfluence;
  s << m_fRadius;
  s << m_MaxAngle;
  s << m_bCollideWithBody;
  s << m_fTeleportDistance;
  s << m_bStandInForCloth;
}

void plJoltBoneChainComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s.ReadArray(m_Chains).AssertSuccess();
  s >> m_fStiffness;
  s >> m_fDamping;
  s >> m_fGravityFactor;
  s >> m_fLinearInertia;
  s >> m_fWindInfluence;
  s >> m_fRadius;
  s >> m_MaxAngle;
  s >> m_bCollideWithBody;
  s >> m_fTeleportDistance;

  if (inout_stream.GetComponentTypeVersion(GetStaticRTTI()) >= 2)
    s >> m_bStandInForCloth;
}

void plJoltBoneChainComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  m_bChainsBuilt = false;
  m_bResetPending = true;
}

void plJoltBoneChainComponent::Reset()
{
  m_bResetPending = true;
}

void plJoltBoneChainComponent::SetSkeleton(const plSkeletonResourceHandle& hSkeleton)
{
  m_hSkeleton = hSkeleton;
  m_bChainsBuilt = false;
}

plArrayPtr<const plVec3> plJoltBoneChainComponent::GetChainPoints(plUInt32 uiChain) const
{
  return uiChain < m_Solvers.GetCount() ? m_Solvers[uiChain].GetPoints() : plArrayPtr<const plVec3>();
}

void plJoltBoneChainComponent::OnAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg)
{
  // the pose this component sends on comes back to it as well
  if (m_bSendingPose || ref_msg.m_pRootTransform == nullptr)
    return;

  m_AnimatedPose = ref_msg.m_ModelTransforms;
  m_RootTransform = *ref_msg.m_pRootTransform;
  m_bHasPose = true;
}

void plJoltBoneChainComponent::BuildChains(const plSkeletonResourceDescriptor& skeleton)
{
  const plSkeleton& skel = skeleton.m_Skeleton;
  const plUInt16 uiJointCount = skel.GetJointCount();

  m_Solvers.Clear();
  m_Shapes.Clear();
  m_ShapeJointIndices.Clear();

  // which joints a chain moves: its own, and the ones that hang off it
  plDynamicArray<bool> moved;
  moved.SetCount(uiJointCount, false);

  for (const plString& sChain : m_Chains)
  {
    plUInt16 uiJoint = skel.FindJointByName(plTempHashedString(sChain));
    if (uiJoint == plInvalidJointIndex)
    {
      plLog::Warning("Bone chain on '{}': the skeleton has no joint '{}'.", GetOwner()->GetName(), sChain);
      continue;
    }

    // from the named joint along the first child, to the last joint
    plHybridArray<plUInt16, 16> joints;

    while (uiJoint != plInvalidJointIndex)
    {
      joints.PushBack(uiJoint);

      plUInt16 uiChild = plInvalidJointIndex;
      for (plUInt16 j = uiJoint + 1; j < uiJointCount; ++j)
      {
        if (skel.GetJointByIndex(j).GetParentIndex() == uiJoint)
        {
          uiChild = j;
          break;
        }
      }

      uiJoint = uiChild;
    }

    // every other joint below the chain is carried by the chain joint nearest above it
    plHybridArray<plJoltBoneChainSolver::Follower, 16> followers;

    for (plUInt16 j = 0; j < uiJointCount; ++j)
    {
      if (joints.Contains(j))
      {
        moved[j] = true;
        continue;
      }

      for (plUInt16 uiAncestor = skel.GetJointByIndex(j).GetParentIndex(); uiAncestor != plInvalidJointIndex; uiAncestor = skel.GetJointByIndex(uiAncestor).GetParentIndex())
      {
        const plUInt32 uiChainIndex = joints.IndexOf(uiAncestor);
        if (uiChainIndex == plInvalidIndex)
          continue;

        plJoltBoneChainSolver::Follower& follower = followers.ExpandAndGetRef();
        follower.m_uiJoint = j;
        follower.m_uiChainIndex = static_cast<plUInt16>(uiChainIndex);

        moved[j] = true;
        break;
      }
    }

    m_Solvers.ExpandAndGetRef().SetChain(joints, followers);
  }

  if (!m_bCollideWithBody || m_Solvers.IsEmpty())
    return;

  // The body's shapes, the same ones ragdolls, hitboxes and character cloth use. A shape that a chain moves is
  // part of that chain and not something for it to collide with.
  plJoltClothDescriptor body;
  plJoltClothSkeletonColliderOptions all;
  all.m_bAllShapes = true;

  plJoltClothBuilder::AddSkeletonColliders(skeleton, all, body);

  for (const plJoltClothCollider& shape : body.m_Colliders)
  {
    const plUInt16 uiJoint = skel.FindJointByName(shape.m_sJoint);

    if (uiJoint == plInvalidJointIndex || moved[uiJoint])
      continue;

    m_Shapes.PushBack(shape);
    m_ShapeJointIndices.PushBack(uiJoint);
  }
}

void plJoltBoneChainComponent::Update(plTime timeStep)
{
  if (!m_bHasPose || m_Chains.IsEmpty())
    return;

  if (!m_hSkeleton.IsValid())
  {
    plMsgQueryAnimationSkeleton msg;
    GetOwner()->SendMessage(msg);
    m_hSkeleton = msg.m_hSkeleton;

    if (!m_hSkeleton.IsValid())
      return;
  }

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pSkeleton.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  const plSkeletonResourceDescriptor& skeleton = pSkeleton->GetDescriptor();
  if (m_AnimatedPose.GetCount() != skeleton.m_Skeleton.GetJointCount())
    return;

  if (!m_bChainsBuilt || m_uiSkeletonChangeCounter != pSkeleton->GetCurrentResourceChangeCounter())
  {
    BuildChains(skeleton);

    m_bChainsBuilt = true;
    m_uiSkeletonChangeCounter = pSkeleton->GetCurrentResourceChangeCounter();
    m_bResetPending = true;
  }

  // a paused world keeps its chains as they are
  const float fTimeStep = plMath::Min(timeStep.AsFloatInSeconds(), s_fLongestChainTimeStep);
  if (m_Solvers.IsEmpty() || fTimeStep <= 0.0f)
    return;

  // A stand-in rests while the cloth simulates: the animated pose goes through as it is, and the chains start from
  // it when the cloth hands over. The cloth steps after the chains, so its state is the one of the last frame.
  if (m_bStandInForCloth)
  {
    const plJoltCharacterClothComponent* pCloth = nullptr;

    if (GetOwner()->TryGetComponentOfBaseType(pCloth) && pCloth->GetState() >= plJoltClothState::Blending)
    {
      m_bResetPending = true;
      return;
    }
  }

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const plMat4 mModelToWorld = ownerTransform.GetAsMat4() * m_RootTransform.GetAsMat4();

  m_ShapeJoints.SetCountUninitialized(m_Shapes.GetCount());
  for (plUInt32 s = 0; s < m_Shapes.GetCount(); ++s)
  {
    plMat4 mFull;
    plQuat qRotation;
    plMsgAnimationPoseUpdated::ComputeFullBoneTransform(mModelToWorld, m_AnimatedPose[m_ShapeJointIndices[s]], mFull, qRotation);

    m_ShapeJoints[s] = plTransform(mFull.GetTranslationVector(), qRotation);
  }

  plJoltBoneChainSettings settings;
  settings.m_fStiffness = m_fStiffness;
  settings.m_fDamping = m_fDamping;
  settings.m_fGravityFactor = m_fGravityFactor;
  settings.m_fLinearInertia = m_fLinearInertia;
  settings.m_fDrag = 0.5f * m_fWindInfluence;
  settings.m_fRadius = m_fRadius;
  settings.m_MaxAngle = m_MaxAngle;
  settings.m_fTeleportDistance = m_fTeleportDistance;

  plJoltBoneChainSolver::StepInput input;
  input.m_mModelToWorld = mModelToWorld;
  input.m_fTimeStep = fTimeStep;
  input.m_Shapes = m_Shapes;
  input.m_ShapeJoints = m_ShapeJoints;
  input.m_fShapeScale = plMath::Max(0.001f, mModelToWorld.TransformDirection(plVec3(1, 0, 0)).GetLength());
  input.m_bReset = m_bResetPending;

  if (const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
    input.m_vWind = pWind->GetWindAt(ownerTransform.m_vPosition + plVec3(0, 0, 1));

  m_bResetPending = false;

  m_Pose = m_AnimatedPose;

  for (plJoltBoneChainSolver& solver : m_Solvers)
  {
    solver.Step(settings, input, m_Pose);
  }

  if (cvar_JoltBoneChainVisualize)
  {
    for (const plJoltBoneChainSolver& solver : m_Solvers)
    {
      const plArrayPtr<const plVec3> points = solver.GetPoints();

      plHybridArray<plDebugRenderer::Line, 16> lines;
      for (plUInt32 i = 0; i + 1 < points.GetCount(); ++i)
      {
        plDebugRenderer::Line& line = lines.ExpandAndGetRef();
        line.m_start = points[i];
        line.m_end = points[i + 1];
      }

      plDebugRenderer::DrawLines(GetWorld(), lines, plColor(0.84f, 0.51f, 0.47f));
    }
  }

  // the pose with the chains on it replaces the animated one for everything that listens: the mesh, cloth, attachments
  plMsgAnimationPoseUpdated msg;
  msg.m_pRootTransform = &m_RootTransform;
  msg.m_pSkeleton = &skeleton.m_Skeleton;
  msg.m_ModelTransforms = m_Pose;

  m_bSendingPose = true;
  GetOwner()->SendMessageRecursive(msg);
  m_bSendingPose = false;
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltBoneChainComponent);
