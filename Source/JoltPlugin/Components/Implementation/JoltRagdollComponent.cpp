#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Physics/SurfaceResource.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <GameEngine/Animation/Skeletal/AnimatedMeshComponent.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <JoltPlugin/Components/JoltRagdollComponent.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>
#include <JoltPlugin/Utilities/JoltUserData.h>
#include <Physics/Body/BodyLockMulti.h>
#include <Physics/Collision/Shape/CompoundShape.h>
#include <RendererCore/AnimationSystem/AnimPoseGenerator.h>
#include <RendererCore/AnimationSystem/Declarations.h>
#include <RendererCore/AnimationSystem/SkeletonPoseComponent.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Debug/DebugRenderer.h>

/* TODO
 * prevent crashes with zero bodies
 * external constraints
 * max force clamping / point vs area impulse ?
 * integrate root motion into controlled playback (?)
 */

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltRagdollStartMode, 1)
  PL_ENUM_CONSTANTS(plJoltRagdollStartMode::WithBindPose, plJoltRagdollStartMode::WithNextAnimPose, plJoltRagdollStartMode::WithCurrentMeshPose)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltRagdollAnimMode, 1)
  PL_ENUM_CONSTANTS(plJoltRagdollAnimMode::Off, plJoltRagdollAnimMode::Limp, plJoltRagdollAnimMode::Powered, plJoltRagdollAnimMode::Controlled)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltRagdollComponent, 5, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SelfCollision", m_bSelfCollision),
    PL_ENUM_ACCESSOR_PROPERTY("StartMode", plJoltRagdollStartMode, GetStartMode, SetStartMode),
    PL_ENUM_ACCESSOR_PROPERTY("AnimMode", plJoltRagdollAnimMode, GetAnimMode, SetAnimMode),
    PL_ACCESSOR_PROPERTY("GravityFactor", GetGravityFactor, SetGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("WeightCategory", m_uiWeightCategory)->AddAttributes(new plDynamicEnumAttribute("PhysicsWeightCategory")),
    PL_ACCESSOR_PROPERTY("WeightScale", GetWeight_Scale, SetWeight_Scale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_ACCESSOR_PROPERTY("Mass", GetWeight_Mass, SetWeight_Mass)->AddAttributes(new plSuffixAttribute(" kg"), new plDefaultValueAttribute(50.0f), new plClampValueAttribute(1.0f, 1000.0f)),
    PL_MEMBER_PROPERTY("StiffnessFactor", m_fStiffnessFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("OwnerVelocityScale", m_fOwnerVelocityScale)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("CenterPosition", m_vCenterPosition),
    PL_MEMBER_PROPERTY("CenterVelocity", m_fCenterVelocity)->AddAttributes(new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("CenterAngularVelocity", m_fCenterAngularVelocity)->AddAttributes(new plDefaultValueAttribute(0.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgAnimationPoseUpdated, OnAnimationPoseUpdated),
    PL_MESSAGE_HANDLER(plMsgRetrieveBoneState, OnRetrieveBoneState),
    PL_MESSAGE_HANDLER(plMsgPhysicsAddImpulse, OnMsgPhysicsAddImpulse),
    PL_MESSAGE_HANDLER(plMsgInjectPoseCommands, OnMsgAnimationPoseGeneration),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Animation"),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(GetObjectFilterID),
    PL_SCRIPT_FUNCTION_PROPERTY(SetInitialImpulse, In, "vWorldPosition", In, "vWorldDirectionAndStrength"),
    PL_SCRIPT_FUNCTION_PROPERTY(AddInitialImpulse, In, "vWorldPosition", In, "vWorldDirectionAndStrength"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetJointTypeOverride, In, "sJointName", In, "overrideType"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetJointMotorStrength, In, "fStrength"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetJointMotorStrength),
    PL_SCRIPT_FUNCTION_PROPERTY(FadeJointMotorStrength, In, "fTargetStrength", In, "tDuration"),
  }
  PL_END_FUNCTIONS;
}
PL_END_ABSTRACT_COMPONENT_TYPE;
// clang-format on

PL_DEFINE_AS_POD_TYPE(JPH::Vec3);

//////////////////////////////////////////////////////////////////////////

plJoltRagdollComponentManager::plJoltRagdollComponentManager(plWorld* pWorld)
  : plComponentManager<plJoltRagdollComponent, plBlockStorageType::FreeList>(pWorld)
{
}

plJoltRagdollComponentManager::~plJoltRagdollComponentManager() = default;

void plJoltRagdollComponentManager::Initialize()
{
  SUPER::Initialize();

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltRagdollComponentManager::Update, this);
    desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostAsync;
    desc.m_bOnlyUpdateWhenSimulating = true;

    this->RegisterUpdateFunction(desc);
  }
}

void plJoltRagdollComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("UpdateRagdolls");

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  // FetchResults has higher priority and joins the solver before this PostAsync stage.
  for (auto it = GetComponents(); it.IsValid(); it.Next())
  {
    if (it->IsActiveAndSimulating() && it->m_bPendingPose)
      it->CreatePendingLimbs();
  }

  for (auto it : pModule->GetActiveRagdolls())
  {
    plJoltRagdollComponent* pComponent = it.Key();

    pComponent->Update(false);
  }

  plHybridArray<plComponentHandle, 16> sleepingRagdolls;
  sleepingRagdolls = pModule->GetRagdollsPutToSleep();
  for (const auto& hComponent : sleepingRagdolls)
  {
    plJoltRagdollComponent* pComponent = nullptr;
    if (TryGetComponent(hComponent, pComponent) && pComponent->IsActiveAndSimulating())
      pComponent->Update(true);
  }
}

void plJoltRagdollComponentManager::DriveAnimatedRagdolls(plTime deltaTime)
{
  PL_PROFILE_SCOPE("DriveAnimatedRagdolls");

  for (auto it = GetComponents(0); it.IsValid(); it.Next())
  {
    it->DriveAnimated(deltaTime);
  }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

plJoltRagdollComponent::plJoltRagdollComponent() = default;
plJoltRagdollComponent::~plJoltRagdollComponent() = default;

void plJoltRagdollComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();


  s << m_StartMode;
  s << m_fGravityFactor;
  s << m_bSelfCollision;
  s << m_fOwnerVelocityScale;
  s << m_fCenterVelocity;
  s << m_fCenterAngularVelocity;
  s << m_vCenterPosition;
  s << m_uiWeightCategory;
  s << (float)m_fWeightScale;
  s << (float)m_fWeightMass;
  s << m_fStiffnessFactor;
  s << m_AnimMode;
}

void plJoltRagdollComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  auto& s = inout_stream.GetStream();

  PL_ASSERT_DEBUG(uiVersion >= 4, "Outdated version, please re-transform asset.");
  if (uiVersion < 4)
    return;

  s >> m_StartMode;
  s >> m_fGravityFactor;
  s >> m_bSelfCollision;
  s >> m_fOwnerVelocityScale;
  s >> m_fCenterVelocity;
  s >> m_fCenterAngularVelocity;
  s >> m_vCenterPosition;
  s >> m_uiWeightCategory;

  {
    float f;

    s >> f;
    m_fWeightScale = f;

    s >> f;
    m_fWeightMass = f;
  }

  s >> m_fStiffnessFactor;

  if (uiVersion >= 5)
  {
    s >> m_AnimMode;
  }
}

void plJoltRagdollComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  if (m_StartMode == plJoltRagdollStartMode::WithBindPose)
  {
    CreateLimbsFromBindPose();
  }
  if (m_StartMode == plJoltRagdollStartMode::WithCurrentMeshPose)
  {
    CreateLimbsFromCurrentMeshPose();
  }
}

