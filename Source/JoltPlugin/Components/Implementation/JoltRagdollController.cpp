#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Math/Rect.h>
#include <cstdio>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <JoltPlugin/Components/JoltRagdollController.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>
#include <RendererCore/AnimationSystem/AnimationPose.h>
#include <RendererCore/AnimationSystem/HumanoidAvatar.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Debug/DebugRenderer.h>

using namespace JPH;

PL_DEFINE_AS_POD_TYPE(JPH::Quat);

namespace
{
  PL_ALWAYS_INLINE float Deg(float f) { return plAngle::DegToRad(f); }

  constexpr float s_fShiftRad = 0.26f;
  constexpr float s_fGiveUpTime = 0.45f; ///< s over which a failed balance hands the body over to the fall
  constexpr float s_fGripStretch = 0.25f; ///< m a gripping hand can be dragged off its hold before it tears loose     ///< how far past the old limit a dislocated joint's resting target sits (~15°)
  constexpr float s_fDislocationSlackRad = 0.44f; ///< how far a dislocated joint's range opens beyond the original, every direction (~25°)

  constexpr float s_fProjectionToleranceRad = 0.035f; ///< ~2°: past its envelope by more, a joint is moved back directly

  constexpr float s_fGuardStart = 0.35f; ///< seconds of protection before the arms start to come in to the guard
  constexpr float s_fGuardRamp = 0.35f;  ///< seconds from the start of the guard to its full blend
  constexpr float s_fGuardMax = 0.7f;    ///< how far the brace pose moves towards the guard on the longest falls

  /// The hard envelope: no joint, dislocated or not, bends further than this from the middle of its range. Self collision is
  /// off by default, so these angles are what keeps a dislocated limb out of the body.
  void GetHardEnvelope(plJoltRagdollRegion::Enum region, float& out_fSwing, float& out_fTwist)
  {
    switch (region)
    {
      case plJoltRagdollRegion::UpperArm:
        out_fSwing = Deg(120.0f);
        out_fTwist = Deg(80.0f);
        return;
      case plJoltRagdollRegion::LowerArm:
        out_fSwing = Deg(100.0f);
        out_fTwist = Deg(60.0f);
        return;
      case plJoltRagdollRegion::Shin:
        out_fSwing = Deg(90.0f);
        out_fTwist = Deg(30.0f);
        return;
      default:
        out_fSwing = Deg(90.0f);
        out_fTwist = Deg(45.0f);
        return;
    }
  }

  /// A direction that starts pointing down and is turned towards the front by fAngleDeg, then outwards by fOutDeg.
  PL_ALWAYS_INLINE void Down(float fAngleDeg, float fOutDeg, float& f, float& u, float& o)
  {
    const float a = Deg(fAngleDeg), b = Deg(fOutDeg);
    f = plMath::Sin(plAngle::MakeFromRadian(a)) * plMath::Cos(plAngle::MakeFromRadian(b));
    u = -plMath::Cos(plAngle::MakeFromRadian(a)) * plMath::Cos(plAngle::MakeFromRadian(b));
    o = plMath::Sin(plAngle::MakeFromRadian(b));
  }

  /// A direction that starts pointing up and is tilted to the front by fTiltDeg.
  PL_ALWAYS_INLINE void Up(float fTiltDeg, float& f, float& u, float& o)
  {
    f = plMath::Sin(plAngle::MakeFromDegree(fTiltDeg));
    u = plMath::Cos(plAngle::MakeFromDegree(fTiltDeg));
    o = 0.0f;
  }

  PL_ALWAYS_INLINE Quat SafeFromTo(Vec3Arg from, Vec3Arg to)
  {
    if (from.LengthSq() < 1e-8f || to.LengthSq() < 1e-8f)
      return Quat::sIdentity();
    return Quat::sFromTo(from.Normalized(), to.Normalized());
  }

  PL_ALWAYS_INLINE Vec3 AnyPerpendicular(Vec3Arg v) { return v.GetNormalizedPerpendicular(); }

  /// Inverse inertia of one body about a world axis.
  PL_ALWAYS_INLINE float InvInertiaAlong(const Body* pBody, Vec3Arg vAxis) { return plMath::Max(vAxis.Dot(pBody->GetInverseInertia().Multiply3x3(vAxis)), 1e-6f); }

  /// Effective inertia of the relative rotation of two bodies about a world axis: what an internal impulse pair acts against.
  PL_ALWAYS_INLINE float PairInertia(const Body* pBody1, const Body* pBody2, Vec3Arg vAxis) { return 1.0f / (InvInertiaAlong(pBody1, vAxis) + InvInertiaAlong(pBody2, vAxis)); }

  /// Normalised lerp on the shorter arc: close enough to a slerp for blending two poses, at a fraction of the cost.
  PL_ALWAYS_INLINE Quat Blend(QuatArg a, QuatArg b, float t)
  {
    const Vec4 vB = a.Dot(b) < 0.0f ? -b.GetXYZW() : b.GetXYZW();
    return Quat(a.GetXYZW() * (1.0f - t) + vB * t).Normalized();
  }

  plHumanoidBone::Enum ChildBone(plHumanoidBone::Enum bone)
  {
    switch (bone)
    {
      case plHumanoidBone::Hips: return plHumanoidBone::Spine;
      case plHumanoidBone::Spine: return plHumanoidBone::MiddleSpine;
      case plHumanoidBone::MiddleSpine: return plHumanoidBone::Chest;
      case plHumanoidBone::Chest: return plHumanoidBone::Neck;
      case plHumanoidBone::Neck: return plHumanoidBone::Head;
      case plHumanoidBone::LeftUpperLeg: return plHumanoidBone::LeftLowerLeg;
      case plHumanoidBone::LeftLowerLeg: return plHumanoidBone::LeftFoot;
      case plHumanoidBone::LeftFoot: return plHumanoidBone::LeftToes;
      case plHumanoidBone::RightUpperLeg: return plHumanoidBone::RightLowerLeg;
      case plHumanoidBone::RightLowerLeg: return plHumanoidBone::RightFoot;
      case plHumanoidBone::RightFoot: return plHumanoidBone::RightToes;
      case plHumanoidBone::LeftShoulder: return plHumanoidBone::LeftUpperArm;
      case plHumanoidBone::LeftUpperArm: return plHumanoidBone::LeftLowerArm;
      case plHumanoidBone::LeftLowerArm: return plHumanoidBone::LeftHand;
      case plHumanoidBone::RightShoulder: return plHumanoidBone::RightUpperArm;
      case plHumanoidBone::RightUpperArm: return plHumanoidBone::RightLowerArm;
      case plHumanoidBone::RightLowerArm: return plHumanoidBone::RightHand;
      default: return plHumanoidBone::COUNT;
    }
  }

  plJoltRagdollRegion::Enum RegionOfBone(plHumanoidBone::Enum bone)
  {
    switch (bone)
    {
      case plHumanoidBone::Hips: return plJoltRagdollRegion::Pelvis;
      case plHumanoidBone::Spine:
      case plHumanoidBone::MiddleSpine: return plJoltRagdollRegion::Spine;
      case plHumanoidBone::Chest:
      case plHumanoidBone::LeftShoulder:
      case plHumanoidBone::RightShoulder: return plJoltRagdollRegion::Chest;
      case plHumanoidBone::Neck: return plJoltRagdollRegion::Neck;
      case plHumanoidBone::Head: return plJoltRagdollRegion::Head;
      case plHumanoidBone::LeftUpperLeg:
      case plHumanoidBone::RightUpperLeg: return plJoltRagdollRegion::Thigh;
      case plHumanoidBone::LeftLowerLeg:
      case plHumanoidBone::RightLowerLeg: return plJoltRagdollRegion::Shin;
      case plHumanoidBone::LeftFoot:
      case plHumanoidBone::RightFoot:
      case plHumanoidBone::LeftToes:
      case plHumanoidBone::RightToes: return plJoltRagdollRegion::Foot;
      case plHumanoidBone::LeftUpperArm:
      case plHumanoidBone::RightUpperArm: return plJoltRagdollRegion::UpperArm;
      case plHumanoidBone::LeftLowerArm:
      case plHumanoidBone::RightLowerArm: return plJoltRagdollRegion::LowerArm;
      case plHumanoidBone::LeftHand:
      case plHumanoidBone::RightHand: return plJoltRagdollRegion::Hand;
      default: return plJoltRagdollRegion::Other;
    }
  }

  plInt8 SideOfBone(plHumanoidBone::Enum bone)
  {
    if (bone >= plHumanoidBone::LeftUpperLeg && bone <= plHumanoidBone::LeftToes)
      return -1;
    if (bone >= plHumanoidBone::RightUpperLeg && bone <= plHumanoidBone::RightToes)
      return 1;
    if (bone >= plHumanoidBone::LeftShoulder && bone <= plHumanoidBone::LeftHand)
      return -1;
    if (bone >= plHumanoidBone::RightShoulder && bone <= plHumanoidBone::RightHand)
      return 1;
    return 0;
  }

  bool IsUpperBody(plJoltRagdollRegion::Enum region)
  {
    return region != plJoltRagdollRegion::Thigh && region != plJoltRagdollRegion::Shin && region != plJoltRagdollRegion::Foot && region != plJoltRagdollRegion::Pelvis;
  }

  /// Ignores the bodies of one ragdoll (same collision group) and sensors.
  class IgnoreGroupFilter : public BodyFilter
  {
  public:
    explicit IgnoreGroupFilter(plUInt32 uiGroup)
      : m_uiGroup(uiGroup)
    {
    }

    virtual bool ShouldCollideLocked(const Body& body) const override
    {
      return !body.IsSensor() && body.GetCollisionGroup().GetGroupID() != m_uiGroup;
    }

    plUInt32 m_uiGroup;
  };
} // namespace

plJoltRagdollBehaviour::StorageType plJoltRagdollBehaviour::FromName(plStringView sName)
{
  if (sName.IsEqual_NoCase("NeutralPose")) return NeutralPose;
  if (sName.IsEqual_NoCase("ImpactSpin")) return ImpactSpin;
  if (sName.IsEqual_NoCase("FallPose")) return FallPose;
  if (sName.IsEqual_NoCase("ProtectFace")) return ProtectFace;
  if (sName.IsEqual_NoCase("RollGuard")) return RollGuard;
  if (sName.IsEqual_NoCase("Injury")) return Injury;
  if (sName.IsEqual_NoCase("Balance")) return Balance;
  return 0;
}

plJoltRagdollController::plJoltRagdollController()
{
  for (auto& p : m_PartForBone)
    p = -1;
}

plJoltRagdollController::~plJoltRagdollController()
{
  PL_ASSERT_DEV(m_Replacements.IsEmpty(), "Shutdown() was not called");
}

bool plJoltRagdollController::Init(Ragdoll* pRagdoll, PhysicsSystem* pSystem, const plSkeletonResourceDescriptor& skeleton, plArrayPtr<const plUInt16> partToJoint, plUInt32 uiCollisionGroup, plUInt32 uiSeed)
{
  m_pRagdoll = pRagdoll;
  m_pSystem = pSystem;
  m_uiCollisionGroup = uiCollisionGroup;
  m_uiSeed = uiSeed;
  m_Rng.Initialize(uiSeed + 1);

  const RagdollSettings* pSettings = pRagdoll->GetRagdollSettings();
  const plUInt32 uiNumParts = (plUInt32)pRagdoll->GetBodyCount();

  m_Parts.SetCount(uiNumParts);
  bool bAnyConstraint = false;

  for (plUInt32 p = 0; p < uiNumParts; ++p)
  {
    Part& part = m_Parts[p];
    part.m_uiJoint = partToJoint[p];
    part.m_iParent = (plInt16)pSettings->GetSkeleton()->GetJoint(p).mParentJointIndex;
    part.m_pBody = pSystem->GetBodyLockInterfaceNoLock().TryGetBody(pRagdoll->GetBodyID(p));

    if (part.m_pBody == nullptr)
      return false;

    part.m_fMass = 1.0f / plMath::Max(part.m_pBody->GetMotionProperties()->GetInverseMass(), 1e-6f);
    part.m_fFriction = part.m_pBody->GetFriction();
    m_fTotalMass += part.m_fMass;

    const int iConstraint = pSettings->GetConstraintIndexForBodyIndex(p);
    if (iConstraint >= 0)
    {
      TwoBodyConstraint* pConstraint = pRagdoll->GetConstraint(iConstraint);
      if (pConstraint->GetSubType() == EConstraintSubType::SwingTwist)
      {
        part.m_pOriginal = static_cast<SwingTwistConstraint*>(pConstraint);
        part.m_pConstraint = part.m_pOriginal;
        bAnyConstraint = true;
      }
    }
  }

  if (!bAnyConstraint)
    return false;

  BuildBodyMap(skeleton, partToJoint);
  CacheEnvelopes();
  ComputeRestPose(skeleton);
  ComputeChainProperties();
  RebuildStaticPoses();
  return true;
}

void plJoltRagdollController::Shutdown()
{
  for (Part& part : m_Parts)
  {
    if (part.m_pOriginal && part.m_pConstraint != part.m_pOriginal)
    {
      part.m_pOriginal->SetEnabled(true);
      part.m_pConstraint = part.m_pOriginal;
    }
  }

  for (TwoBodyConstraint* pConstraint : m_Replacements)
  {
    m_pSystem->RemoveConstraint(pConstraint);
    pConstraint->Release();
  }

  m_Replacements.Clear();

  for (int s = 0; s < 2; ++s)
    ReleaseGrip(s, false);
  m_pRagdoll = nullptr;
}

void plJoltRagdollController::SetProfile(const plJoltRagdollProfileDescriptor& profile)
{
  m_Profile = profile;

  if (m_pRagdoll)
  {
    RebuildStaticPoses();
  }
}

//////////////////////////////////////////////////////////////////////////
// Body map and rest pose

void plJoltRagdollController::BuildBodyMap(const plSkeletonResourceDescriptor& skeletonDesc, plArrayPtr<const plUInt16> partToJoint)
{
  const plSkeleton& skeleton = skeletonDesc.m_Skeleton;
  const plHumanoidAvatarDescriptor& avatar = skeletonDesc.m_HumanoidAvatar;

  plUInt16 jointForBone[plHumanoidBone::COUNT];
  for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
    jointForBone[b] = plInvalidJointIndex;

  if (avatar.IsValidHumanoid())
  {
    for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
      jointForBone[b] = avatar.m_JointIndices[b];
  }
  else
  {
    // no humanoid mapping on the skeleton asset: try the same auto-detection the skeleton editor uses
    plHybridArray<plString, plHumanoidBone::COUNT> names;
    names.SetCount(plHumanoidBone::COUNT);
    plHumanoidAutoMapBones(skeleton, names);

    for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
    {
      if (!names[b].IsEmpty())
        jointForBone[b] = skeleton.FindJointByName(plTempHashedString(names[b]));
    }
  }

  plDynamicArray<plUInt8> boneOfJoint;
  boneOfJoint.SetCount(skeleton.GetJointCount(), plHumanoidBone::COUNT);

  for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
  {
    if (jointForBone[b] < skeleton.GetJointCount())
      boneOfJoint[jointForBone[b]] = (plUInt8)b;
  }

  auto Depth = [&](plUInt16 uiJoint)
  {
    plUInt32 d = 0;
    while (uiJoint != plInvalidJointIndex)
    {
      uiJoint = skeleton.GetJointByIndex(uiJoint).GetParentIndex();
      ++d;
    }
    return d;
  };

  for (Part& part : m_Parts)
  {
    // the region comes from the joint itself or the closest mapped ancestor (fingers belong to the hand)
    plUInt16 j = part.m_uiJoint;
    while (j != plInvalidJointIndex && boneOfJoint[j] == plHumanoidBone::COUNT)
      j = skeleton.GetJointByIndex(j).GetParentIndex();

    if (j != plInvalidJointIndex)
    {
      const plHumanoidBone::Enum bone = (plHumanoidBone::Enum)boneOfJoint[j];
      part.m_Region = RegionOfBone(bone);
      part.m_iSide = SideOfBone(bone);
    }

    if (boneOfJoint[part.m_uiJoint] != plHumanoidBone::COUNT)
      part.m_uiBone = boneOfJoint[part.m_uiJoint];
  }

  for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
  {
    const plUInt16 jb = jointForBone[b];
    if (jb == plInvalidJointIndex || jb >= skeleton.GetJointCount())
      continue;

    plUInt32 uiBestDepth = 0xFFFF;

    for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
    {
      Part& part = m_Parts[p];

      if (part.m_uiJoint == jb)
      {
        m_PartForBone[b] = (plInt16)p;
        break;
      }

      // a mapped joint without a body: use the closest body below it in the same region
      if (part.m_Region != RegionOfBone((plHumanoidBone::Enum)b) || part.m_iSide != SideOfBone((plHumanoidBone::Enum)b))
        continue;

      bool bBelow = false;
      for (plUInt16 a = part.m_uiJoint; a != plInvalidJointIndex; a = skeleton.GetJointByIndex(a).GetParentIndex())
      {
        if (a == jb)
        {
          bBelow = true;
          break;
        }
      }

      const plUInt32 d = Depth(part.m_uiJoint);
      if (bBelow && d < uiBestDepth)
      {
        uiBestDepth = d;
        m_PartForBone[b] = (plInt16)p;
      }
    }

    if (m_PartForBone[b] >= 0 && m_Parts[m_PartForBone[b]].m_uiBone == plHumanoidBone::COUNT)
      m_Parts[m_PartForBone[b]].m_uiBone = (plUInt8)b;
  }

  m_bHumanoid = GetPart(plHumanoidBone::Hips) >= 0 && GetPart(plHumanoidBone::LeftUpperLeg) >= 0 && GetPart(plHumanoidBone::RightUpperLeg) >= 0 &&
                GetPart(plHumanoidBone::LeftLowerLeg) >= 0 && GetPart(plHumanoidBone::RightLowerLeg) >= 0 &&
                (GetPart(plHumanoidBone::Head) >= 0 || GetPart(plHumanoidBone::Neck) >= 0 || GetPart(plHumanoidBone::Chest) >= 0);

  {
    plUInt32 uiMapped = 0;
    for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
      uiMapped += m_PartForBone[b] >= 0 ? 1 : 0;

    if (m_bHumanoid)
      plLog::Dev("Active ragdoll: {} bodies, {} humanoid body parts mapped.", m_Parts.GetCount(), uiMapped);
    else
      plLog::Warning("Active ragdoll: the skeleton's humanoid map lacks the hips, legs or head ({} body parts mapped). Muscles, damping and soft limits work; the poses, face protection and balance stay off. Map the bones in the skeleton asset's Humanoid section.", uiMapped);
  }

  // remember which joints the rest pose positions are needed for
  for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
  {
    m_bRestJointValid[b] = jointForBone[b] != plInvalidJointIndex && jointForBone[b] < skeleton.GetJointCount();
    m_vRestJointPos[b].SetZero();
    if (m_bRestJointValid[b])
      m_vRestJointPos[b].x = (float)jointForBone[b]; // temporarily stores the joint index, replaced in ComputeRestPose()
  }
}

void plJoltRagdollController::ComputeRestPose(const plSkeletonResourceDescriptor& skeletonDesc)
{
  const plSkeleton& skeleton = skeletonDesc.m_Skeleton;
  const plUInt32 uiNumJoints = skeleton.GetJointCount();

  // the same transforms the ragdoll bodies are built from: model space rest pose through plMsgAnimationPoseUpdated
  plDynamicArray<plMat4> modelTransforms;
  modelTransforms.SetCountUninitialized(uiNumJoints);

  for (plUInt32 i = 0; i < uiNumJoints; ++i)
  {
    const plSkeletonJoint& joint = skeleton.GetJointByIndex(i);
    const plMat4 mLocal = joint.GetRestPoseLocalTransform().GetAsMat4();
    modelTransforms[i] = joint.GetParentIndex() != plInvalidJointIndex ? modelTransforms[joint.GetParentIndex()] * mLocal : mLocal;
  }

  plMsgAnimationPoseUpdated pose;
  pose.m_pRootTransform = &skeletonDesc.m_RootTransform;
  pose.m_pSkeleton = &skeleton;
  pose.m_ModelTransforms = modelTransforms;

  plDynamicArray<plVec3> positions;
  plDynamicArray<plQuat> rotations;
  positions.SetCount(uiNumJoints);
  rotations.SetCount(uiNumJoints);

  for (plUInt32 i = 0; i < uiNumJoints; ++i)
  {
    plMat4 mFull;
    pose.ComputeFullBoneTransform(i, mFull, rotations[i]);
    positions[i] = mFull.GetTranslationVector();
  }

  for (plUInt32 b = 0; b < plHumanoidBone::COUNT; ++b)
  {
    if (m_bRestJointValid[b])
      m_vRestJointPos[b] = positions[(plUInt32)m_vRestJointPos[b].x];
  }

  for (Part& part : m_Parts)
  {
    part.m_qRestGlobal = plJoltConversionUtils::ToQuat(rotations[part.m_uiJoint]).Normalized();
  }

  for (Part& part : m_Parts)
  {
    part.m_qRelRest = part.m_iParent >= 0 ? (m_Parts[part.m_iParent].m_qRestGlobal.Conjugated() * part.m_qRestGlobal).Normalized() : Quat::sIdentity();
  }

  // body frame of the rest pose: up the torso, right towards the right leg, forward where the toes point
  auto P = [&](plHumanoidBone::Enum b) { return plJoltConversionUtils::ToVec3(m_vRestJointPos[b]); };

  if (m_bHumanoid)
  {
    const Vec3 vHips = P(plHumanoidBone::Hips);
    const plHumanoidBone::Enum topBone = m_bRestJointValid[plHumanoidBone::Neck] ? plHumanoidBone::Neck : (m_bRestJointValid[plHumanoidBone::Chest] ? plHumanoidBone::Chest : plHumanoidBone::Head);
    Vec3 vUp = (P(topBone) - vHips).NormalizedOr(Vec3::sAxisZ());
    Vec3 vRight = P(plHumanoidBone::RightUpperLeg) - P(plHumanoidBone::LeftUpperLeg);
    vRight = (vRight - vUp * vRight.Dot(vUp)).NormalizedOr(AnyPerpendicular(vUp));

    // the engine's convention, as in plHumanoidAvatarDescriptor: forward = right x up. Skeletons with toes confirm it below;
    // a skeleton without toes relies on it entirely
    Vec3 vForward = vRight.Cross(vUp).Normalized();

    if (m_bRestJointValid[plHumanoidBone::LeftFoot] && m_bRestJointValid[plHumanoidBone::LeftToes])
    {
      if ((P(plHumanoidBone::LeftToes) - P(plHumanoidBone::LeftFoot)).Dot(vForward) < 0.0f)
        vForward = -vForward;
    }

    m_RestFrame.m_vUp = vUp;
    m_RestFrame.m_vRight = vRight;
    m_RestFrame.m_vForward = vForward;

    const float fThigh = (P(plHumanoidBone::LeftLowerLeg) - P(plHumanoidBone::LeftUpperLeg)).Length();
    const float fShin = m_bRestJointValid[plHumanoidBone::LeftFoot] ? (P(plHumanoidBone::LeftFoot) - P(plHumanoidBone::LeftLowerLeg)).Length() : fThigh;
    m_fLegLength = plMath::Max(0.2f, fThigh + fShin);
  }
}