void plJoltRagdollComponent::OnDeactivated()
{
  DestroyAllLimbs();

  if (m_pSkeletonPose)
  {
    auto pMan = static_cast<plJoltRagdollComponentManager*>(GetOwningManager());

    PL_LOCK(pMan->m_SkeletonsMutex);
    pMan->m_FreeSkeletonPoses.PushBack(std::move(m_pSkeletonPose));
    m_pSkeletonPose.Clear();
  }

  SUPER::OnDeactivated();
}

void plJoltRagdollComponent::Update(bool bForce)
{
  if (!HasCreatedLimbs())
    return;

  if (m_AnimMode != plJoltRagdollAnimMode::Powered && m_bIsPowered)
  {
    ResetJointMotors();
  }

  if (m_MotorLerpDuration.IsPositive())
  {
    const plTime tNow = GetWorld()->GetClock().GetAccumulatedTime();
    const plTime tDiff = GetWorld()->GetClock().GetTimeDiff();
    const plTime tStart = tNow - tDiff;
    const plTime tFinish = tStart + m_MotorLerpDuration;

    const float fLerpFactor = plMath::Saturate(plMath::Unlerp(tStart.GetSeconds(), tFinish.GetSeconds(), tNow.GetSeconds()));
    const float fNewStrength = plMath::Lerp(m_fMotorStrength, m_fMotorTargetStrength, fLerpFactor);

    if (m_fMotorStrength != fNewStrength)
    {
      m_fMotorStrength = fNewStrength;
      m_MotorLerpDuration -= tDiff;

      if (m_MotorLerpDuration.IsNegative())
        m_MotorLerpDuration = plTime::MakeZero();

      ApplyJointMotorStrength(fNewStrength);
    }
  }

  if (m_pSkeletonPose && (m_AnimMode != plJoltRagdollAnimMode::Controlled) && (m_AnimMode != plJoltRagdollAnimMode::Powered || m_fMotorStrength <= 0.0f))
  {
    auto pMan = static_cast<plJoltRagdollComponentManager*>(GetOwningManager());

    PL_LOCK(pMan->m_SkeletonsMutex);
    pMan->m_FreeSkeletonPoses.PushBack(std::move(m_pSkeletonPose));
    m_pSkeletonPose.Clear();
  }

  const plVisibilityState visState = GetOwner()->GetVisibilityState();
  if (!bForce && visState != plVisibilityState::Direct)
  {
    m_ElapsedTimeSinceUpdate += plClock::GetGlobalClock()->GetTimeDiff();

    if (visState == plVisibilityState::Indirect && m_ElapsedTimeSinceUpdate < plTime::MakeFromMilliseconds(200))
    {
      // when the ragdoll is only visible by shadows or reflections, update it infrequently
      return;
    }

    if (visState == plVisibilityState::Invisible && m_ElapsedTimeSinceUpdate < plTime::MakeFromMilliseconds(500))
    {
      // when the ragdoll is entirely invisible, update it very rarely
      return;
    }
  }

  const plVec3 vRootPos = RetrieveRagdollPose();
  GetOwner()->SetGlobalPosition(vRootPos);

  SendAnimationPoseMsg();

  m_ElapsedTimeSinceUpdate = plTime::MakeZero();
}

void plJoltRagdollComponent::DriveAnimated(plTime deltaTime)
{
  if (m_pSkeletonPose)
  {
    m_pSkeletonPose->CalculateJointStates();

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
    for (auto& joint : m_pSkeletonPose->GetJoints())
    {
      joint.mRotation = joint.mRotation.Normalized();
    }
#endif

    if (m_AnimMode == plJoltRagdollAnimMode::Controlled)
    {
      m_pRagdoll->DriveToPoseUsingKinematics(*m_pSkeletonPose, deltaTime.AsFloatInSeconds());
    }
    else if (m_AnimMode == plJoltRagdollAnimMode::Powered && m_fMotorStrength > 0.0f)
    {
      m_bIsPowered = true;
      m_pRagdoll->DriveToPoseUsingMotors(*m_pSkeletonPose);
    }
    else
    {
      m_pSkeletonPose.Clear();
    }
  }
}

plResult plJoltRagdollComponent::EnsureSkeletonIsKnown()
{
  if (!m_hSkeleton.IsValid())
  {
    plMsgQueryAnimationSkeleton msg;
    GetOwner()->SendMessage(msg);
    m_hSkeleton = msg.m_hSkeleton;
  }

  if (!m_hSkeleton.IsValid())
  {
    plLog::Error("No skeleton available for ragdoll on object '{}'.", GetOwner()->GetName());
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

bool plJoltRagdollComponent::HasCreatedLimbs() const
{
  return m_pRagdoll != nullptr;
}

void plJoltRagdollComponent::CreateLimbsFromBindPose()
{
  DestroyAllLimbs();

  if (EnsureSkeletonIsKnown().Failed())
    return;

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  const auto& desc = pSkeleton->GetDescriptor();

  m_CurrentLimbTransforms.SetCountUninitialized(desc.m_Skeleton.GetJointCount());

  auto ComputeFullJointTransform = [&](plUInt32 uiJointIdx, auto self) -> plMat4
  {
    const auto& joint = desc.m_Skeleton.GetJointByIndex(uiJointIdx);
    const plMat4 jointTransform = joint.GetRestPoseLocalTransform().GetAsMat4();

    if (joint.GetParentIndex() != plInvalidJointIndex)
    {
      const plMat4 parentTransform = self(joint.GetParentIndex(), self);

      return parentTransform * jointTransform;
    }

    return jointTransform;
  };

  for (plUInt32 i = 0; i < m_CurrentLimbTransforms.GetCount(); ++i)
  {
    m_CurrentLimbTransforms[i] = ComputeFullJointTransform(i, ComputeFullJointTransform);
  }

  plMsgAnimationPoseUpdated msg;
  msg.m_pRootTransform = &desc.m_RootTransform;
  msg.m_pSkeleton = &desc.m_Skeleton;
  msg.m_ModelTransforms = m_CurrentLimbTransforms;

  CreateLimbsFromPose(msg);
}

void plJoltRagdollComponent::CreateLimbsFromCurrentMeshPose()
{
  DestroyAllLimbs();

  if (EnsureSkeletonIsKnown().Failed())
    return;

  plAnimatedMeshComponent* pMesh = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType<plAnimatedMeshComponent>(pMesh))
    return;

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded_NeverFail);

  plTransform tRoot;
  pMesh->RetrievePose(m_CurrentLimbTransforms, tRoot, pSkeleton->GetDescriptor().m_Skeleton);

  plMsgAnimationPoseUpdated msg;
  msg.m_pRootTransform = &tRoot;
  msg.m_pSkeleton = &pSkeleton->GetDescriptor().m_Skeleton;
  msg.m_ModelTransforms = m_CurrentLimbTransforms;

  CreateLimbsFromPose(msg);
}

void plJoltRagdollComponent::DestroyAllLimbs()
{
  m_bPendingPose = false;
  if (m_pRagdoll)
  {
    m_pRagdoll->RemoveFromPhysicsSystem();
    m_pRagdoll->Release();
    m_pRagdoll = nullptr;
  }

  if (plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>())
  {
    pModule->DeallocateUserData(m_uiJoltUserDataIndex);
    pModule->DeleteObjectFilterID(m_uiObjectFilterID);
    m_pJoltUserData = nullptr;
  }

  m_CurrentLimbTransforms.Clear();
  m_Limbs.Clear();
}

void plJoltRagdollComponent::SetGravityFactor(float fFactor)
{
  if (m_fGravityFactor == fFactor)
    return;

  m_fGravityFactor = fFactor;

  if (!m_pRagdoll)
    return;

  for (plUInt32 i = 0; i < m_pRagdoll->GetBodyCount(); ++i)
  {
    m_pJoltSystem->GetBodyInterface().SetGravityFactor(m_pRagdoll->GetBodyID(i), m_fGravityFactor);
  }

  m_pRagdoll->Activate();
}

void plJoltRagdollComponent::SetStartMode(plEnum<plJoltRagdollStartMode> mode)
{
  if (m_StartMode == mode)
    return;

  m_StartMode = mode;
}

void plJoltRagdollComponent::SetAnimMode(plEnum<plJoltRagdollAnimMode> mode)
{
  if (m_AnimMode == mode)
    return;

  m_AnimMode = mode;

  if (m_pRagdoll)
  {
    if (m_AnimMode == plJoltRagdollAnimMode::Controlled)
    {
      for (plUInt32 i = 0; i < m_pRagdoll->GetBodyCount(); ++i)
      {
        // in the 'Controlled' mode, disable gravity, so that it doesn't affect the pose
        m_pJoltSystem->GetBodyInterface().SetGravityFactor(m_pRagdoll->GetBodyID(i), 0.0f);
      }
    }
    else
    {
      for (plUInt32 i = 0; i < m_pRagdoll->GetBodyCount(); ++i)
      {
        m_pJoltSystem->GetBodyInterface().SetGravityFactor(m_pRagdoll->GetBodyID(i), m_fGravityFactor);
      }
    }
  }
}

void plJoltRagdollComponent::OnMsgPhysicsAddImpulse(plMsgPhysicsAddImpulse& ref_msg)
{
  if (!ref_msg.m_vImpulse.IsValid() || !ref_msg.m_vGlobalPosition.IsValid())
    return;

  const float fImpulse = plJoltCore::GetImpulseTypeConfig().GetImpulseForWeight(ref_msg.m_uiImpulseType, m_uiWeightCategory);

  if (!HasCreatedLimbs())
  {
    m_vInitialImpulsePosition += ref_msg.m_vGlobalPosition;
    m_vInitialImpulseDirection += ref_msg.m_vImpulse * fImpulse;
    m_uiNumInitialImpulses++;
    return;
  }

  if (ref_msg.m_pInternalPhysicsActor == nullptr)
    return;

  const plUInt32 uiBodyId = reinterpret_cast<size_t>(ref_msg.m_pInternalPhysicsActor) & 0xFFFFFFFF;
  for (const auto& bodyId : m_pRagdoll->GetBodyIDs())
  {
    if (bodyId.GetIndexAndSequenceNumber() == uiBodyId)
    {
      GetWorld()->GetModule<plJoltWorldModule>()->AddImpulse(uiBodyId, ref_msg.m_vImpulse * fImpulse, ref_msg.m_vGlobalPosition);
      return;
    }
  }
}

void plJoltRagdollComponent::SetInitialImpulse(const plVec3& vPosition, const plVec3& vDirectionAndStrength)
{
  if (vDirectionAndStrength.IsZero())
  {
    m_vInitialImpulsePosition.SetZero();
    m_vInitialImpulseDirection.SetZero();
    m_uiNumInitialImpulses = 0;
  }
  else
  {
    m_vInitialImpulsePosition = vPosition;
    m_vInitialImpulseDirection = vDirectionAndStrength;
    m_uiNumInitialImpulses = 1;
  }
}

void plJoltRagdollComponent::AddInitialImpulse(const plVec3& vPosition, const plVec3& vDirectionAndStrength)
{
  m_vInitialImpulsePosition += vPosition;
  m_vInitialImpulseDirection += vDirectionAndStrength;
  m_uiNumInitialImpulses++;
}

void plJoltRagdollComponent::SetJointTypeOverride(plStringView sJointName, plEnum<plSkeletonJointType> type)
{
  const plTempHashedString sJointNameHashed(sJointName);

  for (plUInt32 i = 0; i < m_JointOverrides.GetCount(); ++i)
  {
    if (m_JointOverrides[i].m_sJointName == sJointNameHashed)
    {
      m_JointOverrides[i].m_JointType = type;
      return;
    }
  }

  auto& jo = m_JointOverrides.ExpandAndGetRef();
  jo.m_sJointName = sJointNameHashed;
  jo.m_JointType = type;
}

void plJoltRagdollComponent::OnAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_poseMsg)
{
  if (!IsActiveAndSimulating())
    return;

  if (HasCreatedLimbs())
  {
    // Note: if this code is reached, although the ragdoll is supposed to use the animation (controlled or powered anim mode)
    // then the component that generates the animation doesn't have "Apply IK" enabled, ie it doesn't send plMsgAnimationPoseGeneration
    ref_poseMsg.m_bContinueAnimating = false;

    // TODO: if at some point we can layer ragdolls with detail animations, we should
    // take poses for all bones for which there are no shapes (link == null) -> to animate leafs (fingers and such)
    return;
  }

  if (m_StartMode != plJoltRagdollStartMode::WithNextAnimPose)
    return;

  if (ref_poseMsg.m_pRootTransform == nullptr)
    return;

  m_CurrentLimbTransforms = ref_poseMsg.m_ModelTransforms;
  m_PendingRootTransform = *ref_poseMsg.m_pRootTransform;
  m_bPendingPose = true;
}

void plJoltRagdollComponent::CreatePendingLimbs()
{
  m_bPendingPose = false;
  if (HasCreatedLimbs() || EnsureSkeletonIsKnown().Failed())
    return;

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  if (m_CurrentLimbTransforms.GetCount() != pSkeleton->GetDescriptor().m_Skeleton.GetJointCount())
  {
    plLog::Warning("Ragdoll pose no longer matches its skeleton; discarding pending creation.");
    return;
  }

  plMsgAnimationPoseUpdated pose;
  pose.m_pRootTransform = &m_PendingRootTransform;
  pose.m_pSkeleton = &pSkeleton->GetDescriptor().m_Skeleton;
  pose.m_ModelTransforms = m_CurrentLimbTransforms;
  CreateLimbsFromPose(pose);
}

void plJoltRagdollComponent::OnRetrieveBoneState(plMsgRetrieveBoneState& ref_msg) const
{
  if (!HasCreatedLimbs())
    return;

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  const auto& skeleton = pSkeleton->GetDescriptor().m_Skeleton;

  for (plUInt32 uiJointIdx = 0; uiJointIdx < skeleton.GetJointCount(); ++uiJointIdx)
  {
    plMat4 mJoint = m_CurrentLimbTransforms[uiJointIdx];

    const auto& joint = skeleton.GetJointByIndex(uiJointIdx);
    const plUInt16 uiParentIdx = joint.GetParentIndex();
    if (uiParentIdx != plInvalidJointIndex)
    {
      // remove the parent transform to get the pure local transform
      const plMat4 mParent = m_CurrentLimbTransforms[uiParentIdx].GetInverse();

      mJoint = mParent * mJoint;
    }

    auto& t = ref_msg.m_BoneTransforms[joint.GetName().GetString()];
    t.m_vPosition = mJoint.GetTranslationVector();
    t.m_qRotation.ReconstructFromMat4(mJoint);
    t.m_vScale.Set(1.0f);
  }
}

static void ComputeFullBoneTransform(const plMat4& mRootTransform, const plMat4& mModelTransform, plTransform& out_transform)
{
  plMat4 mFullTransform = mRootTransform * mModelTransform;

  out_transform.m_qRotation.ReconstructFromMat4(mFullTransform);
  out_transform.m_vScale.Set(1);
  out_transform.m_vPosition = mFullTransform.GetTranslationVector();
}