void plJoltRagdollController::ComputeChainProperties()
{
  const float g = m_pSystem->GetGravity().Length();
  const float fGravity = g > 0.01f ? g : 9.81f;

  Vec3 vBodyCom = Vec3::sZero();
  for (const Part& part : m_Parts)
    vBodyCom += part.m_pBody->GetCenterOfMassPosition() * part.m_fMass;
  vBodyCom /= plMath::Max(m_fTotalMass, 1e-6f);

  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    Part& part = m_Parts[p];
    const Vec3 vJoint = part.m_pBody->GetPosition();

    float fMass = 0.0f;
    Vec3 vCom = Vec3::sZero();

    // parts are ordered parents first, so every descendant has a higher index
    for (plUInt32 c = p; c < m_Parts.GetCount(); ++c)
    {
      bool bInChain = c == p;
      for (plInt16 a = m_Parts[c].m_iParent; !bInChain && a >= 0; a = m_Parts[a].m_iParent)
        bInChain = a == (plInt16)p;

      if (!bInChain)
        continue;

      fMass += m_Parts[c].m_fMass;
      vCom += m_Parts[c].m_pBody->GetCenterOfMassPosition() * m_Parts[c].m_fMass;
    }

    vCom /= plMath::Max(fMass, 1e-6f);

    float fInertia = 0.0f;
    for (plUInt32 c = p; c < m_Parts.GetCount(); ++c)
    {
      bool bInChain = c == p;
      for (plInt16 a = m_Parts[c].m_iParent; !bInChain && a >= 0; a = m_Parts[a].m_iParent)
        bInChain = a == (plInt16)p;

      if (bInChain)
        fInertia += m_Parts[c].m_fMass * ((m_Parts[c].m_pBody->GetCenterOfMassPosition() - vJoint).LengthSq() + 0.002f);
    }

    part.m_fChainMass = fMass;
    part.m_fChainInertia = plMath::Max(fInertia, 1e-4f);
    part.m_fGravityTorque = plMath::Max(fMass * fGravity * (vCom - vJoint).Length(), 0.05f);
    part.m_fSupportInertia = plMath::Max((m_fTotalMass - fMass) * (vBodyCom - vJoint).LengthSq(), part.m_fChainInertia);
  }
}

void plJoltRagdollController::BuildPose(plArrayPtr<const PoseBone> bones, float fVariationDeg, plInt8 iSideOnly, plUInt32 uiPoseSeed, Quat Part::* pTarget)
{
  plRandom rng;
  rng.Initialize(m_uiSeed * 7919u + uiPoseSeed);

  const BodyFrame& f = m_RestFrame;

  plHybridArray<Quat, 64> globals;
  globals.SetCount(m_Parts.GetCount(), Quat::sIdentity());

  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    Part& part = m_Parts[p];
    const PoseBone* pSpec = nullptr;

    if (part.m_uiBone != plHumanoidBone::COUNT && (iSideOnly == 0 || part.m_iSide == iSideOnly))
    {
      for (const PoseBone& pb : bones)
      {
        if (pb.m_uiBone == part.m_uiBone)
        {
          pSpec = &pb;
          break;
        }
      }
    }

    if (pSpec != nullptr && m_bRestJointValid[part.m_uiBone])
    {
      const plHumanoidBone::Enum bone = (plHumanoidBone::Enum)part.m_uiBone;
      const plHumanoidBone::Enum child = ChildBone(bone);
      const Vec3 vJoint = plJoltConversionUtils::ToVec3(m_vRestJointPos[bone]);

      Vec3 vRestDir;
      if (child != plHumanoidBone::COUNT && m_bRestJointValid[child])
        vRestDir = plJoltConversionUtils::ToVec3(m_vRestJointPos[child]) - vJoint;
      else if (RegionOfBone(bone) == plJoltRagdollRegion::Foot)
        vRestDir = f.m_vForward * 0.9f - f.m_vUp * 0.42f;
      else if (RegionOfBone(bone) == plJoltRagdollRegion::Hand && part.m_iParent >= 0 && m_Parts[part.m_iParent].m_uiBone != plHumanoidBone::COUNT)
        vRestDir = vJoint - plJoltConversionUtils::ToVec3(m_vRestJointPos[m_Parts[part.m_iParent].m_uiBone]);
      else
        vRestDir = f.m_vUp;

      const float fSide = (float)part.m_iSide;
      Vec3 vDesired = f.m_vForward * pSpec->m_fForward + f.m_vUp * pSpec->m_fUp + f.m_vRight * (pSpec->m_fOut * fSide);
      vDesired = vDesired.NormalizedOr(vRestDir.NormalizedOr(f.m_vUp));

      if (fVariationDeg > 0.0f)
      {
        const Vec3 vAxis = (AnyPerpendicular(vDesired) * rng.FloatMinMax(-1.0f, 1.0f) + vDesired.Cross(AnyPerpendicular(vDesired)) * rng.FloatMinMax(-1.0f, 1.0f)).NormalizedOr(AnyPerpendicular(vDesired));
        vDesired = Quat::sRotation(vAxis, Deg(rng.FloatMinMax(-fVariationDeg, fVariationDeg))) * vDesired;
      }

      Quat qGlobal = SafeFromTo(vRestDir, vDesired) * part.m_qRestGlobal;

      if (pSpec->m_fTwistDeg != 0.0f)
        qGlobal = Quat::sRotation(vDesired, Deg(pSpec->m_fTwistDeg)) * qGlobal;

      globals[p] = qGlobal.Normalized();
    }
    else
    {
      globals[p] = part.m_iParent >= 0 ? (globals[part.m_iParent] * part.m_qRelRest).Normalized() : part.m_qRestGlobal;
    }
  }

  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    Part& part = m_Parts[p];
    part.*pTarget = part.m_iParent >= 0 ? ClampToLimits(part, (globals[part.m_iParent].Conjugated() * globals[p]).Normalized()) : Quat::sIdentity();
  }
}

Quat plJoltRagdollController::ClampToLimits(const Part& part, const Quat& qRel) const
{
  const SwingTwistConstraint* pConstraint = part.m_pConstraint;
  if (pConstraint == nullptr)
    return qRel;

  // into constraint space: r = C1^-1 * q * C2
  const Quat qC1 = pConstraint->GetConstraintToBody1Matrix().GetQuaternion();
  const Quat qC2 = pConstraint->GetConstraintToBody2Matrix().GetQuaternion();
  Quat r = qC1.Conjugated() * qRel * qC2;
  if (r.GetW() < 0.0f)
    r = -r;

  Quat qSwing, qTwist;
  r.GetSwingTwist(qSwing, qTwist);

  // twist: inside the middle 90% of the range
  const float fMin = pConstraint->GetTwistMinAngle();
  const float fMax = pConstraint->GetTwistMaxAngle();
  const float fMargin = 0.05f * (fMax - fMin);
  const float fTwist = plMath::Clamp(2.0f * plMath::ATan2(qTwist.GetX(), qTwist.GetW()).GetRadian(), fMin + fMargin, fMax - fMargin);
  qTwist = Quat::sRotation(Vec3::sAxisX(), fTwist);

  // swing: at most 90% of the elliptical cone in the swing's direction
  const float fAlpha = 2.0f * plMath::ACos(plMath::Clamp(qSwing.GetW(), -1.0f, 1.0f)).GetRadian();
  const Vec3 vAxis(0.0f, qSwing.GetY(), qSwing.GetZ());
  if (fAlpha > 1e-4f && vAxis.LengthSq() > 1e-10f)
  {
    const Vec3 vA = vAxis.Normalized();
    const float fLimY = plMath::Max(pConstraint->GetPlaneHalfConeAngle(), 1e-3f);
    const float fLimZ = plMath::Max(pConstraint->GetNormalHalfConeAngle(), 1e-3f);
    const float fLimit = 0.9f / plMath::Sqrt(plMath::Square(vA.GetY() / fLimY) + plMath::Square(vA.GetZ() / fLimZ));
    if (fAlpha > fLimit)
      qSwing = Quat::sRotation(vA, fLimit);
  }

  return (qC1 * (qSwing * qTwist) * qC2.Conjugated()).Normalized();
}

void plJoltRagdollController::CacheEnvelopes()
{
  // the original range always fits, even where it is already wider than the cap
  auto Open = [](float fLimit, float fCap) { return plMath::Max(fLimit, plMath::Min(fLimit + s_fDislocationSlackRad, fCap)); };

  for (Part& part : m_Parts)
  {
    const SwingTwistConstraint* pOriginal = part.m_pOriginal;
    if (pOriginal == nullptr)
      continue;

    float fSwingCap, fTwistCap;
    GetHardEnvelope(part.m_Region, fSwingCap, fTwistCap);

    part.m_qOriginalC1 = pOriginal->GetConstraintToBody1Matrix().GetQuaternion();
    part.m_qOriginalC2 = pOriginal->GetConstraintToBody2Matrix().GetQuaternion();
    part.m_fEnvelopePlane = Open(pOriginal->GetPlaneHalfConeAngle(), fSwingCap);
    part.m_fEnvelopeNormal = Open(pOriginal->GetNormalHalfConeAngle(), fSwingCap);
    part.m_fEnvelopeTwistMax = Open(pOriginal->GetTwistMaxAngle(), fTwistCap);
    part.m_fEnvelopeTwistMin = -Open(-pOriginal->GetTwistMinAngle(), fTwistCap);
  }
}

void plJoltRagdollController::GetEnvelopeLimits(const Part& part, float& out_fPlane, float& out_fNormal, float& out_fTwistMin, float& out_fTwistMax) const
{
  out_fPlane = part.m_fEnvelopePlane;
  out_fNormal = part.m_fEnvelopeNormal;
  out_fTwistMin = part.m_fEnvelopeTwistMin;
  out_fTwistMax = part.m_fEnvelopeTwistMax;
}

float plJoltRagdollController::ComputeEnvelopeExcess(const Part& part, float* out_pSwingExcess, Vec3* out_pSwingAxis, float* out_pTwistExcess, Vec3* out_pTwistAxis) const
{
  if (part.m_pOriginal == nullptr || part.m_iParent < 0)
    return 0.0f;

  float fPlane, fNormal, fTwistMin, fTwistMax;
  GetEnvelopeLimits(part, fPlane, fNormal, fTwistMin, fTwistMax);

  // the joint's rotation in the original constraint space, whichever constraint holds it now
  const Quat qBody1 = m_Parts[part.m_iParent].m_pBody->GetRotation();
  const Quat qRel = qBody1.Conjugated() * part.m_pBody->GetRotation();
  const Quat qC1 = part.m_qOriginalC1;
  const Quat qC2 = part.m_qOriginalC2;
  Quat r = (qC1.Conjugated() * qRel * qC2).Normalized();
  if (r.GetW() < 0.0f)
    r = -r;

  Quat qSwing, qTwist;
  r.GetSwingTwist(qSwing, qTwist);
  const Quat qFrame1 = qBody1 * qC1;

  // twist: turning about the frame's X makes it larger
  const float fTwist = 2.0f * plMath::ATan2(qTwist.GetX(), qTwist.GetW()).GetRadian();
  const float fTwistExcess = plMath::Max(0.0f, fTwist - fTwistMax, fTwistMin - fTwist);
  if (out_pTwistExcess)
  {
    *out_pTwistExcess = fTwistExcess;
    *out_pTwistAxis = qFrame1 * (fTwist > 0.0f ? Vec3::sAxisX() : -Vec3::sAxisX());
  }

  // swing: turning further about the swing axis makes it larger
  float fSwingExcess = 0.0f;
  const float fAlpha = 2.0f * plMath::ACos(plMath::Clamp(qSwing.GetW(), -1.0f, 1.0f)).GetRadian();
  const Vec3 vAxis(0.0f, qSwing.GetY(), qSwing.GetZ());
  Vec3 vA = Vec3::sAxisY();
  if (fAlpha > 1e-4f && vAxis.LengthSq() > 1e-10f)
  {
    vA = vAxis.Normalized();
    const float fLimit = 1.0f / plMath::Sqrt(plMath::Square(vA.GetY() / fPlane) + plMath::Square(vA.GetZ() / fNormal));
    fSwingExcess = plMath::Max(0.0f, fAlpha - fLimit);
  }
  if (out_pSwingExcess)
  {
    *out_pSwingExcess = fSwingExcess;
    *out_pSwingAxis = qFrame1 * vA;
  }

  return plMath::Max(fSwingExcess, fTwistExcess);
}

void plJoltRagdollController::EnforceEnvelope(float fDeltaTime)
{
  // The backstop behind the hard envelope. Jolt's solver can overshoot a limit for a few steps under a hard hit, and a
  // dislocated joint has nothing else past its range, so a joint past its envelope gets its outward relative spin removed
  // and is turned back in, a quarter of the overshoot per step.
  constexpr float fMaxReturnSpeed = 10.0f; // rad/s

  for (Part& part : m_Parts)
  {
    if (part.m_pOriginal == nullptr || part.m_iParent < 0)
      continue;

    // Only a joint the soft limits just found at or in its limit zone can be past the envelope: the envelope is never
    // narrower than the constraint the soft limits looked at. Joints that barely turn are skipped there as well, they have
    // no outward spin to remove; the projection after the frame handles their position.
    if (!part.m_bInSoftZone)
      continue;

    Body* pBody1 = m_Parts[part.m_iParent].m_pBody;
    Body* pBody2 = part.m_pBody;

    float fSwingExcess, fTwistExcess;
    Vec3 vSwingAxis, vTwistAxis;
    if (ComputeEnvelopeExcess(part, &fSwingExcess, &vSwingAxis, &fTwistExcess, &vTwistAxis) <= 0.0f)
      continue;

    auto PushBack = [&](Vec3Arg vAxis, float fExcess)
    {
      if (fExcess <= 0.0f)
        return;

      const float fOut = (pBody2->GetAngularVelocity() - pBody1->GetAngularVelocity()).Dot(vAxis);
      const float fTarget = -plMath::Min(0.25f * fExcess / plMath::Max(fDeltaTime, 1e-4f), fMaxReturnSpeed);
      if (fOut <= fTarget)
        return;

      const float fImpulse = PairInertia(pBody1, pBody2, vAxis) * (fOut - fTarget);
      pBody2->AddAngularImpulse(-vAxis * fImpulse);
      pBody1->AddAngularImpulse(vAxis * fImpulse);
    };

    PushBack(vSwingAxis, fSwingExcess);
    PushBack(vTwistAxis, fTwistExcess);
    ++m_uiEnvelopeCorrections;
  }
}

void plJoltRagdollController::ProjectOntoEnvelope(Part& part)
{
  float fPlane, fNormal, fTwistMin, fTwistMax;
  GetEnvelopeLimits(part, fPlane, fNormal, fTwistMin, fTwistMax);

  const Body* pBody1 = m_Parts[part.m_iParent].m_pBody;
  const Body* pBody2 = part.m_pBody;
  const Quat qC1 = part.m_qOriginalC1;
  const Quat qC2 = part.m_qOriginalC2;

  Quat r = (qC1.Conjugated() * pBody1->GetRotation().Conjugated() * pBody2->GetRotation() * qC2).Normalized();
  if (r.GetW() < 0.0f)
    r = -r;

  Quat qSwing, qTwist;
  r.GetSwingTwist(qSwing, qTwist);

  const float fTwist = plMath::Clamp(2.0f * plMath::ATan2(qTwist.GetX(), qTwist.GetW()).GetRadian(), fTwistMin, fTwistMax);
  qTwist = Quat::sRotation(Vec3::sAxisX(), fTwist);

  const float fAlpha = 2.0f * plMath::ACos(plMath::Clamp(qSwing.GetW(), -1.0f, 1.0f)).GetRadian();
  const Vec3 vAxis(0.0f, qSwing.GetY(), qSwing.GetZ());
  if (fAlpha > 1e-4f && vAxis.LengthSq() > 1e-10f)
  {
    const Vec3 vA = vAxis.Normalized();
    const float fLimit = 1.0f / plMath::Sqrt(plMath::Square(vA.GetY() / fPlane) + plMath::Square(vA.GetZ() / fNormal));
    if (fAlpha > fLimit)
      qSwing = Quat::sRotation(vA, fLimit);
  }

  // the rotation that takes the limb from where it is to the envelope's edge, about the joint
  const Quat qTarget = (pBody1->GetRotation() * qC1 * (qSwing * qTwist) * qC2.Conjugated()).Normalized();
  const Quat qDelta = (qTarget * pBody2->GetRotation().Conjugated()).Normalized();
  const RVec3 vPivot = pBody2->GetCenterOfMassTransform() * part.m_pOriginal->GetLocalSpacePosition2();

  // the limb and everything hanging from it turn together, so the joints further down keep their angles
  BodyInterface& bodies = m_pSystem->GetBodyInterface();
  const plInt16 iThis = static_cast<plInt16>(&part - m_Parts.GetData());
  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    bool bBelow = false;
    for (plInt16 i = static_cast<plInt16>(p); i >= 0; i = m_Parts[i].m_iParent)
    {
      if (i == iThis)
      {
        bBelow = true;
        break;
      }
    }

    if (!bBelow)
      continue;

    const Body* pBody = m_Parts[p].m_pBody;
    const RVec3 vPos = vPivot + qDelta * Vec3(pBody->GetPosition() - vPivot);
    bodies.SetPositionAndRotation(pBody->GetID(), vPos, (qDelta * pBody->GetRotation()).Normalized(), EActivation::DontActivate);
  }

  ++m_uiEnvelopeProjections;
}

namespace
{
  struct PoseBuilder
  {
    plHybridArray<plJoltRagdollController::PoseBone, 24>* m_pBones;

    void Add(plHumanoidBone::Enum bone, float f, float u, float o, float fTwist = 0.0f)
    {
      auto& b = m_pBones->ExpandAndGetRef();
      b.m_uiBone = (plUInt8)bone;
      b.m_fForward = f;
      b.m_fUp = u;
      b.m_fOut = o;
      b.m_fTwistDeg = fTwist;
    }

    void AddUp(plHumanoidBone::Enum bone, float fTilt, float fTwist = 0.0f)
    {
      float f, u, o;
      Up(fTilt, f, u, o);
      Add(bone, f, u, o, fTwist);
    }

    void AddDown(plHumanoidBone::Enum bone, float fAngle, float fOut)
    {
      float f, u, o;
      Down(fAngle, fOut, f, u, o);
      Add(bone, f, u, o);
    }

    /// Both sides with the same values.
    void AddDownPair(plHumanoidBone::Enum left, plHumanoidBone::Enum right, float fAngle, float fOut)
    {
      AddDown(left, fAngle, fOut);
      AddDown(right, fAngle, fOut);
    }
  };
} // namespace