void plJoltRagdollComponent::OnMsgAnimationPoseGeneration(plMsgInjectPoseCommands& ref_msg)
{
  if (m_AnimMode == plJoltRagdollAnimMode::Limp)
    return;

  if (!HasCreatedLimbs())
    return;

  if (ref_msg.m_uiOrderNow > 0xFF00)
    return;

  if (ref_msg.m_uiOrderNow < 0xFF00)
  {
    ref_msg.m_uiOrderNext = plMath::Min<plUInt16>(ref_msg.m_uiOrderNext, 0xFF00);
    return;
  }

  if (m_AnimMode == plJoltRagdollAnimMode::Powered && m_fMotorStrength == 0.0f)
  {
    // basically the same as Limp mode, but we don't want to deactivate animations, because motor strength can still be changed
    return;
  }

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  const plMat4 mRootTransform = pSkeleton->GetDescriptor().m_RootTransform.GetAsMat4();

  const plTransform tGlobal = GetOwner()->GetGlobalTransform();
  const auto& curPose = ref_msg.m_pGenerator->GetCurrentPose();

  if (m_pSkeletonPose == nullptr)
  {
    auto pMan = static_cast<plJoltRagdollComponentManager*>(GetOwningManager());
    PL_LOCK(pMan->m_SkeletonsMutex);
    if (!pMan->m_FreeSkeletonPoses.IsEmpty())
    {
      m_pSkeletonPose = std::move(pMan->m_FreeSkeletonPoses.PeekBack());
      pMan->m_FreeSkeletonPoses.PopBack();
    }
    else
    {
      m_pSkeletonPose = PL_NEW(plFoundation::GetAlignedAllocator(), JPH::SkeletonPose);
    }
  }

  m_pSkeletonPose->SetSkeleton(m_pRagdoll->GetRagdollSettings()->GetSkeleton());

  plVec3 vRootOffset(0);

  for (plUInt32 uiLimbIdx = 0; uiLimbIdx < m_Limbs.GetCount(); ++uiLimbIdx)
  {
    if (m_Limbs[uiLimbIdx].m_uiPartIndex != plInvalidJointIndex)
    {
      JPH::Mat44& jointMat = m_pSkeletonPose->GetJointMatrix(m_Limbs[uiLimbIdx].m_uiPartIndex);

      plTransform trans;
      ComputeFullBoneTransform(mRootTransform, curPose[uiLimbIdx], trans);

      trans = plTransform::MakeGlobalTransform(tGlobal, trans);

      plMat4 mGlobal = trans.GetAsMat4();
      jointMat = JPH::Mat44::sLoadFloat4x4((const JPH::Float4*)mGlobal.m_fElementsCM);
    }
  }

  m_pSkeletonPose->SetRootOffset(plJoltConversionUtils::ToVec3(vRootOffset));
}

void plJoltRagdollComponent::SetJointMotorStrength(float fStrength)
{
  if (m_fMotorStrength != fStrength)
  {
    ApplyJointMotorStrength(fStrength);
  }

  m_fMotorStrength = fStrength;
  m_fMotorTargetStrength = fStrength;
  m_MotorLerpDuration = plTime::MakeZero();
}

float plJoltRagdollComponent::GetJointMotorStrength() const
{
  return m_fMotorStrength;
}

void plJoltRagdollComponent::FadeJointMotorStrength(float fTargetStrength, plTime duration)
{
  m_fMotorTargetStrength = fTargetStrength;
  m_MotorLerpDuration = duration;
}

void plJoltRagdollComponent::SendAnimationPoseMsg()
{
  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  const plTransform rootTransform = pSkeleton->GetDescriptor().m_RootTransform;

  plMsgAnimationPoseUpdated poseMsg;
  poseMsg.m_ModelTransforms = m_CurrentLimbTransforms;
  poseMsg.m_pRootTransform = &rootTransform;
  poseMsg.m_pSkeleton = &pSkeleton->GetDescriptor().m_Skeleton;

  GetOwner()->SendMessage(poseMsg);
}

plVec3 plJoltRagdollComponent::RetrieveRagdollPose()
{
  const float fLerpToPos = (m_AnimMode != plJoltRagdollAnimMode::Controlled) ? 0.1f : 0.0f;

  const JPH::RVec3 vCurPosition = plJoltConversionUtils::ToVec3(GetOwner()->GetGlobalPosition());

  const int body_count = (int)m_pRagdoll->GetBodyCount();
  if (body_count == 0)
    return GetOwner()->GetGlobalPosition();
  JPH::BodyLockMultiRead lock(static_cast<const JPH::BodyLockInterface&>(m_pJoltSystem->GetBodyLockInterface()), m_pRagdoll->GetBodyIDs().data(), body_count);

  const JPH::Body* root = lock.GetBody(0);
  if (root == nullptr)
    return GetOwner()->GetGlobalPosition();
  JPH::RMat44 root_transform = root->GetWorldTransform();
  JPH::RVec3 vRootOffset = root_transform.GetTranslation();
  vRootOffset = vCurPosition + (vRootOffset - vCurPosition) * fLerpToPos; // interpolate the object position towards the root bone position


  const plVec3 vObjectScale = GetOwner()->GetGlobalScaling();
  const float fObjectScale = plMath::Max(vObjectScale.x, vObjectScale.y, vObjectScale.z);

  plResourceLock<plSkeletonResource> pSkeleton(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);
  const plSkeleton& skeleton = pSkeleton->GetDescriptor().m_Skeleton;
  const plTransform rootTransform = pSkeleton->GetDescriptor().m_RootTransform;
  const plQuat qGlobalRot = GetOwner()->GetGlobalRotation();
  const plMat4 mInvScale = plMat4::MakeScaling(plVec3(1.0f).CompDiv(plVec3(fObjectScale)));

  const plMat4 mInv = m_mInvSkeletonRootTransform * qGlobalRot.GetInverse().GetAsMat4() * mInvScale;
  const plMat4 mScale = plMat4::MakeScaling(rootTransform.m_vScale * fObjectScale);

  plHybridArray<plMat4, 128> relativeTransforms;

  {
    // m_CurrentLimbTransforms is stored in model space
    // for bones that don't have their own shape in the ragdoll,
    // we don't get a new transform from the ragdoll, but we still must update them,
    // if there is a parent bone in the ragdoll, otherwise they don't move along as expected
    // therefore we compute their relative transform here
    // and then later we take their new parent transform (which may come from the ragdoll)
    // to set their final new transform

    for (plUInt32 uiLimbIdx = 0; uiLimbIdx < m_Limbs.GetCount(); ++uiLimbIdx)
    {
      if (m_Limbs[uiLimbIdx].m_uiPartIndex != plInvalidJointIndex)
        continue;

      const auto& joint = skeleton.GetJointByIndex(uiLimbIdx);
      const plUInt16 uiParentIdx = joint.GetParentIndex();

      if (uiParentIdx == plInvalidJointIndex)
        continue;

      const plMat4 mJoint = m_CurrentLimbTransforms[uiLimbIdx];

      // remove the parent transform to get the pure local transform
      const plMat4 mParentInv = m_CurrentLimbTransforms[uiParentIdx].GetInverse();

      relativeTransforms.PushBack(mParentInv * mJoint);
    }
  }

  plUInt32 uiNextRelativeIdx = 0;
  for (plUInt32 uiLimbIdx = 0; uiLimbIdx < m_Limbs.GetCount(); ++uiLimbIdx)
  {
    if (m_Limbs[uiLimbIdx].m_uiPartIndex == plInvalidJointIndex)
    {
      const auto& joint = skeleton.GetJointByIndex(uiLimbIdx);
      const plUInt16 uiParentIdx = joint.GetParentIndex();

      if (uiParentIdx != plInvalidJointIndex)
      {
        m_CurrentLimbTransforms[uiLimbIdx] = m_CurrentLimbTransforms[uiParentIdx] * relativeTransforms[uiNextRelativeIdx];
        ++uiNextRelativeIdx;
      }
    }
    else
    {
      const JPH::Body* pBody = lock.GetBody(m_Limbs[uiLimbIdx].m_uiPartIndex);
      const JPH::RMat44 transform = pBody->GetWorldTransform();
      const JPH::Mat44 jointMatrix = JPH::Mat44(transform.GetColumn4(0), transform.GetColumn4(1), transform.GetColumn4(2), JPH::Vec4(JPH::Vec3(transform.GetTranslation() - vRootOffset), 1));

      const plMat4& mPose = (const plMat4&)jointMatrix;

      m_CurrentLimbTransforms[uiLimbIdx] = (mInv * mPose) * mScale;
    }
  }

  return plJoltConversionUtils::ToVec3(vRootOffset);
}