void plJoltRagdollController::RebuildStaticPoses()
{
  if (!m_bHumanoid)
  {
    // without a body map the neutral pose is the rest pose
    for (Part& part : m_Parts)
    {
      part.m_qNeutral = part.m_qRelRest;
      part.m_qBrace = part.m_qRelRest;
      part.m_qGuard = part.m_qRelRest;
      part.m_qBackGuard = part.m_qRelRest;
      part.m_qFall = part.m_qRelRest;
      part.m_qSplay = part.m_qRelRest;
      part.m_qCatchBack = part.m_qRelRest;
      part.m_qCatchFront = part.m_qRelRest;
      part.m_qCatchSide = part.m_qRelRest;
      part.m_qStaggerBack = part.m_qRelRest;
      part.m_qStaggerFront = part.m_qRelRest;
      part.m_qLandNear = part.m_qRelRest;
      part.m_qLandFar = part.m_qRelRest;
    }
    return;
  }

  using B = plHumanoidBone;
  plHybridArray<PoseBone, 24> bones;
  PoseBuilder pb{&bones};

  // Neutral: the relaxed float posture. Hips and knees about 50°, ankles relaxed, shoulders rolled forward with the arms
  // forward and slightly out, elbows bent, head tilted forward.
  pb.AddUp(B::Spine, 5.0f);
  pb.AddUp(B::MiddleSpine, 6.0f);
  pb.AddUp(B::Chest, 8.0f);
  pb.AddUp(B::Neck, 18.0f);
  pb.AddUp(B::Head, 24.0f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 50.0f, 6.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, 0.0f, 3.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 45.0f, 0.0f);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 36.0f, 25.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 110.0f, 10.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 110.0f, 10.0f);
  BuildPose(bones, m_Profile.m_fNeutralVariationDeg, 0, 1, &Part::m_qNeutral);

  // Brace: forearms over the head, chin tucked, face turned away, knees pulled up.
  bones.Clear();
  const float fTurn = (m_uiSeed & 1) ? m_Profile.m_fFaceTurnDeg : -m_Profile.m_fFaceTurnDeg;
  pb.AddUp(B::Spine, 15.0f);
  pb.AddUp(B::MiddleSpine, 15.0f);
  pb.AddUp(B::Chest, 20.0f);
  pb.AddUp(B::Neck, 30.0f);
  pb.AddUp(B::Head, 30.0f + m_Profile.m_fChinTuckDeg, fTurn);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 150.0f, 25.0f);
  pb.Add(B::LeftLowerArm, 0.25f, 0.35f, -0.9f);
  pb.Add(B::RightLowerArm, 0.25f, 0.35f, -0.9f);
  pb.Add(B::LeftHand, 0.25f, 0.35f, -0.9f);
  pb.Add(B::RightHand, 0.25f, 0.35f, -0.9f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 85.0f, 8.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -15.0f, 4.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 60.0f, 0.0f);
  BuildPose(bones, 0.0f, 0, 2, &Part::m_qBrace);

  // Guard: the brace for longer falls. Elbows down and tucked against the ribs, forearms up in front of the face with the
  // fists by the chin, the back rounded a little more. Legs as in the brace.
  bones.Clear();
  pb.AddUp(B::Spine, 20.0f);
  pb.AddUp(B::MiddleSpine, 20.0f);
  pb.AddUp(B::Chest, 25.0f);
  pb.AddUp(B::Neck, 35.0f);
  pb.AddUp(B::Head, 35.0f + m_Profile.m_fChinTuckDeg, fTurn);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 35.0f, 6.0f);
  pb.Add(B::LeftLowerArm, 0.35f, 0.9f, -0.3f);
  pb.Add(B::RightLowerArm, 0.35f, 0.9f, -0.3f);
  pb.Add(B::LeftHand, 0.3f, 0.9f, -0.35f);
  pb.Add(B::RightHand, 0.3f, 0.9f, -0.35f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 85.0f, 8.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -15.0f, 4.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 60.0f, 0.0f);
  BuildPose(bones, 0.0f, 0, 2, &Part::m_qGuard);

  // Back guard: going over backwards or sliding on the back. The back rounds and the chin goes to the chest, which lifts
  // the head off the ground; the elbows come up beside the head with the hands cupped behind it; knees up as in the brace.
  bones.Clear();
  pb.AddUp(B::Spine, 25.0f);
  pb.AddUp(B::MiddleSpine, 25.0f);
  pb.AddUp(B::Chest, 30.0f);
  pb.AddUp(B::Neck, 45.0f);
  pb.AddUp(B::Head, 45.0f + m_Profile.m_fChinTuckDeg);
  pb.Add(B::LeftUpperArm, 0.35f, 0.8f, 0.45f);
  pb.Add(B::RightUpperArm, 0.35f, 0.8f, 0.45f);
  pb.Add(B::LeftLowerArm, -0.45f, 0.2f, -0.85f);
  pb.Add(B::RightLowerArm, -0.45f, 0.2f, -0.85f);
  pb.Add(B::LeftHand, -0.3f, 0.3f, -0.9f);
  pb.Add(B::RightHand, -0.3f, 0.3f, -0.9f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 75.0f, 8.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -15.0f, 4.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 60.0f, 0.0f);
  BuildPose(bones, 0.0f, 0, 2, &Part::m_qBackGuard);

  BuildFallPose(0.0f);

  // Splay: arm and leg spread out to the side. Only used for the uphill side while rolling.
  bones.Clear();
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 10.0f, 80.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 10.0f, 80.0f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 15.0f, 35.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, 15.0f, 35.0f);
  BuildPose(bones, 0.0f, 0, 3, &Part::m_qSplay);

  // Catch, going over backwards: the arms reach back and down behind the hips for the ground with the elbows nearly
  // straight, the back rounds a little and the chin goes down so the head does not whip back, and the hips and knees bend
  // to sit the fall down.
  bones.Clear();
  pb.AddUp(B::Spine, 12.0f);
  pb.AddUp(B::MiddleSpine, 12.0f);
  pb.AddUp(B::Chest, 15.0f);
  pb.AddUp(B::Neck, 35.0f);
  pb.AddUp(B::Head, 35.0f + m_Profile.m_fChinTuckDeg);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, -55.0f, 30.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, -40.0f, 25.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, -25.0f, 20.0f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 45.0f, 8.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -15.0f, 4.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 60.0f, 0.0f);
  BuildPose(bones, 0.0f, 0, 5, &Part::m_qCatchBack);

  // Catch, going over forwards: the arms reach forwards and down with soft elbows, the head comes up to keep the face off
  // the ground, the knees give a little.
  bones.Clear();
  pb.AddUp(B::Chest, -5.0f);
  pb.AddUp(B::Neck, -10.0f);
  pb.AddUp(B::Head, -20.0f);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 75.0f, 22.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 65.0f, 18.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 50.0f, 12.0f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 15.0f, 6.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -10.0f, 3.0f);
  pb.AddDownPair(B::LeftFoot, B::RightFoot, 60.0f, 0.0f);
  BuildPose(bones, 0.0f, 0, 6, &Part::m_qCatchFront);

  // Catch, going over sideways: the arm reaches out and down to the side. Only the arm on the falling side takes it.
  bones.Clear();
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 15.0f, 65.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 20.0f, 55.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 20.0f, 45.0f);
  BuildPose(bones, 0.0f, 0, 7, &Part::m_qCatchSide);

  // Stagger, stumbling backwards: the arms are flung forwards and out against the fall with the elbows bent, the body bends
  // at the hips.
  bones.Clear();
  pb.AddUp(B::Spine, 10.0f);
  pb.AddUp(B::MiddleSpine, 10.0f);
  pb.AddUp(B::Chest, 12.0f);
  pb.AddUp(B::Neck, 15.0f);
  pb.AddUp(B::Head, 15.0f);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 65.0f, 45.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 100.0f, 35.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 105.0f, 30.0f);
  BuildPose(bones, 0.0f, 0, 8, &Part::m_qStaggerBack);

  // Stagger, stumbling forwards: the arms go out to the sides and back, the chest comes up.
  bones.Clear();
  pb.AddUp(B::Spine, -5.0f);
  pb.AddUp(B::Chest, -5.0f);
  pb.AddUp(B::Neck, 5.0f);
  pb.AddUp(B::Head, 5.0f);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, -25.0f, 60.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, -10.0f, 55.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 0.0f, 50.0f);
  BuildPose(bones, 0.0f, 0, 9, &Part::m_qStaggerFront);

  // Landing on a side, the side that lands: the arm reaches down and out to take the ground and slide, the leg stays long.
  // The trunk curls a little and the chin tucks, so the head is not what lands. The centre parts take these values.
  bones.Clear();
  pb.AddUp(B::Spine, 12.0f);
  pb.AddUp(B::MiddleSpine, 12.0f);
  pb.AddUp(B::Chest, 15.0f);
  pb.AddUp(B::Neck, 35.0f);
  pb.AddUp(B::Head, 35.0f + m_Profile.m_fChinTuckDeg);
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 10.0f, 70.0f);
  pb.AddDownPair(B::LeftLowerArm, B::RightLowerArm, 15.0f, 60.0f);
  pb.AddDownPair(B::LeftHand, B::RightHand, 15.0f, 50.0f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 15.0f, 15.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -5.0f, 8.0f);
  BuildPose(bones, 0.0f, 0, 10, &Part::m_qLandNear);

  // Landing on a side, the side on top: the arm comes in to guard the head, the knee comes up.
  bones.Clear();
  pb.AddDownPair(B::LeftUpperArm, B::RightUpperArm, 35.0f, 6.0f);
  pb.Add(B::LeftLowerArm, 0.35f, 0.9f, -0.3f);
  pb.Add(B::RightLowerArm, 0.35f, 0.9f, -0.3f);
  pb.Add(B::LeftHand, 0.3f, 0.9f, -0.35f);
  pb.Add(B::RightHand, 0.3f, 0.9f, -0.35f);
  pb.AddDownPair(B::LeftUpperLeg, B::RightUpperLeg, 80.0f, 10.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -10.0f, 4.0f);
  BuildPose(bones, 0.0f, 0, 11, &Part::m_qLandFar);

  for (Part& part : m_Parts)
  {
    if (part.m_bDislocated)
      part.m_qNeutral = part.m_qDislocated;
  }
}

void plJoltRagdollController::BuildFallPose(float fPhase)
{
  if (!m_bHumanoid)
    return;

  using B = plHumanoidBone;
  plHybridArray<PoseBone, 24> bones;
  PoseBuilder pb{&bones};

  // Fall: arms out and windmilling, knees slightly up, the two sides out of phase.
  const float s = plMath::Sin(plAngle::MakeFromRadian(fPhase));
  pb.AddUp(B::Spine, -5.0f);
  pb.AddUp(B::Chest, -5.0f);
  pb.AddUp(B::Neck, 5.0f);
  pb.AddUp(B::Head, 10.0f);
  pb.AddDown(B::LeftUpperArm, 40.0f + 35.0f * s, 70.0f);
  pb.AddDown(B::RightUpperArm, 40.0f - 35.0f * s, 70.0f);
  pb.AddDown(B::LeftLowerArm, 70.0f + 35.0f * s, 55.0f);
  pb.AddDown(B::RightLowerArm, 70.0f - 35.0f * s, 55.0f);
  pb.AddDown(B::LeftUpperLeg, 30.0f - 15.0f * s, 10.0f);
  pb.AddDown(B::RightUpperLeg, 30.0f + 15.0f * s, 10.0f);
  pb.AddDownPair(B::LeftLowerLeg, B::RightLowerLeg, -25.0f, 5.0f);
  BuildPose(bones, 0.0f, 0, 4, &Part::m_qFall);
}

//////////////////////////////////////////////////////////////////////////
// Main thread

void plJoltRagdollController::Prepare(const Inputs& inputs, const SkeletonPose* pAnimPose)
{
  m_Inputs = inputs;
  m_Inputs.m_fFrameTime = plMath::Max(inputs.m_fFrameTime, 1e-4f);
  m_fTimeInFrame = 0.0f;

  m_Hits.PushBackRange(m_PendingHits);
  m_PendingHits.Clear();

  if (pAnimPose != nullptr && pAnimPose->GetJointCount() == (int)m_Parts.GetCount())
  {
    const bool bFirst = !m_bHasAnimTarget;

    for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
    {
      Part& part = m_Parts[p];
      part.m_qAnimPrev = part.m_qAnimCur;

      if (part.m_iParent >= 0)
      {
        const Quat qParent = pAnimPose->GetJointMatrix(part.m_iParent).GetQuaternion();
        const Quat qThis = pAnimPose->GetJointMatrix(p).GetQuaternion();
        part.m_qAnimCur = (qParent.Conjugated() * qThis).Normalized();
      }

      if (bFirst)
        part.m_qAnimPrev = part.m_qAnimCur;
    }

    m_bHasAnimTarget = true;
  }
  else if (!inputs.m_bAnimationDriven)
  {
    m_bHasAnimTarget = false;
  }
}

void plJoltRagdollController::AddHit(const plVec3& vImpulse, const plVec3& vPosition)
{
  if (vImpulse.IsZero(0.0001f))
    return;

  auto& hit = m_PendingHits.ExpandAndGetRef();
  hit.m_vImpulse = plJoltConversionUtils::ToVec3(vImpulse);
  hit.m_vPosition = plJoltConversionUtils::ToVec3(vPosition);
}

void plJoltRagdollController::PostStep(plDynamicArray<plMsgRagdollImpact>& out_impacts, plDynamicArray<plMsgRagdollJointDislocated>& out_dislocations, plDynamicArray<plMsgRagdollBalanceLost>& out_balanceLost, float& out_fConsciousnessDrop)
{
  // dislocations detected during the steps: swap the joint's constraint for a looser one. It keeps the original's frames,
  // so its range has the same middle, and opens by the slack in every direction, but never past the hard envelope.
  for (Part& part : m_Parts)
  {
    if (!part.m_bDislocationPending || part.m_pOriginal == nullptr || part.m_iParent < 0)
      continue;

    part.m_bDislocationPending = false;

    Body* pBody1 = m_Parts[part.m_iParent].m_pBody;
    Body* pBody2 = part.m_pBody;

    const Mat44 mFrame1 = part.m_pOriginal->GetConstraintToBody1Matrix();
    const Mat44 mFrame2 = part.m_pOriginal->GetConstraintToBody2Matrix();
    const Quat qShift = Quat::sRotation(part.m_vDislocationAxis, s_fShiftRad);

    float fPlane, fNormal, fTwistMin, fTwistMax;
    GetEnvelopeLimits(part, fPlane, fNormal, fTwistMin, fTwistMax);

    SwingTwistConstraintSettings settings;
    settings.mSpace = EConstraintSpace::LocalToBodyCOM;
    settings.mPosition1 = RVec3(mFrame1.GetTranslation());
    settings.mPosition2 = RVec3(mFrame2.GetTranslation());
    // Jolt's constraint space: X is the twist axis, Y the normal axis, Z the plane axis
    settings.mTwistAxis1 = mFrame1.GetAxisX();
    settings.mPlaneAxis1 = mFrame1.GetAxisZ();
    settings.mTwistAxis2 = mFrame2.GetAxisX();
    settings.mPlaneAxis2 = mFrame2.GetAxisZ();
    settings.mPlaneHalfConeAngle = fPlane;
    settings.mNormalHalfConeAngle = fNormal;
    settings.mTwistMinAngle = fTwistMin;
    settings.mTwistMaxAngle = fTwistMax;
    settings.mMaxFrictionTorque = part.m_pOriginal->GetMaxFrictionTorque();
    settings.mSwingMotorSettings = part.m_pOriginal->GetSwingMotorSettings();
    settings.mTwistMotorSettings = part.m_pOriginal->GetTwistMotorSettings();

    TwoBodyConstraint* pReplacement = static_cast<TwoBodyConstraint*>(settings.Create(*pBody1, *pBody2));
    pReplacement->AddRef();
    m_pSystem->AddConstraint(pReplacement);
    m_Replacements.PushBack(pReplacement);

    part.m_pOriginal->SetEnabled(false);
    part.m_pConstraint = static_cast<SwingTwistConstraint*>(pReplacement);

    // the limb now rests past its old limit: the current pose turned further by the shift, kept inside the new range
    part.m_qDislocated = ClampToLimits(part, (pBody1->GetRotation().Conjugated() * (qShift * pBody2->GetRotation())).Normalized());
    part.m_qNeutral = part.m_qDislocated;
  }

  // the step listener may only change velocities, so a joint that still ended the frame past its envelope is put back on
  // the envelope's edge here. Parents come before their children in the part list, so a child is checked after its parent
  // has been moved.
  for (Part& part : m_Parts)
  {
    if (ComputeEnvelopeExcess(part) > s_fProjectionToleranceRad)
      ProjectOntoEnvelope(part);
  }

  out_impacts.PushBackRange(m_Impacts);
  out_dislocations.PushBackRange(m_Dislocations);
  out_balanceLost.PushBackRange(m_BalanceLost);
  out_fConsciousnessDrop = m_fConsciousnessDrop;

  m_Impacts.Clear();
  m_Dislocations.Clear();
  m_BalanceLost.Clear();
  m_fConsciousnessDrop = 0.0f;

  // grips are constraints, which can only be added and removed outside the physics update
  UpdateGrips();

  for (Part& part : m_Parts)
    part.m_vPosition = part.m_pBody->GetPosition();

  // the muscles' target for each bone, as a line from the joint: towards the first child joint, or the body's centre for a leaf
  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    Part& part = m_Parts[p];
    part.m_bHasTargetEnd = part.m_iParent >= 0 && part.m_fAccumTorque > 0.02f;
    if (!part.m_bHasTargetEnd)
      continue;

    Vec3 vBone = (part.m_pBody->GetCenterOfMassPosition() - part.m_vPosition) * 2.0f;
    for (const Part& child : m_Parts)
    {
      if (child.m_iParent == (plInt16)p)
      {
        vBone = child.m_pBody->GetPosition() - part.m_vPosition;
        break;
      }
    }

    const Vec3 vLocal = part.m_pBody->GetRotation().Conjugated() * vBone;
    const Quat qTarget = m_Parts[part.m_iParent].m_pBody->GetRotation() * part.m_qAccum.Normalized();
    part.m_vTargetEnd = part.m_vPosition + qTarget * vLocal;
  }
}

void plJoltRagdollController::ResetInjuries()
{
  for (Part& part : m_Parts)
  {
    if (!part.m_bDislocated)
      continue;

    if (part.m_pConstraint != part.m_pOriginal)
    {
      for (plUInt32 i = 0; i < m_Replacements.GetCount(); ++i)
      {
        if (m_Replacements[i] == part.m_pConstraint)
        {
          m_pSystem->RemoveConstraint(m_Replacements[i]);
          m_Replacements[i]->Release();
          m_Replacements.RemoveAtAndSwap(i);
          break;
        }
      }
    }

    part.m_pOriginal->SetEnabled(true);
    part.m_pConstraint = part.m_pOriginal;
    part.m_bDislocated = false;
    part.m_bDislocationPending = false;
  }

  RebuildStaticPoses();
}

plJoltRagdollController::Metrics plJoltRagdollController::ComputeMetrics() const
{
  Metrics m;
  Vec3 vMomentum = Vec3::sZero();
  Vec3 vCom = Vec3::sZero();

  for (const Part& part : m_Parts)
  {
    const Body* pBody = part.m_pBody;
    vMomentum += pBody->GetLinearVelocity() * part.m_fMass;
    vCom += pBody->GetCenterOfMassPosition() * part.m_fMass;
    m.m_fMaxSpeed = plMath::Max(m.m_fMaxSpeed, pBody->GetLinearVelocity().Length());

    if (part.m_pConstraint == nullptr || part.m_iParent < 0)
      continue;

    m.m_fMaxJointSpeed = plMath::Max(m.m_fMaxJointSpeed, (pBody->GetAngularVelocity() - m_Parts[part.m_iParent].m_pBody->GetAngularVelocity()).Length());

    const Quat qRel = (m_Parts[part.m_iParent].m_pBody->GetRotation().Conjugated() * pBody->GetRotation()).Normalized();
    const float fNeutralErr = plAngle::RadToDeg(2.0f * plMath::ACos(plMath::Min(1.0f, plMath::Abs(qRel.Dot(part.m_qNeutral)))).GetRadian());
    m.m_fMaxNeutralErrorDeg = plMath::Max(m.m_fMaxNeutralErrorDeg, fNeutralErr);
    m.m_fMaxEnvelopeExcessDeg = plMath::Max(m.m_fMaxEnvelopeExcessDeg, plAngle::RadToDeg(ComputeEnvelopeExcess(part)));

    // contortion: how far past 80% of its range each joint rests
    Quat q = part.m_pConstraint->GetRotationInConstraintSpace();
    if (q.GetW() < 0.0f)
      q = -q;
    Quat qSwing, qTwist;
    q.GetSwingTwist(qSwing, qTwist);

    const float fAlpha = 2.0f * plMath::ACos(plMath::Clamp(qSwing.GetW(), -1.0f, 1.0f)).GetRadian();
    const Vec3 vAxis = Vec3(0.0f, qSwing.GetY(), qSwing.GetZ());
    if (vAxis.LengthSq() > 1e-8f)
    {
      const Vec3 vA = vAxis.Normalized();
      const float fLimY = plMath::Max(part.m_pConstraint->GetPlaneHalfConeAngle(), 1e-3f);
      const float fLimZ = plMath::Max(part.m_pConstraint->GetNormalHalfConeAngle(), 1e-3f);
      const float fLimit = 1.0f / plMath::Sqrt(plMath::Square(vA.GetY() / fLimY) + plMath::Square(vA.GetZ() / fLimZ));
      m.m_fContortionDeg += plAngle::RadToDeg(plMath::Max(0.0f, fAlpha - 0.8f * fLimit));
    }

    const float fTwist = 2.0f * plMath::ATan2(qTwist.GetX(), qTwist.GetW()).GetRadian();
    const float fMin = part.m_pConstraint->GetTwistMinAngle();
    const float fMax = part.m_pConstraint->GetTwistMaxAngle();
    const float fCentre = 0.5f * (fMin + fMax);
    const float fHalf = 0.5f * (fMax - fMin);
    m.m_fContortionDeg += plAngle::RadToDeg(plMath::Max(0.0f, plMath::Abs(fTwist - fCentre) - 0.8f * fHalf));
  }

  m.m_vMomentum = plJoltConversionUtils::ToVec3(vMomentum);
  m.m_vCom = plJoltConversionUtils::ToVec3(vCom / plMath::Max(m_fTotalMass, 1e-6f));

  using B = plHumanoidBone;
  const B::Enum arms[2] = {B::LeftUpperArm, B::RightUpperArm};
  const B::Enum legs[2] = {B::LeftUpperLeg, B::RightUpperLeg};
  auto RelSpeed = [&](plInt16 iPart)
  {
    const Part& part = m_Parts[iPart];
    const Vec3 vParent = part.m_iParent >= 0 ? m_Parts[part.m_iParent].m_pBody->GetAngularVelocity() : Vec3::sZero();
    return (part.m_pBody->GetAngularVelocity() - vParent).Length();
  };

  for (int i = 0; i < 2; ++i)
  {
    if (GetPart(arms[i]) >= 0)
      m.m_fArmSpeed[i] = RelSpeed(GetPart(arms[i]));
    if (GetPart(legs[i]) >= 0)
      m.m_fLegSpeed[i] = RelSpeed(GetPart(legs[i]));
  }

  if (GetPart(B::Hips) >= 0)
  {
    m.m_vPelvis = plJoltConversionUtils::ToVec3(m_Parts[GetPart(B::Hips)].m_pBody->GetPosition());
    m.m_vPelvisVelocity = plJoltConversionUtils::ToVec3(m_Parts[GetPart(B::Hips)].m_pBody->GetLinearVelocity());
  }

  return m;
}