void plJoltRagdollComponent::CreateLimbsFromPose(const plMsgAnimationPoseUpdated& pose)
{
  PL_ASSERT_DEBUG(!HasCreatedLimbs(), "Limbs are already created.");

  if (EnsureSkeletonIsKnown().Failed())
    return;

  const plVec3 vObjectScale = GetOwner()->GetGlobalScaling();
  const float fObjectScale = plMath::Max(vObjectScale.x, vObjectScale.y, vObjectScale.z);

  plJoltWorldModule& worldModule = *GetWorld()->GetOrCreateModule<plJoltWorldModule>();
  m_pJoltSystem = worldModule.GetJoltSystem(); // cache this for later
  m_uiObjectFilterID = worldModule.CreateObjectFilterID();
  m_uiJoltUserDataIndex = worldModule.AllocateUserData(m_pJoltUserData);
  m_pJoltUserData->Init(this);

  plResourceLock<plSkeletonResource> pSkeletonResource(m_hSkeleton, plResourceAcquireMode::BlockTillLoaded);

  m_mInvSkeletonRootTransform = pSkeletonResource->GetDescriptor().m_RootTransform.GetAsMat4().GetInverse();

  // allocate the limbs array
  m_Limbs.SetCount(pose.m_ModelTransforms.GetCount());

  JPH::Ref<JPH::RagdollSettings> ragdollSettings = new JPH::RagdollSettings();

  ragdollSettings->mParts.reserve(pSkeletonResource->GetDescriptor().m_Skeleton.GetJointCount());
  ragdollSettings->mSkeleton = new JPH::Skeleton(); // TODO: share this in the resource
  ragdollSettings->mSkeleton->GetJoints().reserve(ragdollSettings->mParts.size());

  CreateAllLimbs(*pSkeletonResource.GetPointer(), pose, worldModule, fObjectScale, ragdollSettings);

  if (ragdollSettings->mParts.empty())
  {
    plLog::Warning("Ragdoll skeleton '{}' contains no usable collision limbs. Add collision geometry to the skeleton asset.", pSkeletonResource->GetResourceIdOrDescription());
    DestroyAllLimbs();
    return;
  }

  {
    const float fInitialMass = plJoltCore::GetWeightCategoryConfig().GetMassForWeightCategory(m_uiWeightCategory, 50.0f, m_fWeightMass, m_fWeightScale);

    ApplyBodyMass(ragdollSettings, fInitialMass);
  }

  SetupLimbJoints(pSkeletonResource.GetPointer(), ragdollSettings);
  ApplyPartInitialVelocity(ragdollSettings);

  if (m_bSelfCollision)
  {
    // enables collisions between all bodies except the ones that are directly connected to each other
    ragdollSettings->DisableParentChildCollisions();
  }

  ragdollSettings->Stabilize();
  ragdollSettings->CalculateBodyIndexToConstraintIndex();
  ragdollSettings->CalculateConstraintIndexToBodyIdxPair();

  m_pRagdoll = ragdollSettings->CreateRagdoll(m_uiObjectFilterID, reinterpret_cast<plUInt64>(m_pJoltUserData), m_pJoltSystem);

  if (m_pRagdoll == nullptr)
  {
    plLog::Error("Failed to create ragdoll bodies for '{}'. Check collision geometry and the physics body limit.", pSkeletonResource->GetResourceIdOrDescription());
    DestroyAllLimbs();
    return;
  }

  m_pRagdoll->AddRef();
  m_pRagdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

  ApplyInitialImpulse(worldModule, pSkeletonResource->GetDescriptor().m_fMaxImpulse);
}

void plJoltRagdollComponent::ConfigureRagdollPart(void* pRagdollSettingsPart, const plTransform& globalTransform, plUInt8 uiCollisionLayer, plJoltWorldModule& worldModule)
{
  JPH::RagdollSettings::Part* pPart = reinterpret_cast<JPH::RagdollSettings::Part*>(pRagdollSettingsPart);

  pPart->mPosition = plJoltConversionUtils::ToVec3(globalTransform.m_vPosition);
  pPart->mRotation = plJoltConversionUtils::ToQuat(globalTransform.m_qRotation).Normalized();
  pPart->mMotionQuality = JPH::EMotionQuality::LinearCast;
  pPart->mGravityFactor = m_AnimMode == plJoltRagdollAnimMode::Controlled ? 0.0f : m_fGravityFactor;
  pPart->mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
  pPart->mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(uiCollisionLayer, plJoltBroadphaseLayer::Ragdoll);
  pPart->mCollisionGroup.SetGroupID(m_uiObjectFilterID);
  pPart->mCollisionGroup.SetGroupFilter(worldModule.GetGroupFilterIgnoreSame()); // this is used if m_bSelfCollision is off, otherwise it gets overridden below
}

void plJoltRagdollComponent::ApplyPartInitialVelocity(JPH::RagdollSettings* pRagdollSettings)
{
  JPH::Vec3 vCommonVelocity = plJoltConversionUtils::ToVec3(GetOwner()->GetLinearVelocity() * m_fOwnerVelocityScale);
  const JPH::Vec3 vCenterPos = plJoltConversionUtils::ToVec3(GetOwner()->GetGlobalTransform() * m_vCenterPosition);

  plCoordinateSystem coord;
  GetWorld()->GetCoordinateSystem(GetOwner()->GetGlobalPosition(), coord);
  plRandom& rng = GetOwner()->GetWorld()->GetRandomNumberGenerator();

  for (JPH::RagdollSettings::Part& part : pRagdollSettings->mParts)
  {
    part.mLinearVelocity = vCommonVelocity;

    if (m_fCenterVelocity != 0.0f)
    {
      const JPH::Vec3 vVelocityDir = (part.mPosition - vCenterPos).NormalizedOr(JPH::Vec3::sZero());
      part.mLinearVelocity += vVelocityDir * plMath::Min(part.mMaxLinearVelocity, m_fCenterVelocity);
    }

    if (m_fCenterAngularVelocity != 0.0f)
    {
      const plVec3 vVelocityDir = plJoltConversionUtils::ToVec3(part.mPosition - vCenterPos);
      plVec3 vRotationDir = vVelocityDir.CrossRH(coord.m_vUpDir);
      vRotationDir.NormalizeIfNotZero(coord.m_vUpDir).IgnoreResult();

      plVec3 vRotationAxis = plVec3::MakeRandomDeviation(rng, plAngle::MakeFromDegree(30.0f), vRotationDir);
      vRotationAxis *= rng.Bool() ? 1.0f : -1.0f;

      float fSpeed = rng.FloatVariance(m_fCenterAngularVelocity, 0.5f);
      fSpeed = plMath::Min(fSpeed, part.mMaxAngularVelocity * 0.95f);

      part.mAngularVelocity = plJoltConversionUtils::ToVec3(vRotationAxis) * fSpeed;
    }
  }
}

void plJoltRagdollComponent::ApplyInitialImpulse(plJoltWorldModule& worldModule, float fMaxImpulse)
{
  if (m_uiNumInitialImpulses == 0)
    return;

  if (m_uiNumInitialImpulses > 1)
  {
    plLog::Info("Impulses: {} - {}", m_uiNumInitialImpulses, m_vInitialImpulseDirection.GetLength());
  }

  auto pJoltSystem = worldModule.GetJoltSystem();

  m_vInitialImpulsePosition /= m_uiNumInitialImpulses;

  float fImpulse = m_vInitialImpulseDirection.GetLength();

  if (fImpulse > fMaxImpulse)
  {
    fImpulse = fMaxImpulse;
    m_vInitialImpulseDirection.SetLength(fImpulse).AssertSuccess();
  }

  const JPH::Vec3 vImpulsePosition = plJoltConversionUtils::ToVec3(m_vInitialImpulsePosition);
  float fLowestDistanceSqr = 100000;

  JPH::BodyID closestBody;

  for (plUInt32 uiBodyIdx = 0; uiBodyIdx < m_pRagdoll->GetBodyCount(); ++uiBodyIdx)
  {
    const JPH::BodyID bodyId = m_pRagdoll->GetBodyID(uiBodyIdx);
    JPH::BodyLockRead bodyRead(pJoltSystem->GetBodyLockInterface(), bodyId);

    const float fDistanceToImpulseSqr = (bodyRead.GetBody().GetPosition() - vImpulsePosition).LengthSq();

    if (fDistanceToImpulseSqr < fLowestDistanceSqr)
    {
      fLowestDistanceSqr = fDistanceToImpulseSqr;
      closestBody = bodyId;
    }
  }

  if (pJoltSystem->GetBodyInterface().IsAdded(closestBody))
  {
    pJoltSystem->GetBodyInterface().AddImpulse(closestBody, plJoltConversionUtils::ToVec3(m_vInitialImpulseDirection), vImpulsePosition);
  }
}

void plJoltRagdollComponent::ResetJointMotors()
{
  if (!m_bIsPowered)
    return;

  m_bIsPowered = false;

  if (!m_pRagdoll)
    return;

  for (int i = 0; i < (int)m_pRagdoll->GetConstraintCount(); ++i)
  {
    JPH::TwoBodyConstraint* pConstraint = m_pRagdoll->GetConstraint(i);

    JPH::EConstraintSubType subType = pConstraint->GetSubType();
    if (subType == JPH::EConstraintSubType::SwingTwist)
    {
      JPH::SwingTwistConstraint* pStConstraint = static_cast<JPH::SwingTwistConstraint*>(pConstraint);
      pStConstraint->SetSwingMotorState(JPH::EMotorState::Off);
      pStConstraint->SetTwistMotorState(JPH::EMotorState::Off);
    }
  }
}

void plJoltRagdollComponent::ApplyJointMotorStrength(float fStrength)
{
  if (!m_pRagdoll)
    return;

  for (int i = 0; i < (int)m_pRagdoll->GetConstraintCount(); ++i)
  {
    JPH::TwoBodyConstraint* pConstraint = m_pRagdoll->GetConstraint(i);

    JPH::EConstraintSubType subType = pConstraint->GetSubType();
    if (subType == JPH::EConstraintSubType::SwingTwist)
    {
      JPH::SwingTwistConstraint* pStConstraint = static_cast<JPH::SwingTwistConstraint*>(pConstraint);

      pStConstraint->GetSwingMotorSettings().SetForceLimit(m_fMotorStrength);
      pStConstraint->GetTwistMotorSettings().SetForceLimit(m_fMotorStrength);

      // torque is needed for the 'powered' animation mode
      pStConstraint->GetSwingMotorSettings().SetTorqueLimit(m_fMotorStrength);
      pStConstraint->GetTwistMotorSettings().SetTorqueLimit(m_fMotorStrength);
    }
  }
}

void plJoltRagdollComponent::ApplyBodyMass(JPH::RagdollSettings* pRagdollSettings, float fMass)
{
  if (fMass <= 0.0f)
    return;

  float fPartMass = fMass / pRagdollSettings->mParts.size();

  for (auto& part : pRagdollSettings->mParts)
  {
    part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    part.mMassPropertiesOverride.mMass = fPartMass;
  }
}

void plJoltRagdollComponent::ComputeLimbModelSpaceTransform(plTransform& transform, const plMsgAnimationPoseUpdated& pose, plUInt32 uiPoseJointIndex)
{
  plMat4 mFullTransform;
  pose.ComputeFullBoneTransform(uiPoseJointIndex, mFullTransform, transform.m_qRotation);

  transform.m_vScale.Set(1);
  transform.m_vPosition = mFullTransform.GetTranslationVector();
}


void plJoltRagdollComponent::ComputeLimbGlobalTransform(plTransform& transform, const plMsgAnimationPoseUpdated& pose, plUInt32 uiPoseJointIndex)
{
  plTransform local;
  ComputeLimbModelSpaceTransform(local, pose, uiPoseJointIndex);
  transform = plTransform::MakeGlobalTransform(GetOwner()->GetGlobalTransform(), local);
}

void plJoltRagdollComponent::CreateAllLimbs(const plSkeletonResource& skeletonResource, const plMsgAnimationPoseUpdated& pose, plJoltWorldModule& worldModule, float fObjectScale, JPH::RagdollSettings* pRagdollSettings)
{
  plMap<plUInt16, LimbConstructionInfo> limbConstructionInfos(plFrameAllocator::GetCurrentAllocator());
  limbConstructionInfos.FindOrAdd(plInvalidJointIndex); // dummy root link

  plUInt16 uiLastLimbIdx = plInvalidJointIndex;
  plHybridArray<const plSkeletonResourceGeometry*, 8> geometries;

  for (const auto& geo : skeletonResource.GetDescriptor().m_Geometry)
  {
    if (geo.m_Type == plSkeletonJointGeometryType::None)
      continue;

    if (geo.m_uiAttachedToJoint != uiLastLimbIdx)
    {
      CreateLimb(skeletonResource, limbConstructionInfos, geometries, pose, worldModule, fObjectScale, pRagdollSettings);
      geometries.Clear();
      uiLastLimbIdx = geo.m_uiAttachedToJoint;
    }

    geometries.PushBack(&geo);
  }

  CreateLimb(skeletonResource, limbConstructionInfos, geometries, pose, worldModule, fObjectScale, pRagdollSettings);
}