void plJoltRagdollController::DebugPrint(bool bParts) const
{
  std::printf("    sensors: feet %d %d torso %d airborne %d pelvis height %.2f com (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) capture (%.2f %.2f) state %d steps %u %s\n", m_bFeetContact[0], m_bFeetContact[1], m_bTorsoContact, m_bAirborne, m_fPelvisHeight,
    m_vCom.GetX(), m_vCom.GetY(), m_vCom.GetZ(), m_vComVel.GetX(), m_vComVel.GetY(), m_vComVel.GetZ(), m_vCapturePoint.GetX(), m_vCapturePoint.GetY(), (int)m_BalanceState, m_uiStepsTaken, m_szFallReason);

  std::printf("    consciousness %.2f fall %.2f brace %.2f delay %.2f\n", m_Inputs.m_fConsciousness, m_fFallWeight, m_fBraceWeight, m_fBraceDelay);

  for (const Part& part : m_Parts)
  {
    if (part.m_Region == plJoltRagdollRegion::Foot || part.m_Region == plJoltRagdollRegion::Chest || part.m_Region == plJoltRagdollRegion::Pelvis)
      std::printf("    region %d side %d contact %d\n", (int)part.m_Region, part.m_iSide, part.m_bContact);
  }

  if (!bParts)
    return;

  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    const Part& part = m_Parts[p];
    if (part.m_pConstraint == nullptr || part.m_iParent < 0)
      continue;

    const Quat qRel = (m_Parts[part.m_iParent].m_pBody->GetRotation().Conjugated() * part.m_pBody->GetRotation()).Normalized();
    const float fErr = plAngle::RadToDeg(2.0f * plMath::ACos(plMath::Min(1.0f, plMath::Abs(qRel.Dot(part.m_qNeutral)))).GetRadian());
    const float fRest = plAngle::RadToDeg(2.0f * plMath::ACos(plMath::Min(1.0f, plMath::Abs(part.m_qRelRest.Dot(part.m_qNeutral)))).GetRadian());
    std::printf("    part %2u joint %2u region %2d side %2d bone %2u  err %6.1f  rest->neutral %6.1f  effort %.2f  torque %.2f  state %d  g %.2f\n", p, part.m_uiJoint, (int)part.m_Region, part.m_iSide, part.m_uiBone, fErr, fRest, part.m_fEffort,
      part.m_pConstraint->GetSwingMotorSettings().mMaxTorqueLimit, (int)part.m_pConstraint->GetSwingMotorState(), part.m_fGravityTorque);

    auto Describe = [](Quat q, const char* szLabel)
    {
      if (q.GetW() < 0.0f)
        q = -q;
      Quat s, t;
      q.GetSwingTwist(s, t);
      std::printf("      %s swing %.1f about (%.2f %.2f) twist %.1f\n", szLabel, plAngle::RadToDeg(2.0f * plMath::ACos(plMath::Clamp(s.GetW(), -1.0f, 1.0f)).GetRadian()), s.GetY(), s.GetZ(), plAngle::RadToDeg(2.0f * plMath::ATan2(t.GetX(), t.GetW()).GetRadian()));
    };

    const Quat qC1 = part.m_pConstraint->GetConstraintToBody1Matrix().GetQuaternion();
    const Quat qC2 = part.m_pConstraint->GetConstraintToBody2Matrix().GetQuaternion();
    Describe(part.m_pConstraint->GetRotationInConstraintSpace(), "now   ");
    Describe(qC1.Conjugated() * part.m_qNeutral * qC2, "neutral");
    if (part.m_fAccumTorque > 0.02f)
      Describe(qC1.Conjugated() * part.m_qAccum.Normalized() * qC2, "arbiter");
    std::printf("      W %.2f spring mode %d k %.1f c %.1f limit %.1f lambda %.2f\n", part.m_fAccumTorque, (int)part.m_pConstraint->GetSwingMotorSettings().mSpringSettings.mMode, part.m_pConstraint->GetSwingMotorSettings().mSpringSettings.mFrequency,
      part.m_pConstraint->GetSwingMotorSettings().mSpringSettings.mDamping, part.m_pConstraint->GetSwingMotorSettings().mMaxTorqueLimit, part.m_pConstraint->GetTotalLambdaMotor().Length());
    Describe(qC1.Conjugated() * part.m_qRelRest * qC2, "rest  ");
    std::printf("      limits plane %.1f normal %.1f twist %.1f..%.1f\n", plAngle::RadToDeg(part.m_pConstraint->GetPlaneHalfConeAngle()), plAngle::RadToDeg(part.m_pConstraint->GetNormalHalfConeAngle()), plAngle::RadToDeg(part.m_pConstraint->GetTwistMinAngle()), plAngle::RadToDeg(part.m_pConstraint->GetTwistMaxAngle()));
  }
}

plUInt32 plJoltRagdollController::GetNumDislocatedJoints() const
{
  plUInt32 n = 0;
  for (const Part& part : m_Parts)
    n += part.m_bDislocated ? 1 : 0;
  return n;
}

const char* plJoltRagdollController::GetActiveBehaviourName() const
{
  if (m_Grab[0].m_pGrip != nullptr || m_Grab[1].m_pGrip != nullptr)
    return "Save Fall: Holding On";
  if (m_BalanceState == plJoltRagdollBalanceState::Stepping)
    return "Balance: Stepping";
  if (m_Grab[0].m_fReach > 0.5f || m_Grab[1].m_fReach > 0.5f)
    return "Save Fall: Reaching";
  if (m_bSaveStep)
    return "Save Fall: Leg Reach";
  if (m_fSideLand > 0.5f && m_fCatchBlend > 0.5f)
    return "Protect Face: Land On Side";
  if (m_bRolling)
    return "Roll Guard";
  if (m_fBrakeBlend > 0.3f)
    return "Roll Guard: Stop Slide";
  if (m_fArmPush[0] > 0.3f || m_fArmPush[1] > 0.3f)
    return "Roll Guard: Arm Brace";
  if (m_fCatchBlend > 0.5f)
    return "Protect Face: Catch Fall";
  if (m_fBraceWeight > 0.3f)
    return "Protect Face";
  if (m_fFallWeight > 0.3f)
    return "Fall Pose";
  if (m_Inputs.m_bActiveMode && m_BalanceState == plJoltRagdollBalanceState::Standing && m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::Balance))
    return "Balance: Standing";
  if (m_bHasAnimTarget && m_fUpperAnimWeight > 0.5f)
    return "Animation";
  return "Neutral Pose";
}

//////////////////////////////////////////////////////////////////////////
// Step

void plJoltRagdollController::Step(float fDeltaTime)
{
  if (m_pRagdoll == nullptr || m_Parts.IsEmpty() || fDeltaTime <= 0.0f || m_Inputs.m_bSuspended)
    return;

  if (!m_Parts[0].m_pBody->IsActive())
    return;

  m_fTime += fDeltaTime;
  m_fTimeInFrame += fDeltaTime;
  m_fLastStepDt = fDeltaTime;
  m_fEventBudget = plMath::Min(10.0f, m_fEventBudget + 30.0f * fDeltaTime);

  UpdateSensors(fDeltaTime);

  // the bodies may only sleep once the behaviours are done with them: settled for a while and nothing active
  {
    float fMaxSpeed = 0.0f;
    for (const Part& part : m_Parts)
      fMaxSpeed = plMath::Max(fMaxSpeed, part.m_pBody->GetLinearVelocity().Length());

    m_fSlowTime = fMaxSpeed < 0.1f ? m_fSlowTime + fDeltaTime : 0.0f;

    const bool bNeutral = m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::NeutralPose);
    const bool bKeepAwake = (m_Inputs.m_bActiveMode && m_Inputs.m_fConsciousness > 0.05f) || m_fFallWeight > 0.0f || m_fBraceWeight > 0.0f || m_bRolling || m_fBrakeBlend > 0.0f || (bNeutral && m_fSlowTime < 1.5f);

    if (bKeepAwake)
    {
      for (Part& part : m_Parts)
        part.m_pBody->ResetSleepTimer();
    }
  }

  ApplyHits();
  UpdateRollGuard(fDeltaTime);
  UpdateBalance(fDeltaTime);
  UpdateFallSequence(fDeltaTime);
  UpdateGrip();
  ProposeAll(fDeltaTime);
  ApplyArbiter(fDeltaTime);
  ApplySoftLimits(fDeltaTime);
  EnforceEnvelope(fDeltaTime);
  UpdateInjury(fDeltaTime);
  RecordEvents(fDeltaTime);
  RecordTimeline();

  for (Part& part : m_Parts)
  {
    part.m_vPrevLinVel = part.m_pBody->GetLinearVelocity();
    if (part.m_iParent >= 0)
      part.m_vPrevRelAngVel = part.m_pBody->GetAngularVelocity() - m_Parts[part.m_iParent].m_pBody->GetAngularVelocity();
  }
}

void plJoltRagdollController::ReportContact(const BodyID& bodyId) const
{
  const plUInt32 uiCount = plMath::Min(m_Parts.GetCount(), 64u);
  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    if (m_Parts[p].m_pBody->GetID() == bodyId)
    {
      m_ContactBits.fetch_or(plUInt64(1) << p, std::memory_order_relaxed);
      return;
    }
  }
}

Vec3 plJoltRagdollController::GetJointPosition(const Part& part) const
{
  return part.m_pBody->GetPosition();
}

bool plJoltRagdollController::CastRay(const Vec3& vFrom, const Vec3& vDir, float fLength, float& out_fDistance) const
{
  RRayCast ray{vFrom, vDir * fLength};
  RayCastResult hit;
  IgnoreGroupFilter filter(m_uiCollisionGroup);

  if (!m_pSystem->GetNarrowPhaseQueryNoLock().CastRay(ray, hit, {}, {}, filter))
    return false;

  out_fDistance = hit.mFraction * fLength;
  return true;
}

bool plJoltRagdollController::CastRayDown(const Vec3& vFrom, float fLength, float& out_fDistance, Vec3* out_pNormal) const
{
  RRayCast ray{vFrom, -m_vUp * fLength};
  RayCastResult hit;
  IgnoreGroupFilter filter(m_uiCollisionGroup);

  if (!m_pSystem->GetNarrowPhaseQueryNoLock().CastRay(ray, hit, {}, {}, filter))
    return false;

  out_fDistance = hit.mFraction * fLength;

  if (out_pNormal)
  {
    *out_pNormal = m_vUp;
    if (const Body* pHitBody = m_pSystem->GetBodyLockInterfaceNoLock().TryGetBody(hit.mBodyID))
      *out_pNormal = pHitBody->GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
  }

  return true;
}

void plJoltRagdollController::UpdateSensors(float fDeltaTime)
{
  const Vec3 vGravity = m_pSystem->GetGravity();
  m_fGravity = vGravity.Length();
  m_vUp = m_fGravity > 0.01f ? -vGravity / m_fGravity : Vec3::sAxisZ();
  const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;

  // anti-launch: after a dislocation jolt, the two bodies may not gain more than a little speed
  for (Part& part : m_Parts)
  {
    if (part.m_uiLaunchClamp == 0)
      continue;

    --part.m_uiLaunchClamp;
    const Vec3 v = part.m_pBody->GetLinearVelocity();
    const float fSpeed = v.Length();
    if (fSpeed > part.m_fLaunchSpeedLimit)
      part.m_pBody->SetLinearVelocity(v * (part.m_fLaunchSpeedLimit / fSpeed));
  }

  m_vCom = Vec3::sZero();
  m_vComVel = Vec3::sZero();

  for (const Part& part : m_Parts)
  {
    m_vCom += part.m_pBody->GetCenterOfMassPosition() * part.m_fMass;
    m_vComVel += part.m_pBody->GetLinearVelocity() * part.m_fMass;
  }

  m_vCom /= plMath::Max(m_fTotalMass, 1e-6f);
  m_vComVel /= plMath::Max(m_fTotalMass, 1e-6f);

  // Contacts come from the contact listener (see ReportContact()), so no rays are needed to know what touches the ground.
  // Only the pelvis casts a ray, for its height above the floor and the ground normal.
  const plUInt64 uiContacts = m_ContactBits.exchange(0);
  const bool bAnyContact = uiContacts != 0;
  m_bTorsoContact = false;
  m_bFeetContact[0] = m_bFeetContact[1] = false;

  const plInt16 iFootParts[2] = {GetPart(plHumanoidBone::LeftFoot), GetPart(plHumanoidBone::RightFoot)};

  for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
  {
    Part& part = m_Parts[p];
    part.m_bContact = p < 64 && ((uiContacts >> p) & 1) != 0;

    if (!part.m_bContact)
      continue;

    if (part.m_Region == plJoltRagdollRegion::Pelvis || part.m_Region == plJoltRagdollRegion::Chest || part.m_Region == plJoltRagdollRegion::Spine)
      m_bTorsoContact = true;

    if (part.m_iSide != 0 && (part.m_Region == plJoltRagdollRegion::Foot || (part.m_Region == plJoltRagdollRegion::Shin && iFootParts[part.m_iSide > 0 ? 1 : 0] < 0)))
      m_bFeetContact[part.m_iSide > 0 ? 1 : 0] = true;
  }

  if (const plInt16 iPelvisPart = GetPart(plHumanoidBone::Hips); iPelvisPart >= 0)
  {
    float fDist = 0.0f;
    Vec3 vNormal;
    if (CastRayDown(m_Parts[iPelvisPart].m_pBody->GetCenterOfMassPosition(), 6.0f, fDist, &vNormal))
    {
      m_vGroundNormal = vNormal;
      m_fPelvisHeight = fDist;
    }
    else
    {
      m_fPelvisHeight = 1000.0f;
      m_vGroundNormal = m_vUp;
    }
  }

  m_fAirTime = bAnyContact ? 0.0f : m_fAirTime + fDeltaTime;
  m_bAirborne = m_fAirTime > 0.15f;

  // head: time to impact along its velocity, and impacts on the head for the knock-out
  m_fHeadTimeToImpact = 1000.0f;
  const plInt16 iHead = GetPart(plHumanoidBone::Head) >= 0 ? GetPart(plHumanoidBone::Head) : GetPart(plHumanoidBone::Neck);
  if (iHead >= 0)
  {
    Part& head = m_Parts[iHead];
    const Vec3 v = head.m_pBody->GetLinearVelocity();
    const float fSpeed = v.Length();
    float fDist;
    if (fSpeed > 2.0f && CastRay(head.m_pBody->GetCenterOfMassPosition(), v / fSpeed, fSpeed * 0.3f, fDist))
      m_fHeadTimeToImpact = fDist / fSpeed;

    const Vec3 vDeltaV = v - head.m_vPrevLinVel - vGravity * fDeltaTime;
    const float fImpulse = vDeltaV.Length() * head.m_fMass;

    // only the head hitting something counts: near the ground, or about to hit what is in front of it. A shove to the
    // body whips the head too, but that is no knock-out
    const bool bHeadImpact = head.m_bContact || m_fHeadTimeToImpact < 0.1f || m_fLastHeadTimeToImpact < 0.1f;
    m_fLastHeadTimeToImpact = m_fHeadTimeToImpact;

    if (bHeadImpact)
      m_fPeakHeadImpulse = plMath::Max(m_fPeakHeadImpulse, fImpulse);

    if (bHeadImpact && m_Profile.m_fKnockOutHeadImpulse > 0.0f && fImpulse > m_Profile.m_fKnockOutHeadImpulse)
    {
      const float fDrop = 0.5f + 0.5f * plMath::Saturate((fImpulse - m_Profile.m_fKnockOutHeadImpulse) / m_Profile.m_fKnockOutHeadImpulse);
      m_fConsciousnessDrop = plMath::Max(m_fConsciousnessDrop, fDrop);
    }

    // face down: the head's forward points at the ground
    const Vec3 vHeadForward = head.m_pBody->GetRotation() * (head.m_qRestGlobal.Conjugated() * m_RestFrame.m_vForward);
    m_bFaceDown = vHeadForward.Dot(-m_vUp) > 0.5f;
  }

  // roll rate about the spine
  m_fRollRate = 0.0f;
  const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
  const plInt16 iChest = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Spine);
  if (iPelvis >= 0 && iChest >= 0)
  {
    const Vec3 vSpine = (GetJointPosition(m_Parts[iChest]) - GetJointPosition(m_Parts[iPelvis])).NormalizedOr(m_vUp);
    m_fRollRate = 0.5f * (m_Parts[iPelvis].m_pBody->GetAngularVelocity() + m_Parts[iChest].m_pBody->GetAngularVelocity()).Dot(vSpine);
  }

  (void)g;
}

void plJoltRagdollController::ApplyHits()
{
  if (m_Hits.IsEmpty())
    return;

  if (m_Inputs.m_bActiveMode)
  {
    // the harder the hit, the deeper the takeover: a shove makes the torso give, a blast takes the whole body
    float fSpeedChange = 0.0f;
    for (const Hit& hit : m_Hits)
      fSpeedChange += hit.m_vImpulse.Length() / plMath::Max(m_fTotalMass, 1e-3f);

    m_fTakeoverDepth = plMath::Max(m_fTakeoverTime >= 0.0f ? m_fTakeoverDepth : 0.0f, plMath::Saturate(fSpeedChange / 2.0f));
    m_fTakeoverTime = 0.0f;
  }

  if (!m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::ImpactSpin) || m_Profile.m_fImpactSpin <= 0.0f || !m_bHumanoid)
  {
    m_Hits.Clear();
    return;
  }

  using B = plHumanoidBone;
  const B::Enum limbs[4][2] = {{B::LeftUpperArm, B::LeftLowerArm}, {B::RightUpperArm, B::RightLowerArm}, {B::LeftUpperLeg, B::LeftLowerLeg}, {B::RightUpperLeg, B::RightLowerLeg}};
  const float fRand = m_Profile.m_fImpactSpinRandomness;

  for (const Hit& hit : m_Hits)
  {
    const float fImpulse = hit.m_vImpulse.Length();
    const Vec3 vDir = hit.m_vImpulse / fImpulse;

    for (plUInt32 l = 0; l < 4; ++l)
    {
      const plInt16 iUpper = GetPart(limbs[l][0]);
      const plInt16 iLower = GetPart(limbs[l][1]);
      if (iUpper < 0 || m_Parts[iUpper].m_iParent < 0)
        continue;

      Part& upper = m_Parts[iUpper];
      Part& torso = m_Parts[upper.m_iParent];
      const bool bLeg = l >= 2;

      const Vec3 vJoint = GetJointPosition(upper);
      const Vec3 vLimb = iLower >= 0 ? GetJointPosition(m_Parts[iLower]) - vJoint : upper.m_pBody->GetCenterOfMassPosition() - vJoint;
      const float fLength = plMath::Max(vLimb.Length(), 0.05f);

      // arms whip with the hit; legs swing against it when the feet were planted, because the feet stay behind
      const bool bGrounded = bLeg && m_Profile.m_bLegsFollowGroundedFeet && m_bFeetContact[l == 2 ? 0 : 1];
      Vec3 vAxis = vLimb.Cross(bGrounded ? -vDir : vDir);
      if (vAxis.LengthSq() < 1e-6f)
        vAxis = AnyPerpendicular(vLimb);
      vAxis = vAxis.Normalized();

      // seeded randomness so no two limbs move alike
      const Vec3 vJitterAxis = (Quat::sRotation(vAxis, m_Rng.FloatMinMax(0.0f, 6.283f)) * AnyPerpendicular(vAxis)).Normalized();
      vAxis = (Quat::sRotation(vJitterAxis, Deg(25.0f) * fRand * m_Rng.FloatMinMax(-1.0f, 1.0f)) * vAxis).Normalized();
      const float fScale = plMath::Max(0.0f, 1.0f + fRand * m_Rng.FloatMinMax(-1.0f, 1.0f));

      // the limb whips at about the speed the hit gives the body, turned into a rotation of the limb, at most 15 rad/s
      const float fSpeed = fImpulse / plMath::Max(m_fTotalMass, 1e-3f);
      const float fOmega = plMath::Min(15.0f, m_Profile.m_fImpactSpin * fScale * 0.6f * fSpeed / fLength);
      const float fAngular = fOmega / InvInertiaAlong(upper.m_pBody, vAxis);

      // an internal pair: the limb against the torso, so the ragdoll's total momentum is unchanged
      upper.m_pBody->AddAngularImpulse(vAxis * fAngular);
      torso.m_pBody->AddAngularImpulse(-vAxis * fAngular);
    }
  }

  m_Hits.Clear();
}

void plJoltRagdollController::UpdateRollGuard(float fDeltaTime)
{
  m_fRollCooldown = plMath::Max(0.0f, m_fRollCooldown - fDeltaTime);

  if (!m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::RollGuard) || !m_bHumanoid)
  {
    m_bRolling = false;
    return;
  }

  const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
  const plInt16 iChest = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Spine);
  if (iPelvis < 0 || iChest < 0)
    return;

  const float fRate = plMath::Abs(m_fRollRate);

  if (!m_bRolling)
  {
    m_fRollTimer = (m_bTorsoContact && fRate > m_Profile.m_fRollRateThreshold) ? m_fRollTimer + fDeltaTime : 0.0f;

    if (m_fRollTimer > m_Profile.m_fRollDetectTime && m_fRollCooldown <= 0.0f)
    {
      m_bRolling = true;
      m_fRollCalmTimer = 0.0f;

      // the uphill side: against the downhill direction on the ground plane
      const Vec3 vGravity = -m_vUp;
      Vec3 vDownhill = vGravity - m_vGroundNormal * vGravity.Dot(m_vGroundNormal);
      const Part& pelvis = m_Parts[iPelvis];
      const Vec3 vRight = pelvis.m_pBody->GetRotation() * (pelvis.m_qRestGlobal.Conjugated() * m_RestFrame.m_vRight);

      if (vDownhill.LengthSq() > 0.0025f)
        m_iUphillSide = vRight.Dot(-vDownhill) > 0.0f ? 1 : -1;
      else
        m_iUphillSide = m_fRollRate > 0.0f ? 1 : -1;
    }
  }

  if (!m_bRolling)
    return;

  // damping about the spine axis on the pelvis and the chest. It works at any consciousness, it stands for the body's own weight
  const Vec3 vSpine = (GetJointPosition(m_Parts[iChest]) - GetJointPosition(m_Parts[iPelvis])).NormalizedOr(m_vUp);
  for (plInt16 i : {iPelvis, iChest})
  {
    Part& part = m_Parts[i];
    const float fSpin = part.m_pBody->GetAngularVelocity().Dot(vSpine);
    const float fFactor = plMath::Min(1.0f, 6.0f * m_Profile.m_fRollSpineDamping * fDeltaTime);
    part.m_pBody->AddAngularImpulse(-vSpine * (fSpin * fFactor / InvInertiaAlong(part.m_pBody, vSpine)));
  }

  m_fRollCalmTimer = fRate < 0.3f ? m_fRollCalmTimer + fDeltaTime : 0.0f;

  if (m_fRollCalmTimer > 0.3f || !m_bTorsoContact && m_fRollCalmTimer > 0.1f)
  {
    m_bRolling = false;
    m_fRollTimer = 0.0f;
    m_fRollCooldown = 1.0f;
  }
}