void plJoltRagdollComponent::CreateLimb(const plSkeletonResource& skeletonResource, plMap<plUInt16, LimbConstructionInfo>& limbConstructionInfos, plArrayPtr<const plSkeletonResourceGeometry*> geometries, const plMsgAnimationPoseUpdated& pose, plJoltWorldModule& worldModule, float fObjectScale, JPH::RagdollSettings* pRagdollSettings)
{
  if (geometries.IsEmpty())
    return;

  const plSkeleton& skeleton = skeletonResource.GetDescriptor().m_Skeleton;

  const plUInt16 uiThisJointIdx = geometries[0]->m_uiAttachedToJoint;
  const plSkeletonJoint& thisLimbJoint = skeleton.GetJointByIndex(uiThisJointIdx);
  plUInt16 uiParentJointIdx = thisLimbJoint.GetParentIndex();

  // find the parent joint that is also part of the ragdoll
  while (!limbConstructionInfos.Contains(uiParentJointIdx))
  {
    uiParentJointIdx = skeleton.GetJointByIndex(uiParentJointIdx).GetParentIndex();
  }
  // now uiParentJointIdx is either the index of a limb that has been created before, or plInvalidJointIndex

  LimbConstructionInfo& thisLimbInfo = limbConstructionInfos[uiThisJointIdx];
  const LimbConstructionInfo& parentLimbInfo = limbConstructionInfos[uiParentJointIdx];

  thisLimbInfo.m_uiJoltPartIndex = (plUInt16)pRagdollSettings->mParts.size();
  pRagdollSettings->mParts.resize(pRagdollSettings->mParts.size() + 1);

  m_Limbs[uiThisJointIdx].m_uiPartIndex = thisLimbInfo.m_uiJoltPartIndex;

  pRagdollSettings->mSkeleton->AddJoint(thisLimbJoint.GetName().GetData(), parentLimbInfo.m_uiJoltPartIndex != plInvalidJointIndex ? parentLimbInfo.m_uiJoltPartIndex : -1);

  ComputeLimbGlobalTransform(thisLimbInfo.m_GlobalTransform, pose, uiThisJointIdx);
  ConfigureRagdollPart(&pRagdollSettings->mParts[thisLimbInfo.m_uiJoltPartIndex], thisLimbInfo.m_GlobalTransform, thisLimbJoint.GetCollisionLayer(), worldModule);
  CreateAllLimbGeoShapes(thisLimbInfo, geometries, thisLimbJoint, skeletonResource, fObjectScale, pRagdollSettings);
}

JPH::Shape* plJoltRagdollComponent::CreateLimbGeoShape(const LimbConstructionInfo& limbConstructionInfo, const plSkeletonResourceGeometry& geo, const plJoltMaterial* pJoltMaterial, const plQuat& qBoneDirAdjustment, const plTransform& skeletonRootTransform, plTransform& out_shapeTransform, float fObjectScale)
{
  out_shapeTransform.SetIdentity();
  out_shapeTransform.m_vPosition = qBoneDirAdjustment * geo.m_Transform.m_vPosition * fObjectScale;
  out_shapeTransform.m_qRotation = qBoneDirAdjustment * geo.m_Transform.m_qRotation;

  JPH::Ref<JPH::Shape> pShape;

  switch (geo.m_Type)
  {
    case plSkeletonJointGeometryType::Sphere:
    {
      JPH::SphereShapeSettings shape;
      shape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
      shape.mMaterial = pJoltMaterial;
      shape.mRadius = geo.m_Transform.m_vScale.z * fObjectScale;

      pShape = shape.Create().Get();
    }
    break;

    case plSkeletonJointGeometryType::Box:
    {
      JPH::BoxShapeSettings shape;
      shape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
      shape.mMaterial = pJoltMaterial;
      plVec3 vHalfSize = geo.m_Transform.m_vScale * 0.5f * fObjectScale;
      vHalfSize.x = plMath::Max(vHalfSize.x, JPH::cDefaultConvexRadius);
      vHalfSize.y = plMath::Max(vHalfSize.y, JPH::cDefaultConvexRadius);
      vHalfSize.z = plMath::Max(vHalfSize.z, JPH::cDefaultConvexRadius);
      shape.mHalfExtent = plJoltConversionUtils::ToVec3(vHalfSize);

      out_shapeTransform.m_vPosition += qBoneDirAdjustment * plVec3(geo.m_Transform.m_vScale.x * 0.5f * fObjectScale, 0, 0);

      pShape = shape.Create().Get();
    }
    break;

    case plSkeletonJointGeometryType::Capsule:
    {
      JPH::CapsuleShapeSettings shape;
      shape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
      shape.mMaterial = pJoltMaterial;
      shape.mHalfHeightOfCylinder = geo.m_Transform.m_vScale.x * 0.5f * fObjectScale;
      shape.mRadius = geo.m_Transform.m_vScale.z * fObjectScale;

      plQuat qRot = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisZ(), plAngle::MakeFromDegree(-90));
      out_shapeTransform.m_qRotation = out_shapeTransform.m_qRotation * qRot;
      out_shapeTransform.m_vPosition += qBoneDirAdjustment * plVec3(geo.m_Transform.m_vScale.x * 0.5f * fObjectScale, 0, 0);

      pShape = shape.Create().Get();
    }
    break;

    case plSkeletonJointGeometryType::CapsuleSideways:
    {
      JPH::CapsuleShapeSettings shape;
      shape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
      shape.mMaterial = pJoltMaterial;
      shape.mHalfHeightOfCylinder = geo.m_Transform.m_vScale.x * 0.5f * fObjectScale;
      shape.mRadius = geo.m_Transform.m_vScale.z * fObjectScale;

      // plQuat qRot = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisZ(), plAngle::MakeFromDegree(-90));
      out_shapeTransform.m_qRotation = out_shapeTransform.m_qRotation; // *qRot;
      // out_shapeTransform.m_vPosition += qBoneDirAdjustment * plVec3(geo.m_Transform.m_vScale.x * 0.5f * fObjectScale, 0, 0);

      pShape = shape.Create().Get();
    }
    break;

    case plSkeletonJointGeometryType::ConvexMesh:
    {
      // convex mesh vertices are in "global space" of the mesh file format
      // so first move them into global space of the PL convention (skeletonRootTransform)
      // then move them to the global position of the ragdoll object
      // then apply the inverse global transform of the limb, to move everything into local space of the limb

      out_shapeTransform = limbConstructionInfo.m_GlobalTransform.GetInverse() * GetOwner()->GetGlobalTransform() * skeletonRootTransform;
      out_shapeTransform.m_vPosition *= fObjectScale;

      plHybridArray<JPH::Vec3, 256> verts;
      verts.SetCountUninitialized(geo.m_VertexPositions.GetCount());

      for (plUInt32 i = 0; i < verts.GetCount(); ++i)
      {
        verts[i] = plJoltConversionUtils::ToVec3(geo.m_VertexPositions[i] * fObjectScale);
      }

      JPH::ConvexHullShapeSettings shape(verts.GetData(), (int)verts.GetCount());
      shape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);
      shape.mMaterial = pJoltMaterial;

      const auto shapeRes = shape.Create();

      if (shapeRes.HasError())
      {
        plLog::Error("Cooking convex ragdoll piece failed: {}", shapeRes.GetError().c_str());
        return nullptr;
      }

      pShape = shapeRes.Get();
    }
    break;

      PL_DEFAULT_CASE_NOT_IMPLEMENTED;
  }

  pShape->AddRef();
  return pShape;
}