bool plJoltRagdollController::IsSwingLeg(const Part& part) const
{
  const bool bLeg = part.m_Region == plJoltRagdollRegion::Thigh || part.m_Region == plJoltRagdollRegion::Shin || part.m_Region == plJoltRagdollRegion::Foot;
  if (!bLeg)
    return false;

  return (m_BalanceState == plJoltRagdollBalanceState::Stepping && part.m_iSide == m_iSwingSide) || (m_bSaveStep && part.m_iSide == m_iSaveSide);
}

void plJoltRagdollController::UpdateGrip()
{
  // more grip for the limbs that touch the ground while rolling or pushing against a roll; the slide brake only spreads the
  // limbs, so a fall down a flight of stairs slows rather than stops dead
  const bool bGrip = m_bRolling || m_fArmPush[0] > 0.3f || m_fArmPush[1] > 0.3f;
  if (bGrip == m_bGripBoosted)
    return;

  m_bGripBoosted = bGrip;
  for (Part& part : m_Parts)
  {
    if (part.m_Region == plJoltRagdollRegion::Hand || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Shin || part.m_Region == plJoltRagdollRegion::Thigh)
      part.m_pBody->SetFriction(bGrip ? part.m_fFriction * m_Profile.m_fRollFrictionBoost : part.m_fFriction);
  }
}

void plJoltRagdollController::ProposeTwoBoneIk(plUInt8 uiThighBone, const Vec3& vFootTarget, float fWeight)
{
  const plHumanoidBone::Enum thighBone = (plHumanoidBone::Enum)uiThighBone;
  const plHumanoidBone::Enum shinBone = ChildBone(thighBone);
  const plHumanoidBone::Enum footBone = ChildBone(shinBone);

  const plInt16 iThigh = GetPart(thighBone);
  const plInt16 iShin = GetPart(shinBone);
  const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
  if (iThigh < 0 || iShin < 0 || iPelvis < 0 || m_Parts[iThigh].m_iParent < 0)
    return;

  Part& thigh = m_Parts[iThigh];
  Part& shin = m_Parts[iShin];
  const Part& parent = m_Parts[thigh.m_iParent];

  const float L1 = (m_vRestJointPos[shinBone] - m_vRestJointPos[thighBone]).GetLength();
  const float L2 = m_bRestJointValid[footBone] ? (m_vRestJointPos[footBone] - m_vRestJointPos[shinBone]).GetLength() : L1;

  const Vec3 vHip = GetJointPosition(thigh);
  const Vec3 vKneeNow = GetJointPosition(shin);
  const plInt16 iFoot = GetPart(footBone);
  const Vec3 vAnkleNow = iFoot >= 0 ? GetJointPosition(m_Parts[iFoot]) : vKneeNow + (vKneeNow - vHip).NormalizedOr(-m_vUp) * L2;

  // the ankle target sits as high above the foot target as the ankle is above the ground in the rest pose
  const Vec3 vAnkle = vFootTarget + m_vUp * 0.08f;
  const Vec3 vToTarget = vAnkle - vHip;
  const float fDist = plMath::Clamp(vToTarget.Length(), 0.05f, (L1 + L2) * 0.98f);
  const Vec3 vDir = vToTarget.NormalizedOr(-m_vUp);

  // knees bend towards the front of the pelvis
  const Part& pelvis = m_Parts[iPelvis];
  const Vec3 vForward = pelvis.m_pBody->GetRotation() * (pelvis.m_qRestGlobal.Conjugated() * m_RestFrame.m_vForward);
  const Vec3 vPole = (vForward - vDir * vForward.Dot(vDir)).NormalizedOr(AnyPerpendicular(vDir));

  const float fCos = plMath::Clamp((L1 * L1 + fDist * fDist - L2 * L2) / (2.0f * L1 * fDist), -1.0f, 1.0f);
  const float fSin = plMath::Sqrt(plMath::Max(0.0f, 1.0f - fCos * fCos));
  const Vec3 vKnee = vHip + vDir * (L1 * fCos) + vPole * (L1 * fSin);
  const Vec3 vAnkleIk = vHip + vDir * fDist;

  const Quat qThigh = (SafeFromTo(vKneeNow - vHip, vKnee - vHip) * thigh.m_pBody->GetRotation()).Normalized();
  const Quat qShin = (SafeFromTo(vAnkleNow - vKneeNow, vAnkleIk - vKnee) * shin.m_pBody->GetRotation()).Normalized();

  thigh.m_qIk = ClampToLimits(thigh, (parent.m_pBody->GetRotation().Conjugated() * qThigh).Normalized());
  shin.m_qIk = ClampToLimits(shin, (qThigh.Conjugated() * qShin).Normalized());
  thigh.m_bHasIk = shin.m_bHasIk = fWeight > 0.0f;
}

void plJoltRagdollController::AssistSwingFoot(plInt8 iSide, const Vec3& vFootTarget, float fDeltaTime, float fStrength)
{
  // The joint muscles alone lift a planted foot too late: the leg still carries half the body when the step starts, and a
  // pose target says nothing about where the foot has to be. A capped force pair between the swing foot and the pelvis
  // carries the foot along the step's path. It is internal, so the body as a whole gains no momentum from it: it moves the
  // foot, it does not hold the body up.
  using B = plHumanoidBone;
  const bool bLeft = iSide < 0;
  const plInt16 iFoot = GetPart(bLeft ? B::LeftFoot : B::RightFoot) >= 0 ? GetPart(bLeft ? B::LeftFoot : B::RightFoot) : GetPart(bLeft ? B::LeftLowerLeg : B::RightLowerLeg);
  const plInt16 iThigh = GetPart(bLeft ? B::LeftUpperLeg : B::RightUpperLeg);
  const plInt16 iPelvis = GetPart(B::Hips);
  if (iFoot < 0 || iThigh < 0 || iPelvis < 0)
    return;

  Body* pFoot = m_Parts[iFoot].m_pBody;
  Body* pPelvis = m_Parts[iPelvis].m_pBody;
  const float fLegMass = m_Parts[iThigh].m_fChainMass;
  const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;

  // the ankle target, as in the leg IK; a critically damped spring at 5 Hz on the leg's mass
  const Vec3 vErr = (vFootTarget + m_vUp * 0.08f) - pFoot->GetPosition();
  const Vec3 vRelVel = pFoot->GetLinearVelocity() - pPelvis->GetLinearVelocity();
  const float w = 2.0f * plMath::Pi<float>() * 5.0f;
  Vec3 vForce = (vErr * (w * w) - vRelVel * (2.0f * w)) * fLegMass;

  const float fMax = 2.0f * fStrength * fLegMass * g * plMath::Saturate(m_Inputs.m_fConsciousness);
  const float fLen = vForce.Length();
  if (fLen > fMax)
    vForce *= fMax / fLen;

  // the reaction goes into the pelvis at the hip of the swinging leg, as the hip muscles would push: at the pelvis' centre
  // the pair would have a lever sideways and turn the whole body about its vertical axis
  pFoot->AddImpulse(vForce * fDeltaTime);
  pPelvis->AddImpulse(-vForce * fDeltaTime, RVec3(m_Parts[iThigh].m_pBody->GetPosition()));
}

void plJoltRagdollController::UpdateBalance(float fDeltaTime)
{
  for (Part& part : m_Parts)
    part.m_bHasIk = false;

  m_fBalanceAssist = 0.0f;

  const bool bBalance = m_Inputs.m_bActiveMode && m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::Balance) && m_bHumanoid && m_Inputs.m_fConsciousness > 0.05f;
  const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
  const plInt16 iFeet[2] = {GetPart(plHumanoidBone::LeftFoot) >= 0 ? GetPart(plHumanoidBone::LeftFoot) : GetPart(plHumanoidBone::LeftLowerLeg),
    GetPart(plHumanoidBone::RightFoot) >= 0 ? GetPart(plHumanoidBone::RightFoot) : GetPart(plHumanoidBone::RightLowerLeg)};

  // takeover: after a hit the upper body is physical first and the legs keep their animation while balance holds
  if (m_Inputs.m_bActiveMode && m_fTakeoverTime >= 0.0f)
  {
    m_fTakeoverTime += fDeltaTime;
    const float fUpper = m_Profile.m_fTakeoverUpperTime;

    if (m_BalanceState == plJoltRagdollBalanceState::Falling || m_BalanceState == plJoltRagdollBalanceState::Down || !bBalance)
    {
      // a body going over keeps working its legs while balance hands over to the fall, then lets them go
      const bool bGoneOver = m_BalanceState == plJoltRagdollBalanceState::Falling || m_BalanceState == plJoltRagdollBalanceState::Down;
      const float fLegsTime = bGoneOver ? plMath::Min(m_Profile.m_fTakeoverLegsTime, s_fGiveUpTime) : m_Profile.m_fTakeoverLegsTime;
      m_fUpperAnimWeight = plMath::Max(0.0f, m_fUpperAnimWeight - fDeltaTime / plMath::Max(fUpper, 0.01f));
      m_fLegsAnimWeight = plMath::Max(0.0f, m_fLegsAnimWeight - fDeltaTime / plMath::Max(fLegsTime, 0.01f));
    }
    else
    {
      // stagger: the torso gives at once, as far as the hit was hard, and regains its posture once the feet hold again
      const float fGive = m_fTakeoverTime < fUpper ? m_fTakeoverTime / plMath::Max(fUpper, 0.01f) : 1.0f - plMath::Saturate((m_fTakeoverTime - fUpper - 0.3f) / 1.0f);
      m_fUpperAnimWeight = 1.0f - fGive * m_fTakeoverDepth;
      m_fLegsAnimWeight = 1.0f;
    }
  }
  else if (!m_Inputs.m_bActiveMode)
  {
    m_fUpperAnimWeight = m_fLegsAnimWeight = 1.0f;
  }

  if (!bBalance || iPelvis < 0 || iFeet[0] < 0 || iFeet[1] < 0)
    return;

  if (m_BalanceState == plJoltRagdollBalanceState::Falling)
  {
    // down once the torso lies on something, or once the body has come to rest low down on whatever else it landed on: a
    // brace propped up on elbows and knees never lets the torso touch
    bool bRestingLow = m_vComVel.Length() < 0.5f && m_fPelvisHeight < 0.5f * m_fLegLength;
    if (bRestingLow)
    {
      bool bNonFootContact = false;
      for (const Part& part : m_Parts)
        bNonFootContact = bNonFootContact || (part.m_bContact && part.m_Region != plJoltRagdollRegion::Foot);
      bRestingLow = bNonFootContact;
    }

    if (m_bTorsoContact || bRestingLow)
      m_BalanceState = plJoltRagdollBalanceState::Down;
    return;
  }

  if (m_BalanceState == plJoltRagdollBalanceState::Down)
    return;

  const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;
  auto Horizontal = [&](Vec3Arg v) { return v - m_vUp * v.Dot(m_vUp); };

  const Vec3 vFoot[2] = {m_Parts[iFeet[0]].m_pBody->GetCenterOfMassPosition(), m_Parts[iFeet[1]].m_pBody->GetCenterOfMassPosition()};
  const Vec3 vFeetMid = (vFoot[0] + vFoot[1]) * 0.5f;
  const float fHeight = plMath::Max(0.2f, (m_vCom - vFeetMid).Dot(m_vUp));
  const float fOmega0 = plMath::Sqrt(g / fHeight);

  // capture point on the ground plane, relative to the feet
  const Vec3 vComH = Horizontal(m_vCom - vFeetMid);
  const Vec3 vVelH = Horizontal(m_vComVel);
  const Vec3 vXi = vComH + vVelH / fOmega0;
  m_vCapturePoint = vFeetMid + vXi;

  // distance from the capture point to the support: the segment between the feet, 10 cm wide
  const Vec3 vA = Horizontal(vFoot[0] - vFeetMid);
  const Vec3 vB = Horizontal(vFoot[1] - vFeetMid);
  const Vec3 vAB = vB - vA;
  const float t = plMath::Saturate(vAB.LengthSq() > 1e-6f ? (vXi - vA).Dot(vAB) / vAB.LengthSq() : 0.5f);
  const float fOutside = plMath::Max(0.0f, (vXi - (vA + vAB * t)).Length() - 0.1f);

  // rung 1: a visible, capped upright assist on pelvis and chest. "Upright" leans away from where the capture point sits
  // relative to the feet (at most 10°), so the stiff body tips back over its support: the ankle strategy
  {
    const Part& pelvis = m_Parts[iPelvis];
    const Vec3 vPelvisUp = pelvis.m_pBody->GetRotation() * (pelvis.m_qRestGlobal.Conjugated() * m_RestFrame.m_vUp);
    Vec3 vLean = -vXi * (1.5f / fHeight);
    if (vLean.Length() > 0.18f)
      vLean = vLean.Normalized() * 0.18f;
    const Vec3 vTargetUp = (m_vUp + vLean).Normalized();
    const Vec3 vTiltAxis = vPelvisUp.Cross(vTargetUp);
    const Vec3 vAngVel = Horizontal(pelvis.m_pBody->GetAngularVelocity());
    const float fMgh = m_fTotalMass * g * fHeight;

    Vec3 vTorque = vTiltAxis * (fMgh * 3.0f) - vAngVel * (m_fTotalMass * fHeight * fHeight * 2.0f);
    const float fMax = m_Profile.m_fMaxBalanceAssist * fMgh * m_Inputs.m_fConsciousness;
    const float fLen = vTorque.Length();
    if (fLen > fMax && fLen > 0.0f)
      vTorque *= fMax / fLen;

    m_fBalanceAssist = fLen > 0.0f ? plMath::Min(fLen, fMax) / plMath::Max(fMgh, 1e-3f) : 0.0f;

    if (m_bFeetContact[0] || m_bFeetContact[1])
    {
      const plInt16 iChest = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Spine);
      m_Parts[iPelvis].m_pBody->AddAngularImpulse(vTorque * (0.6f * fDeltaTime));
      if (iChest >= 0)
        m_Parts[iChest].m_pBody->AddAngularImpulse(vTorque * (0.4f * fDeltaTime));
    }
  }

  // leg effort saturating for long means the legs cannot carry the body
  float fLegEffort = 0.0f;
  plUInt32 uiLegJoints = 0;
  for (const Part& part : m_Parts)
  {
    if ((part.m_Region == plJoltRagdollRegion::Thigh || part.m_Region == plJoltRagdollRegion::Shin) && part.m_pConstraint)
    {
      fLegEffort += part.m_fEffort;
      ++uiLegJoints;
    }
  }
  fLegEffort /= plMath::Max(1u, uiLegJoints);
  m_fLegEffortTimer = fLegEffort > 0.95f ? m_fLegEffortTimer + fDeltaTime : 0.0f;

  // the direction the body is being pushed: the stumble and, once balance gives up, the fall
  if (vXi.LengthSq() > 0.0025f)
    m_vFallDir = vXi.Normalized();

  auto Fail = [&](const char* szReason)
  {
    m_szFallReason = szReason;
    m_bFallFromFeet = true;
    m_fGiveUp = 0.0f;
    m_bFirstImpact = false;
    m_BalanceState = plJoltRagdollBalanceState::Falling;
    if (m_fTakeoverTime < 0.0f)
      m_fTakeoverTime = 0.0f;

    auto& msg = m_BalanceLost.ExpandAndGetRef();
    msg.m_vDirection = plJoltConversionUtils::ToVec3(vXi.NormalizedOr(Vec3::sZero()));
    msg.m_fSpeed = vVelH.Length();
  };

  const float fReach = m_fLegLength * 1.3f;

  // the torso on the ground, or a pelvis tipped past 35°, ends any balancing; protection takes over from here. A step under
  // way is the recovery itself, so it gets until 45°
  {
    const Part& pelvis = m_Parts[iPelvis];
    const Vec3 vPelvisUp = pelvis.m_pBody->GetRotation() * (pelvis.m_qRestGlobal.Conjugated() * m_RestFrame.m_vUp);
    const float fMinUp = m_BalanceState == plJoltRagdollBalanceState::Stepping ? 0.7f : 0.82f;
    if (m_bTorsoContact || vPelvisUp.Dot(m_vUp) < fMinUp)
    {
      Fail(m_bTorsoContact ? "torso down" : "pelvis tipped");
      return;
    }
  }

  if (m_BalanceState == plJoltRagdollBalanceState::Standing)
  {
    if (!m_bFeetContact[0] && !m_bFeetContact[1])
    {
      if (m_bAirborne)
        Fail("airborne");
      return;
    }

    // standing steadily for a second gives back the recovery steps
    m_fSteadyTime = fOutside <= 0.0f && vVelH.Length() < 0.2f ? m_fSteadyTime + fDeltaTime : 0.0f;
    if (m_fSteadyTime > 1.0f)
      m_uiStepsTaken = 0;

    if (vXi.Length() > fReach || m_fLegEffortTimer > 0.3f)
    {
      Fail(vXi.Length() > fReach ? "capture point out of reach" : "legs gave way");
      return;
    }

    if (fOutside > 0.0f)
    {
      m_fStepDelay += fDeltaTime;

      // the first step needs a moment to react; a stumble follows one step with the next at once
      if (m_fStepDelay >= (m_uiStepsTaken > 0 ? 0.04f : 0.12f))
      {
        if (m_uiStepsTaken >= m_Profile.m_uiMaxRecoverySteps)
        {
          if (fOutside > 0.15f)
            Fail("out of recovery steps");
          return;
        }

        // step with the foot that is further from the capture point, to just past it
        const int iSwing = (Horizontal(vFoot[0] - m_vCapturePoint).LengthSq() > Horizontal(vFoot[1] - m_vCapturePoint).LengthSq()) ? 0 : 1;
        m_iSwingSide = iSwing == 0 ? -1 : 1;
        m_vStepStart = vFoot[iSwing];

        // just past the capture point, but on the foot's own side at the stance width: stepping onto the capture point itself
        // would cross the foot under the body and turn the hips
        const Vec3 vPushDir = vXi.NormalizedOr(Vec3::sZero());
        const Vec3 vFootSide = Horizontal(vFoot[iSwing] - vFeetMid);
        Vec3 vTarget = m_vCapturePoint + vPushDir * 0.1f + (vFootSide - vPushDir * vFootSide.Dot(vPushDir));
        vTarget = vTarget - m_vUp * (vTarget - vFoot[1 - iSwing]).Dot(m_vUp);

        const Vec3 vHip = GetJointPosition(m_Parts[GetPart(iSwing == 0 ? plHumanoidBone::LeftUpperLeg : plHumanoidBone::RightUpperLeg)]);
        Vec3 vFromHip = Horizontal(vTarget - vHip);
        const float fMaxStep = m_fLegLength * 0.9f;
        if (vFromHip.Length() > fMaxStep)
          vTarget = vTarget - vFromHip + vFromHip.Normalized() * fMaxStep;

        m_vStepTarget = vTarget;
        m_fStepDuration = plMath::Max(m_Profile.m_fMinSwingTime, (vTarget - m_vStepStart).Length() / plMath::Max(m_Profile.m_fMaxFootSpeed, 0.1f));
        m_fStepTimer = 0.0f;
        m_fStepDelay = 0.0f;
        m_BalanceState = plJoltRagdollBalanceState::Stepping;
      }
    }
    else
    {
      m_fStepDelay = 0.0f;
    }
  }

  if (m_BalanceState == plJoltRagdollBalanceState::Stepping)
  {
    m_fStepTimer += fDeltaTime;
    const float s = plMath::Saturate(m_fStepTimer / m_fStepDuration);

    // the foot comes up first and travels after: the lift peaks a third of the way through, the travel eases in and out
    const float fLift = 0.14f * plMath::Sin(plAngle::MakeFromRadian(plMath::Pi<float>() * plMath::Pow(s, 0.7f)));
    const float fTravel = s * s * (3.0f - 2.0f * s);
    const Vec3 vFootPos = m_vStepStart + (m_vStepTarget - m_vStepStart) * fTravel + m_vUp * fLift;

    ProposeTwoBoneIk((plUInt8)(m_iSwingSide < 0 ? plHumanoidBone::LeftUpperLeg : plHumanoidBone::RightUpperLeg), vFootPos, m_Inputs.m_fConsciousness);
    AssistSwingFoot(m_iSwingSide, vFootPos, fDeltaTime);

    const bool bLanded = m_bFeetContact[m_iSwingSide < 0 ? 0 : 1];
    if ((s >= 1.0f && bLanded) || m_fStepTimer > m_fStepDuration + 0.25f)
    {
      ++m_uiStepsTaken;
      m_BalanceState = plJoltRagdollBalanceState::Standing;
    }

    if (vXi.Length() > fReach * 1.2f || m_fLegEffortTimer > 0.3f)
      Fail(vXi.Length() > fReach * 1.2f ? "capture point out of reach mid-step" : "legs gave way mid-step");
  }
}

void plJoltRagdollController::UpdateFallSequence(float fDeltaTime)
{
  const float c = plMath::Saturate(m_Inputs.m_fConsciousness);
  const auto& beh = m_Inputs.m_Behaviours;
  auto Horizontal = [&](Vec3Arg v) { return v - m_vUp * v.Dot(m_vUp); };

  if (m_BalanceState == plJoltRagdollBalanceState::Standing || m_BalanceState == plJoltRagdollBalanceState::Stepping)
  {
    m_bFallFromFeet = false;
    m_bFirstImpact = false;
  }

  // a fall from the feet runs catch -> impact -> protect. The first impact is anything but the feet and shins touching down;
  // until then the fall direction follows the body's motion
  if (m_bFallFromFeet && !m_bFirstImpact)
  {
    for (const Part& part : m_Parts)
    {
      if (part.m_bContact && part.m_Region != plJoltRagdollRegion::Foot && part.m_Region != plJoltRagdollRegion::Shin)
        m_bFirstImpact = true;
    }

    const Vec3 vVelH = Horizontal(m_vComVel);
    if (vVelH.LengthSq() > 0.09f)
      m_vFallDir = vVelH.Normalized();
  }

  // the fall (or stumble) direction in the torso's frame: +forward / +right
  {
    Vec3 vForward = m_RestFrame.m_vForward, vRight = m_RestFrame.m_vRight;
    const plInt16 iTorso = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Hips);
    if (iTorso >= 0)
    {
      const Part& torso = m_Parts[iTorso];
      const Quat q = torso.m_pBody->GetRotation() * torso.m_qRestGlobal.Conjugated();
      vForward = q * m_RestFrame.m_vForward;
      vRight = q * m_RestFrame.m_vRight;
    }

    const Vec3 vDir = Horizontal(m_vFallDir).NormalizedOr(Vec3::sZero());
    m_fFallFwd = vDir.Dot(Horizontal(vForward).NormalizedOr(Vec3::sZero()));
    m_fFallSide = vDir.Dot(Horizontal(vRight).NormalizedOr(Vec3::sZero()));
  }

  // catch: the arms reach for the ground on the way down. A fall that lasts longer than a stumble is a fall from a height,
  // which keeps the guard instead
  const bool bProtect = beh.IsSet(plJoltRagdollBehaviour::ProtectFace) && m_Profile.m_fBraceStrength > 0.0f && m_bHumanoid;
  // the hand-over: balance lets go over s_fGiveUpTime and the catch comes in as it does
  m_fGiveUp = plMath::Min(1.0f, m_fGiveUp + fDeltaTime / s_fGiveUpTime);
  const bool bCatching = bProtect && m_bFallFromFeet && !m_bFirstImpact && m_fFallTime < 1.2f;
  const float fCatchTarget = bCatching ? c * plMath::Min(1.0f, 0.3f + m_fGiveUp) : 0.0f;
  m_fCatchBlend = fCatchTarget > m_fCatchBlend ? plMath::Min(fCatchTarget, m_fCatchBlend + fDeltaTime / 0.12f) : plMath::Max(fCatchTarget, m_fCatchBlend - fDeltaTime / 0.35f);

  // brake: lying on the ground and sliding without rolling, the arms and legs spread out to dig in
  float fBrakeTarget = 0.0f;
  if (beh.IsSet(plJoltRagdollBehaviour::RollGuard) && m_bHumanoid && c > 0.05f && m_bTorsoContact && !m_bRolling && m_fCatchBlend < 0.5f)
  {
    const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
    const float fSpin = iPelvis >= 0 ? m_Parts[iPelvis].m_pBody->GetAngularVelocity().Length() : 0.0f;
    if (fSpin < 3.0f)
      fBrakeTarget = c * plMath::Saturate((Horizontal(m_vComVel).Length() - 0.5f) / 1.0f);
  }
  m_fBrakeBlend = fBrakeTarget > m_fBrakeBlend ? plMath::Min(fBrakeTarget, m_fBrakeBlend + 4.0f * fDeltaTime) : plMath::Max(fBrakeTarget, m_fBrakeBlend - 1.5f * fDeltaTime);

  // stagger: the arms go out for balance while the feet step out of a push
  const bool bStagger = m_Inputs.m_bActiveMode && beh.IsSet(plJoltRagdollBehaviour::Balance) && m_bHumanoid &&
                        (m_BalanceState == plJoltRagdollBalanceState::Stepping || (m_BalanceState == plJoltRagdollBalanceState::Standing && m_fStepDelay > 0.0f));
  // once balance has given up the flailing arms come round into the catch over the hand-over
  const bool bGoneOver = m_BalanceState == plJoltRagdollBalanceState::Falling || m_BalanceState == plJoltRagdollBalanceState::Down;
  m_fStaggerBlend = bStagger ? plMath::Min(c, m_fStaggerBlend + fDeltaTime / 0.15f) : plMath::Max(0.0f, m_fStaggerBlend - fDeltaTime / (bGoneOver ? 0.3f : 0.5f));

  // Rolling or tumbling over the ground, an arm whose hand or forearm touches down pushes against the roll. The push is a
  // force pair between the hand, which the ground holds by friction, and the shoulder: the body only slows down as far as
  // the hand's grip allows.
  {
    const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
    const Vec3 vVelH = Horizontal(m_vComVel);
    const float fSpeed = vVelH.Length();
    const bool bOnGround = m_bTorsoContact || m_BalanceState == plJoltRagdollBalanceState::Down || m_bRolling;
    const bool bTumbling = iPelvis >= 0 && m_Parts[iPelvis].m_pBody->GetAngularVelocity().Length() > 2.0f;
    const bool bAllowed = beh.IsSet(plJoltRagdollBehaviour::RollGuard) && m_bHumanoid && c > 0.05f && bOnGround && bTumbling && fSpeed > 0.4f;
    const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;

    using B = plHumanoidBone;
    const B::Enum hands[2] = {B::LeftHand, B::RightHand};
    const B::Enum forearms[2] = {B::LeftLowerArm, B::RightLowerArm};
    const B::Enum upperArms[2] = {B::LeftUpperArm, B::RightUpperArm};

    for (int s = 0; s < 2; ++s)
    {
      const plInt16 iHand = GetPart(hands[s]);
      const plInt16 iForearm = GetPart(forearms[s]);
      const plInt16 iUpper = GetPart(upperArms[s]);
      const bool bHandDown = iHand >= 0 && m_Parts[iHand].m_bContact;
      const bool bForearmDown = iForearm >= 0 && m_Parts[iForearm].m_bContact;

      const float fTarget = (bAllowed && (bHandDown || bForearmDown)) ? c * plMath::Saturate((fSpeed - 0.3f) / 1.2f) : 0.0f;
      m_fArmPush[s] = fTarget > m_fArmPush[s] ? plMath::Min(fTarget, m_fArmPush[s] + 6.0f * fDeltaTime) : plMath::Max(fTarget, m_fArmPush[s] - 3.0f * fDeltaTime);

      if (fTarget <= 0.0f || iUpper < 0 || m_Parts[iUpper].m_iParent < 0)
        continue;

      // a little: at most 15% of the body's weight per arm
      const Vec3 vImpulse = vVelH.Normalized() * (m_fArmPush[s] * 0.15f * m_fTotalMass * g * fDeltaTime);
      m_Parts[bHandDown ? iHand : iForearm].m_pBody->AddImpulse(vImpulse);
      m_Parts[m_Parts[iUpper].m_iParent].m_pBody->AddImpulse(-vImpulse, RVec3(m_Parts[iUpper].m_pBody->GetPosition()));
    }
  }

  // trying to save the fall while bracing for it: look for a hold, turn to land on a side, reach a leg for the ground
  UpdateGrab(fDeltaTime);
  UpdateSideLanding(fDeltaTime);
  UpdateSaveStep(fDeltaTime);
}

Quat plJoltRagdollController::GetCatchPose(const Part& part) const
{
  // front and back by how far the fall goes forwards or backwards; the arm on the falling side reaches out sideways
  const float fBack = plMath::Saturate(0.5f - 0.75f * m_fFallFwd);
  Quat q = Blend(part.m_qCatchFront, part.m_qCatchBack, fBack);

  const bool bArm = part.m_Region == plJoltRagdollRegion::UpperArm || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Hand;
  if (bArm && part.m_iSide * m_fFallSide > 0.0f)
    q = Blend(q, part.m_qCatchSide, plMath::Saturate(plMath::Abs(m_fFallSide) * 1.5f - 0.2f));

  // turning to land on a side: the near side reaches down and stays long, the far side guards the head and tucks
  if (m_fSideLand > 0.0f && m_iLandSide != 0)
  {
    const Quat& qLand = (part.m_iSide == 0 || part.m_iSide == m_iLandSide) ? part.m_qLandNear : part.m_qLandFar;
    q = Blend(q, qLand, m_fSideLand);
  }

  return q;
}

bool plJoltRagdollController::CastRayHit(const Vec3& vFrom, const Vec3& vDir, float fLength, Vec3& out_vPoint, Vec3& out_vNormal, bool& out_bStatic) const
{
  RRayCast ray{vFrom, vDir * fLength};
  RayCastResult hit;
  IgnoreGroupFilter filter(m_uiCollisionGroup);

  if (!m_pSystem->GetNarrowPhaseQueryNoLock().CastRay(ray, hit, {}, {}, filter))
    return false;

  out_vPoint = Vec3(ray.GetPointOnRay(hit.mFraction));
  out_vNormal = -vDir;
  out_bStatic = false;

  if (const Body* pHitBody = m_pSystem->GetBodyLockInterfaceNoLock().TryGetBody(hit.mBodyID))
  {
    out_vNormal = pHitBody->GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
    out_bStatic = pHitBody->IsStatic();
  }

  return true;
}

void plJoltRagdollController::ProposeArmIk(int iSide, const Vec3& vHandTarget, float fWeight)
{
  using B = plHumanoidBone;
  const plInt16 iUpper = GetPart(iSide == 0 ? B::LeftUpperArm : B::RightUpperArm);
  const plInt16 iLower = GetPart(iSide == 0 ? B::LeftLowerArm : B::RightLowerArm);
  const plInt16 iHand = GetPart(iSide == 0 ? B::LeftHand : B::RightHand);
  if (iUpper < 0 || iLower < 0 || iHand < 0 || m_Parts[iUpper].m_iParent < 0 || fWeight <= 0.0f)
    return;

  Part& upper = m_Parts[iUpper];
  Part& lower = m_Parts[iLower];
  const Part& parent = m_Parts[upper.m_iParent];

  const Vec3 vShoulder = GetJointPosition(upper);
  const Vec3 vElbowNow = GetJointPosition(lower);
  const Vec3 vWristNow = GetJointPosition(m_Parts[iHand]);
  const float L1 = (vElbowNow - vShoulder).Length();
  const float L2 = (vWristNow - vElbowNow).Length();

  const Vec3 vToTarget = vHandTarget - vShoulder;
  const float fDist = plMath::Clamp(vToTarget.Length(), 0.05f, (L1 + L2) * 0.98f);
  const Vec3 vDir = vToTarget.NormalizedOr(-m_vUp);

  // elbows bend down and back, away from where the hand reaches
  const Vec3 vForward = parent.m_pBody->GetRotation() * (parent.m_qRestGlobal.Conjugated() * m_RestFrame.m_vForward);
  const Vec3 vPoleRaw = -m_vUp - vForward * 0.5f;
  const Vec3 vPole = (vPoleRaw - vDir * vPoleRaw.Dot(vDir)).NormalizedOr(AnyPerpendicular(vDir));

  const float fCos = plMath::Clamp((L1 * L1 + fDist * fDist - L2 * L2) / (2.0f * L1 * fDist), -1.0f, 1.0f);
  const float fSin = plMath::Sqrt(plMath::Max(0.0f, 1.0f - fCos * fCos));
  const Vec3 vElbow = vShoulder + vDir * (L1 * fCos) + vPole * (L1 * fSin);
  const Vec3 vWrist = vShoulder + vDir * fDist;

  const Quat qUpper = (SafeFromTo(vElbowNow - vShoulder, vElbow - vShoulder) * upper.m_pBody->GetRotation()).Normalized();
  const Quat qLower = (SafeFromTo(vWristNow - vElbowNow, vWrist - vElbow) * lower.m_pBody->GetRotation()).Normalized();

  upper.m_qIk = ClampToLimits(upper, (parent.m_pBody->GetRotation().Conjugated() * qUpper).Normalized());
  lower.m_qIk = ClampToLimits(lower, (qUpper.Conjugated() * qLower).Normalized());
  upper.m_bHasIk = lower.m_bHasIk = true;
}

void plJoltRagdollController::UpdateGrab(float fDeltaTime)
{
  // A stumbling or falling body looks for something to hold on to: anything solid and not floor-like within arm's reach,
  // towards where it is going or to the side. The hand on that side reaches for it; the grip itself is taken and broken on
  // the main thread (UpdateGrips()).
  const float c = plMath::Saturate(m_Inputs.m_fConsciousness);
  const bool bLooking = m_Inputs.m_bActiveMode && m_bHumanoid && c > 0.3f && m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::Balance) &&
                        (m_fStaggerBlend > 0.05f || (m_bFallFromFeet && !m_bFirstImpact));

  using B = plHumanoidBone;
  const plInt16 iChest = GetPart(B::Chest) >= 0 ? GetPart(B::Chest) : GetPart(B::Hips);
  if (iChest < 0)
    return;

  const Part& chest = m_Parts[iChest];
  const Quat qBody = chest.m_pBody->GetRotation() * chest.m_qRestGlobal.Conjugated();
  const Vec3 vRight = qBody * m_RestFrame.m_vRight;
  const Vec3 vFallH = (m_vFallDir - m_vUp * m_vFallDir.Dot(m_vUp)).NormalizedOr(-(qBody * m_RestFrame.m_vForward));

  ++m_uiProbeCounter;

  for (int s = 0; s < 2; ++s)
  {
    Grab& grab = m_Grab[s];
    grab.m_fCooldown = plMath::Max(0.0f, grab.m_fCooldown - fDeltaTime);

    const plInt16 iUpper = GetPart(s == 0 ? B::LeftUpperArm : B::RightUpperArm);
    const plInt16 iLower = GetPart(s == 0 ? B::LeftLowerArm : B::RightLowerArm);
    const plInt16 iHand = GetPart(s == 0 ? B::LeftHand : B::RightHand);
    if (iUpper < 0 || iLower < 0 || iHand < 0)
      continue;

    const Vec3 vShoulder = GetJointPosition(m_Parts[iUpper]);
    const float fReach = (GetJointPosition(m_Parts[iLower]) - vShoulder).Length() + (GetJointPosition(m_Parts[iHand]) - GetJointPosition(m_Parts[iLower])).Length() + 0.08f;

    if (grab.m_pGrip == nullptr)
    {
      if (!bLooking || grab.m_fCooldown > 0.0f)
        grab.m_bHasTarget = false;
      else if (grab.m_bHasTarget && (grab.m_vPoint - vShoulder).Length() > fReach * 1.15f)
        grab.m_bHasTarget = false;

      // probe a fan of directions every few steps, alternating the sides. While reaching it keeps probing: the body moves
      // on, and the hand goes for whatever is nearest now rather than the spot it first saw
      if (bLooking && grab.m_fCooldown <= 0.0f && (m_uiProbeCounter + s * 2) % 4 == 0)
      {
        // a probe that finds nothing keeps the last target as long as it is still within reach
        const Vec3 vOut = vRight * (s == 0 ? -1.0f : 1.0f);
        const Vec3 dirs[] = {vFallH, (vFallH - m_vUp).Normalized(), vOut, (vOut - m_vUp).Normalized(), (vOut + vFallH).Normalized(), (vOut + vFallH - m_vUp).Normalized()};

        float fBest = 1e9f;
        for (const Vec3& vDir : dirs)
        {
          Vec3 vPoint, vNormal;
          bool bStatic = false;
          if (!CastRayHit(vShoulder, vDir, fReach, vPoint, vNormal, bStatic) || !bStatic)
            continue;

          // floor-like surfaces are for landing on, not for holding on to
          if (vNormal.Dot(m_vUp) > 0.7f)
            continue;

          const float fDist = (vPoint - vShoulder).Length();
          if (fDist < fBest)
          {
            fBest = fDist;
            grab.m_bHasTarget = true;
            grab.m_vPoint = vPoint;
            grab.m_vNormal = vNormal;
          }
        }
      }
    }
    else
    {
      grab.m_fGripTime += fDeltaTime;
    }

    const bool bReaching = grab.m_bHasTarget || grab.m_pGrip != nullptr;
    grab.m_fReach = bReaching ? plMath::Min(1.0f, grab.m_fReach + fDeltaTime / 0.2f) : plMath::Max(0.0f, grab.m_fReach - fDeltaTime / 0.3f);

    // the hand goes to just off the surface; the reach blends from the arm's current pose
    if (grab.m_fReach > 0.0f && grab.m_bHasTarget)
    {
      // a little below the probed point on a wall: reaching out and down is how a hand goes for a wall, and a straight
      // arm held level is the hardest pose to hold against gravity
      const Vec3 vAim = grab.m_vPoint + grab.m_vNormal * 0.05f - m_vUp * (0.2f * (1.0f - plMath::Abs(grab.m_vNormal.Dot(m_vUp))));
      ProposeArmIk(s, vAim, grab.m_fReach * c);
    }
  }
}

void plJoltRagdollController::UpdateGrips()
{
  using B = plHumanoidBone;
  const float c = plMath::Saturate(m_Inputs.m_fConsciousness);
  const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;

  for (int s = 0; s < 2; ++s)
  {
    Grab& grab = m_Grab[s];
    const plInt16 iHand = GetPart(s == 0 ? B::LeftHand : B::RightHand);
    if (iHand < 0)
      continue;

    Body* pHand = m_Parts[iHand].m_pBody;

    if (grab.m_pGrip != nullptr)
    {
      // dragged further off the hold than the hand can take, it tears loose. It lets go by itself once the body is down, after
      // a while, or when consciousness goes
      const bool bTorn = (pHand->GetCenterOfMassPosition() - grab.m_vHold).Length() > s_fGripStretch + 0.02f;
      const bool bDone = grab.m_fGripTime > 1.5f || c < 0.2f || (m_bTorsoContact && m_bFirstImpact);
      if (bTorn || bDone)
        ReleaseGrip(s, bTorn);
      continue;
    }

    if (!grab.m_bHasTarget || grab.m_fReach < 0.3f)
      continue;

    // close enough to the surface, or touching it near the target: take hold where the hand is
    const Vec3 vHand = pHand->GetCenterOfMassPosition();
    const float fDist = (vHand - grab.m_vPoint).Dot(grab.m_vNormal);
    const float fAlong = ((vHand - grab.m_vPoint) - grab.m_vNormal * fDist).Length();
    grab.m_fHandDist = (vHand - grab.m_vPoint).Length();
    // the palm on the surface anywhere near where it aimed is a hold: a straight arm sags a little below its target
    const bool bNear = fDist < 0.12f && fAlong < 0.45f;
    const bool bTouching = m_Parts[iHand].m_bContact && grab.m_fHandDist < 0.6f && plMath::Abs(fDist) < 0.2f;
    if (!bNear && !bTouching)
      continue;

    // A hand's hold gives: a stiff pin would take all of the body's momentum in one step and tear at once. A spring that pulls
    // with the most a hand can hold - half the body's weight - once the hand has been dragged s_fGripStretch off its hold, and
    // tears past that, slows and swings the body instead.
    const float fHold = 0.5f * c * m_fTotalMass * g;
    const float fStiffness = fHold / s_fGripStretch;
    DistanceConstraintSettings settings;
    settings.mSpace = EConstraintSpace::WorldSpace;
    settings.mPoint1 = RVec3(vHand);
    settings.mPoint2 = RVec3(vHand);
    settings.mMinDistance = 0.0f;
    settings.mMaxDistance = 0.02f;
    settings.mLimitsSpringSettings.mMode = ESpringMode::StiffnessAndDamping;
    settings.mLimitsSpringSettings.mStiffness = fStiffness;
    settings.mLimitsSpringSettings.mDamping = 2.0f * 0.6f * plMath::Sqrt(fStiffness * m_fTotalMass);
    grab.m_vHold = vHand;

    TwoBodyConstraint* pGrip = static_cast<TwoBodyConstraint*>(settings.Create(Body::sFixedToWorld, *pHand));
    pGrip->AddRef();
    m_pSystem->AddConstraint(pGrip);
    grab.m_pGrip = pGrip;
    grab.m_fGripTime = 0.0f;
    ++m_uiGripsTaken;
  }
}

void plJoltRagdollController::ReleaseGrip(int iSide, bool bBroken)
{
  Grab& grab = m_Grab[iSide];
  if (grab.m_pGrip != nullptr)
  {
    m_pSystem->RemoveConstraint(grab.m_pGrip);
    grab.m_pGrip->Release();
    grab.m_pGrip = nullptr;
    m_uiGripsBroken += bBroken ? 1 : 0;
  }

  grab.m_bHasTarget = false;
  grab.m_fCooldown = 0.8f;
}

void plJoltRagdollController::UpdateSideLanding(float fDeltaTime)
{
  // Going over, the body turns so that a hip and a shoulder take the impact instead of the back or the face. The side is
  // picked once per fall: the one with a hand on something, else the one the fall already leans to, else either.
  const float c = plMath::Saturate(m_Inputs.m_fConsciousness);
  const bool bCatching = m_fCatchBlend > 0.0f && m_bFallFromFeet && !m_bFirstImpact;

  if (!m_bFallFromFeet)
    m_iLandSide = 0;

  if (bCatching && m_iLandSide == 0)
  {
    if (m_Grab[0].m_bHasTarget != m_Grab[1].m_bHasTarget)
      m_iLandSide = m_Grab[0].m_bHasTarget ? -1 : 1;
    else if (plMath::Abs(m_fFallSide) > 0.2f)
      m_iLandSide = m_fFallSide > 0.0f ? 1 : -1;
    else
      m_iLandSide = (m_uiSeed & 2) ? 1 : -1;
  }

  const float fTarget = (bCatching && m_iLandSide != 0) ? c : 0.0f;
  m_fSideLand = fTarget > m_fSideLand ? plMath::Min(fTarget, m_fSideLand + fDeltaTime / 0.35f) : plMath::Max(fTarget, m_fSideLand - fDeltaTime / 0.6f);

  if (m_fSideLand <= 0.0f || m_iLandSide == 0 || !(m_bFeetContact[0] || m_bFeetContact[1]))
    return;

  // While a foot is still on the ground, it can turn the body: a capped twist about the vertical that brings the landing side
  // round to face the way the body is falling. In the air the body would need something to push against, so it only
  // changes its shape there.
  const plInt16 iPelvis = GetPart(plHumanoidBone::Hips);
  const plInt16 iChest = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Spine);
  if (iPelvis < 0)
    return;

  const Part& pelvis = m_Parts[iPelvis];
  const Vec3 vSide = pelvis.m_pBody->GetRotation() * (pelvis.m_qRestGlobal.Conjugated() * m_RestFrame.m_vRight) * (float)m_iLandSide;
  const Vec3 vSideH = (vSide - m_vUp * vSide.Dot(m_vUp)).NormalizedOr(Vec3::sZero());
  const Vec3 vFallH = (m_vFallDir - m_vUp * m_vFallDir.Dot(m_vUp)).NormalizedOr(Vec3::sZero());
  if (vSideH.IsNearZero() || vFallH.IsNearZero())
    return;

  const float fErr = plMath::ATan2(vSideH.Cross(vFallH).Dot(m_vUp), vSideH.Dot(vFallH)).GetRadian();
  const float fYawRate = pelvis.m_pBody->GetAngularVelocity().Dot(m_vUp);
  const float g = m_fGravity > 0.01f ? m_fGravity : 9.81f;
  const float fMax = 0.15f * m_fTotalMass * g * m_fLegLength * c * m_fSideLand;
  const float fTorque = plMath::Clamp(fMax * plMath::Clamp(fErr / 0.6f, -1.0f, 1.0f) - 0.3f * fMax * fYawRate, -fMax, fMax);

  m_Parts[iPelvis].m_pBody->AddAngularImpulse(m_vUp * (fTorque * 0.6f * fDeltaTime));
  if (iChest >= 0)
    m_Parts[iChest].m_pBody->AddAngularImpulse(m_vUp * (fTorque * 0.4f * fDeltaTime));
}