void plJoltRagdollComponent::CreateAllLimbGeoShapes(const LimbConstructionInfo& limbConstructionInfo, plArrayPtr<const plSkeletonResourceGeometry*> geometries, const plSkeletonJoint& thisLimbJoint, const plSkeletonResource& skeletonResource, float fObjectScale, JPH::RagdollSettings* pRagdollSettings)
{
  const plJoltMaterial* pJoltMaterial = plJoltCore::GetDefaultMaterial();

  if (thisLimbJoint.GetSurface().IsValid())
  {
    plResourceLock<plSurfaceResource> pSurface(thisLimbJoint.GetSurface(), plResourceAcquireMode::BlockTillLoaded);

    if (pSurface->m_pPhysicsMaterialJolt != nullptr)
    {
      pJoltMaterial = static_cast<plJoltMaterial*>(pSurface->m_pPhysicsMaterialJolt);
    }
  }

  const plTransform& skeletonRootTransform = skeletonResource.GetDescriptor().m_RootTransform;

  const auto srcBoneDir = skeletonResource.GetDescriptor().m_Skeleton.m_BoneDirection;
  const plQuat qBoneDirAdjustment = plBasisAxis::GetBasisRotation(plBasisAxis::PositiveX, srcBoneDir);

  JPH::RagdollSettings::Part* pBodyDesc = &pRagdollSettings->mParts[limbConstructionInfo.m_uiJoltPartIndex];

  if (geometries.GetCount() > 1)
  {
    JPH::StaticCompoundShapeSettings compound;

    for (const plSkeletonResourceGeometry* pGeo : geometries)
    {
      plTransform shapeTransform;
      if (JPH::Shape* pSubShape = CreateLimbGeoShape(limbConstructionInfo, *pGeo, pJoltMaterial, qBoneDirAdjustment, skeletonRootTransform, shapeTransform, fObjectScale))
      {
        compound.AddShape(plJoltConversionUtils::ToVec3(shapeTransform.m_vPosition), plJoltConversionUtils::ToQuat(shapeTransform.m_qRotation), pSubShape);
        pSubShape->Release(); // had to manual AddRef once
      }
    }

    const auto compoundRes = compound.Create();
    if (!compoundRes.IsValid())
    {
      plLog::Error("Creating a compound shape for a ragdoll failed: {}", compoundRes.GetError().c_str());
      return;
    }

    pBodyDesc->SetShape(compoundRes.Get());
  }
  else
  {
    plTransform shapeTransform;
    JPH::Shape* pSubShape = CreateLimbGeoShape(limbConstructionInfo, *geometries[0], pJoltMaterial, qBoneDirAdjustment, skeletonRootTransform, shapeTransform, fObjectScale);

    if (!shapeTransform.IsEqual(plTransform::MakeIdentity(), 0.001f))
    {
      JPH::RotatedTranslatedShapeSettings outerShape;
      outerShape.mInnerShapePtr = pSubShape;
      outerShape.mPosition = plJoltConversionUtils::ToVec3(shapeTransform.m_vPosition);
      outerShape.mRotation = plJoltConversionUtils::ToQuat(shapeTransform.m_qRotation);
      outerShape.mUserData = reinterpret_cast<plUInt64>(m_pJoltUserData);

      pBodyDesc->SetShape(outerShape.Create().Get());
    }
    else
    {
      pBodyDesc->SetShape(pSubShape);
    }

    pSubShape->Release(); // had to manual AddRef once
  }
}



//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////


void plJoltRagdollComponent::SetupLimbJoints(const plSkeletonResource* pSkeleton, JPH::RagdollSettings* pRagdollSettings)
{
  // TODO: still needed ? (it should be)
  // the main direction of Jolt bones is +X (for bone limits and such)
  // therefore the main direction of the source bones has to be adjusted
  // const auto srcBoneDir = pSkeleton->GetDescriptor().m_Skeleton.m_BoneDirection;
  // const plQuat qBoneDirAdjustment = -plBasisAxis::GetBasisRotation(srcBoneDir, plBasisAxis::PositiveX);

  const auto& skeleton = pSkeleton->GetDescriptor().m_Skeleton;

  for (plUInt32 uiLimbIdx = 0; uiLimbIdx < m_Limbs.GetCount(); ++uiLimbIdx)
  {
    const auto& thisLimb = m_Limbs[uiLimbIdx];

    if (thisLimb.m_uiPartIndex == plInvalidJointIndex)
      continue;

    const plSkeletonJoint& thisJoint = skeleton.GetJointByIndex(uiLimbIdx);
    plUInt16 uiParentLimb = thisJoint.GetParentIndex();
    while (uiParentLimb != plInvalidJointIndex && m_Limbs[uiParentLimb].m_uiPartIndex == plInvalidJointIndex)
    {
      uiParentLimb = skeleton.GetJointByIndex(uiParentLimb).GetParentIndex();
    }

    if (uiParentLimb == plInvalidJointIndex)
      continue;

    const auto& parentLimb = m_Limbs[uiParentLimb];

    CreateLimbJoint(thisJoint, &pRagdollSettings->mParts[parentLimb.m_uiPartIndex], &pRagdollSettings->mParts[thisLimb.m_uiPartIndex]);
  }
}

void plJoltRagdollComponent::CreateLimbJoint(const plSkeletonJoint& thisJoint, void* pParentBodyDesc, void* pThisBodyDesc)
{
  plEnum<plSkeletonJointType> jointType = thisJoint.GetJointType();

  for (plUInt32 i = 0; i < m_JointOverrides.GetCount(); ++i)
  {
    if (m_JointOverrides[i].m_sJointName == thisJoint.GetName())
    {
      jointType = m_JointOverrides[i].m_JointType;
      break;
    }
  }

  if (jointType == plSkeletonJointType::None)
    return;

  JPH::RagdollSettings::Part* pLink = reinterpret_cast<JPH::RagdollSettings::Part*>(pThisBodyDesc);
  JPH::RagdollSettings::Part* pParentLink = reinterpret_cast<JPH::RagdollSettings::Part*>(pParentBodyDesc);

  plTransform tParent = plJoltConversionUtils::ToTransform(pParentLink->mPosition, pParentLink->mRotation);
  plTransform tThis = plJoltConversionUtils::ToTransform(pLink->mPosition, pLink->mRotation);

  if (jointType == plSkeletonJointType::Fixed)
  {
    JPH::FixedConstraintSettings* pJoint = new JPH::FixedConstraintSettings();
    pLink->mToParent = pJoint;

    pJoint->mDrawConstraintSize = 0.1f;
    pJoint->mPoint1 = pLink->mPosition;
    pJoint->mPoint2 = pLink->mPosition;
  }

  if (jointType == plSkeletonJointType::SwingTwist)
  {
    JPH::SwingTwistConstraintSettings* pJoint = new JPH::SwingTwistConstraintSettings();
    pLink->mToParent = pJoint;

    const plQuat offsetRot = thisJoint.GetLocalOrientation();

    pJoint->mSpace = JPH::EConstraintSpace::WorldSpace;
    pJoint->mDrawConstraintSize = 0.15f;
    pJoint->mPosition1 = pLink->mPosition;
    pJoint->mPosition2 = pLink->mPosition;
    pJoint->mNormalHalfConeAngle = thisJoint.GetHalfSwingLimitZ().GetRadian();
    pJoint->mPlaneHalfConeAngle = thisJoint.GetHalfSwingLimitY().GetRadian();
    pJoint->mTwistMinAngle = thisJoint.GetTwistLimitLow().GetRadian();
    pJoint->mTwistMaxAngle = thisJoint.GetTwistLimitHigh().GetRadian();
    pJoint->mMaxFrictionTorque = m_fStiffnessFactor * thisJoint.GetStiffness();
    pJoint->mPlaneAxis1 = plJoltConversionUtils::ToVec3(tParent.m_qRotation * offsetRot * plVec3::MakeAxisZ()).Normalized();
    pJoint->mPlaneAxis2 = plJoltConversionUtils::ToVec3(tThis.m_qRotation * plVec3::MakeAxisZ()).Normalized();
    pJoint->mTwistAxis1 = plJoltConversionUtils::ToVec3(tParent.m_qRotation * offsetRot * plVec3::MakeAxisY()).Normalized();
    pJoint->mTwistAxis2 = plJoltConversionUtils::ToVec3(tThis.m_qRotation * plVec3::MakeAxisY()).Normalized();

    pJoint->mSwingMotorSettings.mSpringSettings.mFrequency = 20;
    pJoint->mSwingMotorSettings.mSpringSettings.mStiffness = 20;
    pJoint->mSwingMotorSettings.mSpringSettings.mDamping = 2;

    pJoint->mSwingMotorSettings.SetForceLimit(m_fMotorStrength);
    pJoint->mTwistMotorSettings.SetForceLimit(m_fMotorStrength);

    // torque is needed for the 'powered' animation mode
    pJoint->mSwingMotorSettings.SetTorqueLimit(m_fMotorStrength);
    pJoint->mTwistMotorSettings.SetTorqueLimit(m_fMotorStrength);
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltRagdollComponent);