void plJoltRagdollController::UpdateSaveStep(float fDeltaTime)
{
  // After balance gives up, the rear leg still reaches for the ground the body is falling towards - on stairs, the next
  // step down - to take some of the fall on a bent knee. One reach per fall.
  if (!m_bFallFromFeet)
  {
    m_bSaveStep = false;
    m_iSaveSide = 0;
    return;
  }

  const plInt16 iFeet[2] = {GetPart(plHumanoidBone::LeftFoot), GetPart(plHumanoidBone::RightFoot)};
  if (iFeet[0] < 0 || iFeet[1] < 0)
    return;

  if (m_iSaveSide == 0 && !m_bFirstImpact && m_fGiveUp < 0.3f && m_Inputs.m_fConsciousness > 0.3f)
  {
    const Vec3 vFallH = (m_vFallDir - m_vUp * m_vFallDir.Dot(m_vUp)).NormalizedOr(Vec3::sZero());
    const Vec3 vFoot[2] = {GetJointPosition(m_Parts[iFeet[0]]), GetJointPosition(m_Parts[iFeet[1]])};
    const int iRear = vFoot[0].Dot(vFallH) > vFoot[1].Dot(vFallH) ? 0 : 1;

    // where the ground is, half a leg further along the fall
    const Vec3 vProbe = m_vCom + vFallH * (0.55f * m_fLegLength);
    float fDist = 0.0f;
    if (!vFallH.IsNearZero() && CastRayDown(vProbe, 3.0f, fDist, nullptr))
    {
      Vec3 vTarget = vProbe - m_vUp * fDist;

      // no further from the hip than the leg reaches
      const plInt16 iThigh = GetPart(iRear == 0 ? plHumanoidBone::LeftUpperLeg : plHumanoidBone::RightUpperLeg);
      if (iThigh >= 0)
      {
        const Vec3 vHip = GetJointPosition(m_Parts[iThigh]);
        const Vec3 vFromHip = vTarget - vHip;
        if (vFromHip.Length() > m_fLegLength * 0.95f)
          vTarget = vHip + vFromHip.Normalized() * (m_fLegLength * 0.95f);
      }

      m_iSaveSide = iRear == 0 ? -1 : 1;
      m_vSaveStart = vFoot[iRear];
      m_vSaveTarget = vTarget;
      m_fSaveTime = 0.0f;
      m_bSaveStep = true;
    }
  }

  if (!m_bSaveStep)
    return;

  m_fSaveTime += fDeltaTime;
  const float s = plMath::Saturate(m_fSaveTime / 0.35f);
  const float fLift = 0.1f * plMath::Sin(plAngle::MakeFromRadian(plMath::Pi<float>() * plMath::Pow(s, 0.7f)));
  const Vec3 vFootPos = m_vSaveStart + (m_vSaveTarget - m_vSaveStart) * (s * s * (3.0f - 2.0f * s)) + m_vUp * fLift;

  ProposeTwoBoneIk((plUInt8)(m_iSaveSide < 0 ? plHumanoidBone::LeftUpperLeg : plHumanoidBone::RightUpperLeg), vFootPos, m_Inputs.m_fConsciousness);
  AssistSwingFoot(m_iSaveSide, vFootPos, fDeltaTime, 0.6f);

  const bool bLanded = m_fSaveTime > 0.15f && m_bFeetContact[m_iSaveSide < 0 ? 0 : 1];
  if (bLanded || m_fSaveTime > 0.5f || m_bFirstImpact)
    m_bSaveStep = false;
}

void plJoltRagdollController::Propose(Part& part, const Quat& qTarget, float fWeight, float fTorqueScale)
{
  const float w = fWeight * fTorqueScale;
  if (w <= 0.0001f)
    return;

  // keep all proposals in one hemisphere before summing
  Quat q = qTarget;
  if (part.m_fAccumWeight > 0.0f && part.m_qAccum.Dot(q) < 0.0f)
    q = -q;

  part.m_qAccum = Quat(part.m_qAccum.GetXYZW() + q.GetXYZW() * w);
  part.m_fAccumWeight += w;
  part.m_fAccumTorque += w;
}

void plJoltRagdollController::ProposeAll(float fDeltaTime)
{
  const float c = plMath::Saturate(m_Inputs.m_fConsciousness);
  const auto& beh = m_Inputs.m_Behaviours;

  // body speed decides how much the neutral pose may pull: it shapes the settle, not the fall
  float fSpeed = 0.0f;
  for (const Part& part : m_Parts)
    fSpeed = plMath::Max(fSpeed, part.m_pBody->GetLinearVelocity().Length());
  const float fSettle = plMath::Saturate(1.0f - fSpeed / 2.0f);
  m_fNeutralSettle = fSettle;

  // fall pose: airborne, the windmill runs faster while rising and slower while dropping
  const bool bFall = beh.IsSet(plJoltRagdollBehaviour::FallPose) && m_Profile.m_fFallPoseStrength > 0.0f && m_bHumanoid;
  m_fFallWeight = (bFall && m_bAirborne) ? plMath::Min(1.0f, m_fFallWeight + fDeltaTime / 0.2f) : plMath::Max(0.0f, m_fFallWeight - fDeltaTime / 0.15f);
  if (m_fFallWeight > 0.0f)
  {
    const float vUp = m_vComVel.Dot(m_vUp);
    const float fRate = vUp > 0.0f ? plMath::Lerp(1.0f, m_Profile.m_fFallPoseMaxRate, plMath::Saturate(vUp / 3.0f)) : plMath::Lerp(1.0f, m_Profile.m_fFallPoseMinRate, plMath::Saturate(-vUp / 6.0f));
    m_fFallPhase += 2.0f * plMath::Pi<float>() * 0.8f * fRate * fDeltaTime;
    BuildFallPose(m_fFallPhase);
  }

  // protect face: blended in by downward speed and closeness to the floor, after a reaction delay
  const bool bProtect = beh.IsSet(plJoltRagdollBehaviour::ProtectFace) && m_Profile.m_fBraceStrength > 0.0f && m_bHumanoid;
  if (bProtect)
  {
    const float fDown = plMath::Max(0.0f, -m_vComVel.Dot(m_vUp));
    const float fHeightFactor = plMath::Saturate((m_Profile.m_fBraceHeight - (m_fPelvisHeight - 0.25f)) / plMath::Max(m_Profile.m_fBraceHeight, 0.1f));
    float fRaw = plMath::Saturate(fDown / plMath::Max(m_Profile.m_fBraceFallSpeed, 0.1f)) * fHeightFactor;

    if (m_fHeadTimeToImpact < 0.3f)
      fRaw = plMath::Max(fRaw, 1.0f - m_fHeadTimeToImpact / 0.3f);

    // rung 3: when balance gives up, protection takes over at once instead of waiting for the fall to pick up speed
    if (m_BalanceState == plJoltRagdollBalanceState::Falling)
      fRaw = plMath::Max(fRaw, 0.8f);

    // after landing face down the head stays turned
    if (m_bTorsoContact && m_bFaceDown)
      fRaw = plMath::Max(fRaw, 0.3f);

    // after the first impact of a fall from the feet the head stays guarded while the body is still tumbling, unless the
    // limbs are spread to brake a slide
    if (m_bFallFromFeet && m_bFirstImpact && m_vComVel.Length() > 0.8f)
      fRaw = plMath::Max(fRaw, 0.8f * (1.0f - m_fBrakeBlend));

    fRaw *= c * m_Profile.m_fBraceStrength;
    m_fBraceDelay = fRaw > 0.05f ? m_fBraceDelay + fDeltaTime : 0.0f;
    const float fTarget = m_fBraceDelay >= m_Profile.m_fReactionDelay ? fRaw : 0.0f;
    m_fBraceWeight = fTarget > m_fBraceWeight ? plMath::Min(fTarget, m_fBraceWeight + 8.0f * fDeltaTime) : plMath::Max(fTarget, m_fBraceWeight - 2.0f * fDeltaTime);

    // the longer the fall lasts, the further the arms come in towards the guard; it eases back out after landing
    const bool bFalling = !m_bTorsoContact && (m_bAirborne || m_BalanceState == plJoltRagdollBalanceState::Falling);
    m_fFallTime = bFalling ? m_fFallTime + fDeltaTime : 0.0f;
    const float fGuardTarget = c * s_fGuardMax * plMath::Saturate((m_fFallTime - s_fGuardStart) / s_fGuardRamp);
    m_fGuardBlend = fGuardTarget > m_fGuardBlend ? fGuardTarget : plMath::Max(fGuardTarget, m_fGuardBlend - fDeltaTime);

    // which way the body is going over: chest to the sky, or moving towards its back, takes the back guard
    {
      float fBack = 0.0f;
      const plInt16 iTorso = GetPart(plHumanoidBone::Chest) >= 0 ? GetPart(plHumanoidBone::Chest) : GetPart(plHumanoidBone::Hips);
      if (iTorso >= 0)
      {
        const Part& torso = m_Parts[iTorso];
        const Vec3 vForward = torso.m_pBody->GetRotation() * (torso.m_qRestGlobal.Conjugated() * m_RestFrame.m_vForward);
        const Vec3 vHorizontalVel = m_vComVel - m_vUp * m_vComVel.Dot(m_vUp);
        const float fFaceUp = vForward.Dot(m_vUp);
        const float fBackwards = -vHorizontalVel.Dot(vForward);
        fBack = plMath::Saturate(plMath::Max(fFaceUp * 2.0f, fBackwards / 1.5f));
      }

      const float fRate = 4.0f * fDeltaTime;
      m_fBackBlend = plMath::Clamp(fBack, m_fBackBlend - fRate, m_fBackBlend + fRate);
    }
  }
  else
  {
    m_fBraceWeight = 0.0f;
    m_fGuardBlend = 0.0f;
    m_fBackBlend = 0.0f;
  }

  const float fAlpha = plMath::Saturate(m_fTimeInFrame / m_Inputs.m_fFrameTime);

  for (Part& part : m_Parts)
  {
    part.m_qAccum = Quat::sZero();
    part.m_fAccumWeight = 0.0f;
    part.m_fAccumTorque = 0.0f;

    if (part.m_pConstraint == nullptr)
      continue;

    const auto& mus = m_Profile.GetMuscles(part.m_Region);
    const bool bUpper = IsUpperBody(part.m_Region);

    // the leg taking a recovery step follows the step alone, and the arms leave the animation while they fight for balance
    const bool bSwingLeg = IsSwingLeg(part);
    const bool bArm = part.m_Region == plJoltRagdollRegion::UpperArm || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Hand;
    float fAnim = bSwingLeg ? 0.0f : (bUpper ? m_fUpperAnimWeight : m_fLegsAnimWeight);
    if (bArm)
      fAnim *= 1.0f - m_fStaggerBlend;

    // an arm going for a hold does only that: averaged with the flail or the catch it would never get there
    if (bArm && part.m_bHasIk && part.m_iSide != 0)
    {
      const Grab& grab = m_Grab[part.m_iSide > 0 ? 1 : 0];
      if (grab.m_fReach > 0.0f && (grab.m_bHasTarget || grab.m_pGrip != nullptr))
      {
        Propose(part, part.m_qIk, 3.0f, c);
        continue;
      }
    }

    if (m_Inputs.m_bAnimationDriven && m_bHasAnimTarget)
    {
      Propose(part, Blend(part.m_qAnimPrev, part.m_qAnimCur, fAlpha), fAnim, m_Inputs.m_bActiveMode ? c : 1.0f);
    }
    else if (m_Inputs.m_bActiveMode)
    {
      // Active without an animation playing: hold the bind pose
      Propose(part, part.m_qRelRest, fAnim, c);
    }

    // the neutral pose fills in for whatever the animation does not drive
    const bool bAnimates = (m_Inputs.m_bAnimationDriven && m_bHasAnimTarget) || m_Inputs.m_bActiveMode;
    const float fFreeShare = bAnimates ? 1.0f - plMath::Saturate(fAnim * (m_Inputs.m_bActiveMode ? c : 1.0f)) : 1.0f;

    // Lying on the ground once it has settled, a conscious body lies back: the float posture would curl it up against the
    // ground and arch the back, so the pull eases and its target straightens out towards the rest pose.
    if ((beh.IsSet(plJoltRagdollBehaviour::NeutralPose) || part.m_bDislocated) && !bSwingLeg)
    {
      const float fLie = (m_Inputs.m_bActiveMode && m_BalanceState == plJoltRagdollBalanceState::Down && !part.m_bDislocated) ? fSettle : 0.0f;
      const Quat qNeutral = fLie > 0.0f ? Blend(part.m_qNeutral, part.m_qRelRest, 0.6f * fLie) : part.m_qNeutral;
      Propose(part, qNeutral, mus.m_fNeutralPull * (0.35f + 0.65f * fSettle) * (1.0f - 0.5f * fLie) * fFreeShare, 1.0f);
    }

    if (m_fFallWeight > 0.0f)
      Propose(part, part.m_qFall, m_fFallWeight * m_Profile.m_fFallPoseStrength, c);

    // stumbling: arms thrown out against the push, forwards when going backwards and the other way round
    if (m_fStaggerBlend > 0.0f && bUpper)
      Propose(part, Blend(part.m_qStaggerFront, part.m_qStaggerBack, plMath::Saturate(0.5f - 0.75f * m_fFallFwd)), 1.5f * m_fStaggerBlend, c);

    // protection: on the way down the whole body sets up to catch the fall, then the elbows give and the arms come in to
    // guard the head
    if (m_fBraceWeight > 0.0f)
    {
      Quat qProtect = m_fGuardBlend > 0.0f ? Blend(part.m_qBrace, part.m_qGuard, m_fGuardBlend) : part.m_qBrace;
      if (m_fBackBlend > 0.0f)
        qProtect = Blend(qProtect, part.m_qBackGuard, m_fBackBlend);
      Propose(part, qProtect, m_fBraceWeight, 1.0f);
    }

    // the arms reach with full strength; the trunk and legs only lean into the catch
    if (m_fCatchBlend > 0.0f)
      Propose(part, GetCatchPose(part), (bArm ? 1.6f : 1.0f) * m_fCatchBlend, bArm ? 1.0f : 0.6f);

    // going over backwards out of a stumble, the leg in front - not the one that just stepped back - comes up as the body
    // tips, knee first, while the rear one folds under it
    const plInt8 iRearLeg = m_iSaveSide != 0 ? m_iSaveSide : m_iSwingSide;
    if (m_fCatchBlend > 0.0f && iRearLeg != 0 && part.m_iSide == -iRearLeg && m_fFallFwd < -0.3f && m_fSideLand < 0.5f &&
        (part.m_Region == plJoltRagdollRegion::Thigh || part.m_Region == plJoltRagdollRegion::Shin || part.m_Region == plJoltRagdollRegion::Foot))
      Propose(part, Blend(part.m_qCatchBack, part.m_qBrace, 0.6f), 1.2f * m_fCatchBlend * plMath::Saturate(-m_fFallFwd), 0.6f);

    const bool bLimb = part.m_Region == plJoltRagdollRegion::UpperArm || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Thigh || part.m_Region == plJoltRagdollRegion::Shin;

    if (m_bRolling && part.m_iSide == m_iUphillSide && bLimb)
      Propose(part, part.m_qSplay, 0.8f, plMath::Max(0.4f, c));

    // sliding on the ground: everything spreads out to dig in
    if (m_fBrakeBlend > 0.0f && bLimb)
      Propose(part, part.m_qSplay, 0.9f * m_fBrakeBlend, plMath::Max(0.4f, c));

    // rolling with a hand on the ground: that arm braces out against the roll
    if (bArm && part.m_iSide != 0 && m_fArmPush[part.m_iSide > 0 ? 1 : 0] > 0.0f)
      Propose(part, part.m_qSplay, 1.5f * m_fArmPush[part.m_iSide > 0 ? 1 : 0], c);

    // a stepping leg and a reaching arm follow their target over everything else
    if (part.m_bHasIk)
      Propose(part, part.m_qIk, (bSwingLeg || bArm) ? 3.0f : 1.5f, c);
  }
}

void plJoltRagdollController::ApplyArbiter(float fDeltaTime)
{
  const float fStrength = m_Inputs.m_fStrengthScale;

  for (Part& part : m_Parts)
  {
    SwingTwistConstraint* pConstraint = part.m_pConstraint;
    if (pConstraint == nullptr)
      continue;

    const auto& mus = m_Profile.GetMuscles(part.m_Region);

    // effort of the last step: motor impulse against what the torque limit allows
    {
      const float fLimit = pConstraint->GetSwingMotorSettings().mMaxTorqueLimit;
      const float fLambda = pConstraint->GetTotalLambdaMotor().Length();
      part.m_fEffort = fLimit > 1e-4f ? plMath::Saturate(fLambda / (fLimit * fDeltaTime)) : 0.0f;
    }

    const Vec3 vRelAngVel = part.m_pBody->GetAngularVelocity() - (part.m_iParent >= 0 ? m_Parts[part.m_iParent].m_pBody->GetAngularVelocity() : Vec3::sZero());

    if (part.m_fAccumTorque > 0.02f)
    {
      const Quat qTarget = part.m_qAccum.Normalized();
      const float fFrequency = plMath::Max(mus.m_fFrequency, 1.5f);

      // Jolt's frequency springs are scaled by the two bodies at the joint. A light body between heavy ones - a neck under a
      // head, a forearm under a hand - makes them far too soft to hold anything up. Where something has to be held against
      // gravity, the muscle gets an absolute stiffness against the inertia it actually moves: a standing body's legs and torso
      // against the body they carry, the arms of a body in the air, going over or staggering against their chain, the neck
      // and head always (a tucked chin). A body that has come down keeps the soft springs: a stiff limb pressed into the ground moves
      // the whole body, far heavier than the limb its damping is sized for, and rings.
      // A leg taking a step carries nothing: it is stiff against its own chain, so it can swing quickly.
      const bool bSwingLeg = IsSwingLeg(part);
      // A balancing body carries itself, and keeps doing so through the first half of the hand-over after balance gives up:
      // the muscles are still working while the arms come round to catch the fall. After that the legs and trunk give.
      const bool bHandingOver = m_BalanceState == plJoltRagdollBalanceState::Falling && m_fGiveUp < 1.0f;
      const bool bBalancing = m_BalanceState == plJoltRagdollBalanceState::Standing || m_BalanceState == plJoltRagdollBalanceState::Stepping || bHandingOver;
      const bool bLoadBearing = m_Inputs.m_bActiveMode && bBalancing && (m_bFeetContact[0] || m_bFeetContact[1]) && !bSwingLeg &&
                                (!IsUpperBody(part.m_Region) || part.m_Region == plJoltRagdollRegion::Spine || part.m_Region == plJoltRagdollRegion::Chest);
      const bool bHead = part.m_Region == plJoltRagdollRegion::Head || part.m_Region == plJoltRagdollRegion::Neck;
      // Going over or in the air, only the arms (reaching for the ground, guarding the head) hold firm; the trunk and legs give
      // with the fall instead of going over in one piece.
      const bool bArm = part.m_Region == plJoltRagdollRegion::UpperArm || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Hand;
      const bool bGoingOver = m_Inputs.m_bActiveMode && m_BalanceState == plJoltRagdollBalanceState::Falling;
      const bool bStaggering = m_fStaggerBlend > 0.05f && bArm; // arms thrown out against a push
      const bool bReactiveArm = bArm && (m_bAirborne || bGoingOver || bStaggering);
      const bool bStiff = bLoadBearing || bHead || bReactiveArm || bSwingLeg;

      const float fGravityTorque = bLoadBearing ? plMath::Max(part.m_fGravityTorque, m_fTotalMass * m_fGravity * 0.15f) : part.m_fGravityTorque;

      // A muscle that pushes at its limit, far from its target, without the joint moving, is pressing a limb into something
      // it cannot move - the ground under a body that lies there. Pushing on would only make the body twitch, so it gives in
      // over half a second and takes up again as soon as the limb is free or the target moves. Legs that carry the body never
      // give in.
      {
        bool bStuck = !bLoadBearing && part.m_fEffort > 0.9f && vRelAngVel.LengthSq() < 0.25f;
        if (bStuck)
        {
          // more than ~11° from the target: |cos(angle / 2)| below cos(5.7°)
          const Quat qRel = part.m_iParent >= 0 ? m_Parts[part.m_iParent].m_pBody->GetRotation().Conjugated() * part.m_pBody->GetRotation() : Quat::sIdentity();
          bStuck = plMath::Abs(qRel.Normalized().Dot(qTarget)) < 0.995f;
        }
        part.m_fYield = bStuck ? plMath::Min(1.0f, part.m_fYield + fDeltaTime / 0.5f) : plMath::Max(0.0f, part.m_fYield - fDeltaTime / 0.3f);
      }

      // through the hand-over the legs and trunk carry less and less, so the knees buckle progressively instead of letting go
      const float fBuckle = (bLoadBearing && bHandingOver) ? 1.0f - 0.75f * m_fGiveUp : 1.0f;
      const float fTorque = mus.m_fTorqueCap * fGravityTorque * plMath::Min(part.m_fAccumTorque, 1.5f) * fStrength * fBuckle * (1.0f - 0.85f * part.m_fYield);

      // Arms flung out, reaching or guarding are not holding anything up: a softer spring with the muscle's own (under-)damping
      // lets them swing, overshoot a little and trail the body's motion instead of snapping into place like an action
      // figure's. What carries weight - stance legs, trunk, neck - stays at least critically damped so it never rings.
      const float fInertia = bLoadBearing ? part.m_fSupportInertia : part.m_fChainInertia;
      const float fOmega = 2.0f * plMath::Pi<float>() * fFrequency * (bReactiveArm ? 0.7f : 1.0f);
      const float fStiffness = fOmega * fOmega * fInertia;
      const float fZeta = bReactiveArm ? mus.m_fDampingRatio : plMath::Max(mus.m_fDampingRatio, 1.0f);

      for (MotorSettings* pMotor : {&pConstraint->GetSwingMotorSettings(), &pConstraint->GetTwistMotorSettings()})
      {
        if (bStiff)
        {
          pMotor->mSpringSettings.mMode = ESpringMode::StiffnessAndDamping;
          pMotor->mSpringSettings.mStiffness = fStiffness;
          pMotor->mSpringSettings.mDamping = 2.0f * fZeta * plMath::Sqrt(fStiffness * fInertia);
        }
        else
        {
          pMotor->mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
          pMotor->mSpringSettings.mFrequency = fFrequency;
          pMotor->mSpringSettings.mDamping = mus.m_fDampingRatio;
        }

        pMotor->SetTorqueLimit(fTorque);
      }

      pConstraint->SetSwingMotorState(EMotorState::Position);
      pConstraint->SetTwistMotorState(EMotorState::Position);
      pConstraint->SetTargetOrientationBS(qTarget);
    }
    else
    {
      part.m_fYield = plMath::Max(0.0f, part.m_fYield - fDeltaTime / 0.3f);

      // no target: a velocity motor towards zero relative speed with a torque that grows with the speed gives viscous
      // damping that can never overshoot
      const float fZeta = plMath::Max(m_Profile.m_fLimpDampingFloor, mus.m_fDampingRatio * 0.25f);
      const float fOmegaN = 2.0f * plMath::Pi<float>() * plMath::Max(mus.m_fFrequency, 3.0f);
      const float fTorque = 2.0f * fZeta * fOmegaN * part.m_fChainInertia * vRelAngVel.Length();

      pConstraint->GetSwingMotorSettings().SetTorqueLimit(fTorque);
      pConstraint->GetTwistMotorSettings().SetTorqueLimit(fTorque);
      pConstraint->SetSwingMotorState(EMotorState::Velocity);
      pConstraint->SetTwistMotorState(EMotorState::Velocity);
      pConstraint->SetTargetAngularVelocityCS(Vec3::sZero());
    }
  }
}

void plJoltRagdollController::ApplySoftLimits(float fDeltaTime)
{
  const float fZoneRad = Deg(m_Profile.m_fSoftZoneDeg);
  const float fOmegaS = 2.0f * plMath::Pi<float>() * m_Profile.m_fSoftZoneStiffness;
  constexpr float fBounceSpeed = 1.5708f; // 90°/s

  for (Part& part : m_Parts)
  {
    part.m_bInSoftZone = false;
    part.m_bWasAtLimit = false;
    part.m_fArrivalSpeed = 0.0f;

    SwingTwistConstraint* pConstraint = part.m_pConstraint;
    if (pConstraint == nullptr || part.m_iParent < 0)
      continue;

    Body* pBody1 = m_Parts[part.m_iParent].m_pBody;
    Body* pBody2 = part.m_pBody;
    // a joint that barely turns can neither bounce nor push into its limit
    if ((pBody2->GetAngularVelocity() - pBody1->GetAngularVelocity()).LengthSq() < 0.25f && part.m_vPrevRelAngVel.LengthSq() < 0.25f)
      continue;

    Quat q = pConstraint->GetRotationInConstraintSpace();
    if (q.GetW() < 0.0f)
      q = -q;

    // cheap early-out for joints nowhere near a limit: swing below the smaller cone angle minus the zone, twist inside its range
    {
      const float fMinCone = plMath::Min(pConstraint->GetPlaneHalfConeAngle(), pConstraint->GetNormalHalfConeAngle());
      const float fCosHalfSwing = plMath::Sqrt(q.GetW() * q.GetW() + q.GetX() * q.GetX());
      const float fSwingFree = plMath::Max(0.0f, fMinCone - fZoneRad);
      const float fTwistNow = 2.0f * plMath::ATan2(q.GetX(), q.GetW()).GetRadian();
      const float fTwistZone = plMath::Min(fZoneRad, (pConstraint->GetTwistMaxAngle() - pConstraint->GetTwistMinAngle()) * 0.25f);

      if (fCosHalfSwing > plMath::Cos(plAngle::MakeFromRadian(0.5f * fSwingFree)) && fTwistNow > pConstraint->GetTwistMinAngle() + fTwistZone && fTwistNow < pConstraint->GetTwistMaxAngle() - fTwistZone)
        continue;
    }

    if (part.m_pFrameOwner != pConstraint)
    {
      part.m_pFrameOwner = pConstraint;
      part.m_qConstraintToBody1 = pConstraint->GetConstraintToBody1Matrix().GetQuaternion();
    }

    const Quat qFrame1 = pBody1->GetRotation() * part.m_qConstraintToBody1;
    const Vec3 vRel = pBody2->GetAngularVelocity() - pBody1->GetAngularVelocity();
    const Vec3 vPrevRel = part.m_vPrevRelAngVel;

    Quat qSwing, qTwist;
    q.GetSwingTwist(qSwing, qTwist);

    // soft limits stand for passive tissue: when a muscle drives the joint on purpose, e.g. straightening a knee to stand,
    // it may go into the soft zone, so the resistance fades with how much the muscles are in charge
    const float fPassive = 1.0f - plMath::Saturate(part.m_fAccumTorque);

    auto Resist = [&](Vec3Arg vAxis, float fDepth, float fZone, float fOutSpeed, float fPrevOutSpeed)
    {
      part.m_bInSoftZone = true;

      if (fDepth >= fZone * 0.98f)
      {
        part.m_bWasAtLimit = true;

        if (fPrevOutSpeed > part.m_fArrivalSpeed)
        {
          part.m_fArrivalSpeed = fPrevOutSpeed;
          part.m_vArrivalAxis = vAxis;

          if (part.m_Region == plJoltRagdollRegion::UpperArm || part.m_Region == plJoltRagdollRegion::LowerArm || part.m_Region == plJoltRagdollRegion::Shin)
            m_fMaxArrivalSpeed = plMath::Max(m_fMaxArrivalSpeed, fPrevOutSpeed);
        }

        // a bounce: arriving at the stop fast and leaving it fast
        if (fPrevOutSpeed > fBounceSpeed && -fOutSpeed > fBounceSpeed)
          ++m_uiLimitBounces;
      }

      if (fOutSpeed <= 0.0f || fZone <= 0.0f || fPassive <= 0.0f)
        return;

      // one-way: only motion into the limit is resisted, a spring growing with depth plus extra damping. The impulse acts on
      // the two bodies' relative rotation and never does more than stop it, so it cannot throw the limb back
      const float fInertia = PairInertia(pBody1, pBody2, vAxis);
      const float x = fDepth / fZone;
      const float fTorque = fInertia * (fOmegaS * fOmegaS * fDepth * x + 2.0f * 0.7f * fOmegaS * x * fOutSpeed);
      const float fImpulse = fPassive * plMath::Min(fTorque * fDeltaTime, fInertia * fOutSpeed);

      pBody2->AddAngularImpulse(-vAxis * fImpulse);
      pBody1->AddAngularImpulse(vAxis * fImpulse);
    };

    // swing: elliptical cone, Y = plane half angle, Z = normal half angle
    {
      const float fSwingW = plMath::Clamp(qSwing.GetW(), -1.0f, 1.0f);
      const float fAlpha = 2.0f * plMath::ACos(fSwingW).GetRadian();
      const Vec3 vAxisCS = Vec3(0.0f, qSwing.GetY(), qSwing.GetZ());

      if (fAlpha > 1e-3f && vAxisCS.LengthSq() > 1e-8f)
      {
        const Vec3 vA = vAxisCS.Normalized();
        const float fLimY = plMath::Max(pConstraint->GetPlaneHalfConeAngle(), 1e-3f);
        const float fLimZ = plMath::Max(pConstraint->GetNormalHalfConeAngle(), 1e-3f);
        const float fLimit = 1.0f / plMath::Sqrt(plMath::Square(vA.GetY() / fLimY) + plMath::Square(vA.GetZ() / fLimZ));
        const float fZone = plMath::Min(fZoneRad, fLimit * 0.5f);
        const float fDepth = fAlpha - (fLimit - fZone);

        if (fDepth > 0.0f)
        {
          const Vec3 vAxisWorld = qFrame1 * vA;
          Resist(vAxisWorld, plMath::Min(fDepth, fZone), fZone, vRel.Dot(vAxisWorld), vPrevRel.Dot(vAxisWorld));
        }
      }
    }

    // twist about X
    {
      const float fTwist = 2.0f * plMath::ATan2(qTwist.GetX(), qTwist.GetW()).GetRadian();
      const float fMin = pConstraint->GetTwistMinAngle();
      const float fMax = pConstraint->GetTwistMaxAngle();
      const float fZone = plMath::Min(fZoneRad, (fMax - fMin) * 0.25f);
      const Vec3 vAxisWorld = qFrame1 * Vec3::sAxisX();
      const float fSpeed = vRel.Dot(vAxisWorld);
      const float fPrevSpeed = vPrevRel.Dot(vAxisWorld);

      if (fTwist > fMax - fZone)
        Resist(vAxisWorld, plMath::Min(fTwist - (fMax - fZone), fZone), fZone, fSpeed, fPrevSpeed);
      else if (fTwist < fMin + fZone)
        Resist(-vAxisWorld, plMath::Min((fMin + fZone) - fTwist, fZone), fZone, -fSpeed, -fPrevSpeed);
    }
  }
}

void plJoltRagdollController::UpdateInjury(float fDeltaTime)
{
  if (!m_Inputs.m_Behaviours.IsSet(plJoltRagdollBehaviour::Injury) || !m_Profile.m_bInjuryEnabled)
    return;

  for (Part& part : m_Parts)
  {
    if (part.m_bDislocated || !part.m_bWasAtLimit || part.m_pOriginal == nullptr || part.m_iParent < 0)
      continue;

    // shoulders, elbows and knees; wrists and ankles never dislocate
    if (part.m_Region != plJoltRagdollRegion::UpperArm && part.m_Region != plJoltRagdollRegion::LowerArm && part.m_Region != plJoltRagdollRegion::Shin)
      continue;

    const float fThreshold = Deg(m_Profile.GetMuscles(part.m_Region).m_fDislocationSpeed);
    if (fThreshold <= 0.0f)
      continue;

    // the speed into the limit the joint arrived at the stop with, before the solver stopped it
    const float fSpeed = part.m_fArrivalSpeed;
    if (fSpeed <= fThreshold || part.m_vArrivalAxis.LengthSq() < 0.5f)
      continue;

    Part& parent = m_Parts[part.m_iParent];
    const Vec3 vAxis = part.m_vArrivalAxis.Normalized();

    part.m_bDislocated = true;
    part.m_bDislocationPending = true;
    part.m_vDislocationAxis = vAxis;

    // a capped jolt as an internal pair, then a few steps in which neither body may gain more than a little speed
    const float fJolt = m_Profile.m_fInjuryJolt * PairInertia(parent.m_pBody, part.m_pBody, vAxis) * fSpeed;
    part.m_pBody->AddAngularImpulse(vAxis * fJolt);
    parent.m_pBody->AddAngularImpulse(-vAxis * fJolt);

    // the jolt travels through the whole chain, so every body is held to its current speed plus a little
    for (Part& p : m_Parts)
    {
      p.m_uiLaunchClamp = 3;
      p.m_fLaunchSpeedLimit = p.m_pBody->GetLinearVelocity().Length() + 0.3f;
    }

    auto& msg = m_Dislocations.ExpandAndGetRef();
    msg.m_Region = part.m_Region;
    msg.m_vPosition = plJoltConversionUtils::ToVec3(GetJointPosition(part));
    msg.m_fSpeed = plAngle::RadToDeg(fSpeed);
    msg.m_sJointName.Assign(m_pRagdoll->GetRagdollSettings()->GetSkeleton()->GetJoint((int)(&part - m_Parts.GetData())).mName.c_str());
  }

  (void)fDeltaTime;
}

void plJoltRagdollController::RecordEvents(float fDeltaTime)
{
  const Vec3 vGravity = m_pSystem->GetGravity();

  for (Part& part : m_Parts)
  {
    part.m_fImpactCooldown = plMath::Max(0.0f, part.m_fImpactCooldown - fDeltaTime);

    if (!part.m_bContact || part.m_fImpactCooldown > 0.0f || m_fEventBudget < 1.0f)
      continue;

    const Vec3 v = part.m_pBody->GetLinearVelocity();
    const Vec3 vDelta = v - part.m_vPrevLinVel - vGravity * fDeltaTime;
    const float fImpulse = vDelta.Length() * part.m_fMass;

    if (fImpulse < 2.0f + 0.5f * part.m_fMass)
      continue;

    const float fNormalSpeed = plMath::Abs(v.Dot(m_vUp));
    const float fTangentSpeed = (v - m_vUp * v.Dot(m_vUp)).Length();

    auto& msg = m_Impacts.ExpandAndGetRef();
    msg.m_Region = part.m_Region;
    msg.m_vPosition = plJoltConversionUtils::ToVec3(part.m_pBody->GetCenterOfMassPosition());
    msg.m_vNormal = plJoltConversionUtils::ToVec3(m_vGroundNormal);
    msg.m_fImpulse = fImpulse;
    msg.m_bSliding = fTangentSpeed > 0.7f && fNormalSpeed < 0.3f;

    part.m_fImpactCooldown = 0.1f;
    m_fEventBudget -= 1.0f;
  }
}

void plJoltRagdollController::RecordTimeline()
{
  TimelineSample s;
  s.m_fTime = m_fTime;
  s.m_fConsciousness = m_Inputs.m_fConsciousness;
  s.m_fWeights[0] = (m_Inputs.m_bAnimationDriven && m_bHasAnimTarget) ? m_fUpperAnimWeight : 0.0f;
  s.m_fWeights[1] = m_Inputs.m_bActiveMode && m_fTakeoverTime >= 0.0f ? 1.0f - m_fLegsAnimWeight * 0.5f - m_fUpperAnimWeight * 0.5f : 0.0f;
  s.m_fWeights[2] = m_BalanceState == plJoltRagdollBalanceState::Stepping ? 1.0f : (m_BalanceState == plJoltRagdollBalanceState::Standing && m_Inputs.m_bActiveMode ? 0.4f : 0.0f);
  s.m_fWeights[3] = m_fFallWeight;
  s.m_fWeights[4] = m_fBraceWeight;
  s.m_fWeights[5] = m_bRolling ? 1.0f : 0.0f;
  s.m_fWeights[6] = 0.35f + 0.65f * m_fNeutralSettle;
  s.m_fWeights[7] = m_Dislocations.IsEmpty() ? 0.0f : 1.0f;

  if (m_Timeline.GetCount() < 240)
  {
    m_Timeline.PushBack(s);
  }
  else
  {
    m_Timeline[m_uiTimelineNext] = s;
    m_uiTimelineNext = (m_uiTimelineNext + 1) % 240;
  }
}

//////////////////////////////////////////////////////////////////////////
// Debug drawing

void plJoltRagdollController::DebugDraw(const plDebugRendererContext& context, bool bTimeline) const
{
  if (m_Parts.IsEmpty())
    return;

  plHybridArray<plDebugRenderer::Line, 64> lines;

  for (const Part& part : m_Parts)
  {
    if (part.m_iParent < 0)
      continue;

    const float e = part.m_fEffort;
    const plColor col = e < 0.4f ? plColor(0.45f, 0.76f, 0.57f) : (e < 0.8f ? plColor(0.91f, 0.69f, 0.29f) : plColor(1.0f, 0.42f, 0.42f));
    auto& line = lines.ExpandAndGetRef();
    line.m_start = plJoltConversionUtils::ToVec3(m_Parts[part.m_iParent].m_vPosition);
    line.m_end = plJoltConversionUtils::ToVec3(part.m_vPosition);
    line.m_startColor = col;
    line.m_endColor = col;

    if (part.m_bInSoftZone)
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(line.m_end, 0.04f), plColor::Yellow);

    if (part.m_bDislocated)
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(line.m_end, 0.07f), plColor::Red);
  }

  plDebugRenderer::DrawLinesOccluded(context, lines, plColor::White);

  // where the muscles want each bone, in cyan
  lines.Clear();
  for (const Part& part : m_Parts)
  {
    if (!part.m_bHasTargetEnd)
      continue;

    auto& line = lines.ExpandAndGetRef();
    line.m_start = plJoltConversionUtils::ToVec3(part.m_vPosition);
    line.m_end = plJoltConversionUtils::ToVec3(part.m_vTargetEnd);
    line.m_startColor = line.m_endColor = plColor(0.3f, 0.95f, 1.0f);
  }
  plDebugRenderer::DrawLinesOccluded(context, lines, plColor::White);

  // what the hands reach for: green while reaching, red while holding on
  for (const Grab& grab : m_Grab)
  {
    if (grab.m_bHasTarget || grab.m_pGrip != nullptr)
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(plJoltConversionUtils::ToVec3(grab.m_vPoint), 0.06f), grab.m_pGrip ? plColor::Red : plColor::LimeGreen);
  }

  // centre of mass and, while balancing, the capture point
  const plVec3 vCom = plJoltConversionUtils::ToVec3(m_vCom);
  plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(vCom, 0.06f), plColor::White);

  if (m_Inputs.m_bActiveMode && m_BalanceState != plJoltRagdollBalanceState::Down)
  {
    const plVec3 vXi = plJoltConversionUtils::ToVec3(m_vCapturePoint);
    plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(vXi, 0.05f), plColor(0.78f, 0.61f, 0.95f));

    if (m_BalanceState == plJoltRagdollBalanceState::Stepping)
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(plJoltConversionUtils::ToVec3(m_vStepTarget), 0.08f), plColor(0.43f, 0.66f, 1.0f));
  }

  plStringBuilder sText;
  sText.SetFormat("{}\nconsciousness {}\nbalance {} steps {} assist {}% {}\nanimation {} upper {}% legs {}%", GetActiveBehaviourName(), plArgF(m_Inputs.m_fConsciousness, 2), (int)m_BalanceState, m_uiStepsTaken,
    (int)(m_fBalanceAssist * 100.0f), m_szFallReason, (m_Inputs.m_bAnimationDriven && m_bHasAnimTarget) ? "followed" : "none", (int)(m_fUpperAnimWeight * 100.0f), (int)(m_fLegsAnimWeight * 100.0f));
  sText.AppendFormat("\nstagger {}% catch {}% brake {}% arm push {}% {}%", (int)(m_fStaggerBlend * 100.0f), (int)(m_fCatchBlend * 100.0f), (int)(m_fBrakeBlend * 100.0f), (int)(m_fArmPush[0] * 100.0f),
    (int)(m_fArmPush[1] * 100.0f));
  sText.AppendFormat("\nfall dir fwd {} side {}  land {} {}%  leg reach {}", plArgF(m_fFallFwd, 2), plArgF(m_fFallSide, 2), m_iLandSide < 0 ? "L" : (m_iLandSide > 0 ? "R" : "-"),
    (int)(m_fSideLand * 100.0f), m_bSaveStep ? "on" : "off");
  sText.AppendFormat("\ngrab L {}% {}{} R {}% {}{}  grips {} torn {}", (int)(m_Grab[0].m_fReach * 100.0f), plArgF(m_Grab[0].m_fHandDist, 2), m_Grab[0].m_pGrip ? " held" : "", (int)(m_Grab[1].m_fReach * 100.0f),
    plArgF(m_Grab[1].m_fHandDist, 2), m_Grab[1].m_pGrip ? " held" : "", m_uiGripsTaken, m_uiGripsBroken);
  sText.AppendFormat("\nbrace {}% guard {}% back {}%",(int)(m_fBraceWeight * 100.0f), (int)(m_fGuardBlend * 100.0f), (int)(m_fBackBlend * 100.0f));
  plDebugRenderer::Draw3DText(context, sText, vCom + plJoltConversionUtils::ToVec3(m_vUp) * 1.1f, plColor::White);

  if (!bTimeline || m_Timeline.IsEmpty())
    return;

  // a strip of lanes, newest sample on the right
  const char* szLanes[] = {"Animation", "Takeover", "Balance", "Fall Pose", "Protect Face", "Roll Guard", "Neutral Pose", "Injury"};
  const plColor laneCol[] = {plColor(0.39f, 0.75f, 0.73f), plColor(0.59f, 0.69f, 0.76f), plColor(0.88f, 0.69f, 0.51f), plColor(0.43f, 0.66f, 1.0f), plColor(0.78f, 0.61f, 0.95f), plColor(0.44f, 0.76f, 0.79f), plColor(0.61f, 0.81f, 0.45f), plColor(1.0f, 0.42f, 0.42f)};

  const float x0 = 140.0f, y0 = 40.0f, laneH = 16.0f, w = 2.0f;
  const plUInt32 n = m_Timeline.GetCount();

  for (plUInt32 l = 0; l < 8; ++l)
  {
    plDebugRenderer::Draw2DText(context, szLanes[l], plVec2I32(10, (int)(y0 + l * laneH)), laneCol[l], 12);

    for (plUInt32 i = 0; i < n; ++i)
    {
      const TimelineSample& s = m_Timeline[(m_uiTimelineNext + i) % n];
      const float a = s.m_fWeights[l];
      if (a <= 0.01f)
        continue;

      plColor c = laneCol[l];
      c.a = plMath::Saturate(a);
      plDebugRenderer::Draw2DRectangle(context, plRectFloat(x0 + i * w, y0 + l * laneH + 2.0f, w, laneH - 4.0f), 0.0f, c);
    }
  }

  for (plUInt32 i = 0; i < n; ++i)
  {
    const TimelineSample& s = m_Timeline[(m_uiTimelineNext + i) % n];
    plDebugRenderer::Draw2DRectangle(context, plRectFloat(x0 + i * w, y0 + 8 * laneH + 30.0f - s.m_fConsciousness * 26.0f, w, 2.0f), 0.0f, plColor::White);
  }

  plDebugRenderer::Draw2DText(context, "Consciousness", plVec2I32(10, (int)(y0 + 8 * laneH + 10.0f)), plColor::White, 12);
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltRagdollController);
