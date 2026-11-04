#include "JoltTestSupport.h"

#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/EventMessageHandlerComponent.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Time/Time.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <JoltPlugin/Components/JoltRagdollComponent.h>
#include <RendererCore/AnimationSystem/Declarations.h>
#include <RendererCore/AnimationSystem/SkeletonBuilder.h>
#include <Core/Physics/SurfaceResource.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <Foundation/Containers/Set.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <cstdio>
#include <cstdlib>

/// \file
/// Active ragdoll (RD-00 to RD-08). Regression checks guard what must not break; plan gates measure the exit thresholds of
/// the active ragdoll plan and print their numbers, so a miss is visible without failing the run.

using namespace JoltTest;

using plJoltTestRagdollHostManager = plComponentManager<class plJoltTestRagdollHost, plBlockStorageType::FreeList>;

/// Hands the test skeleton to the ragdoll and counts the ragdoll's event messages.
class plJoltTestRagdollHost : public plEventMessageHandlerComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltTestRagdollHost, plEventMessageHandlerComponent, plJoltTestRagdollHostManager);

public:
  void OnQuerySkeleton(plMsgQueryAnimationSkeleton& ref_msg)
  {
    if (!ref_msg.m_hSkeleton.IsValid())
      ref_msg.m_hSkeleton = m_hSkeleton;
  }

  void OnImpact(plMsgRagdollImpact& ref_msg) { ++m_uiImpacts; }
  void OnDislocated(plMsgRagdollJointDislocated& ref_msg) { ++m_uiDislocations; }
  void OnBalanceLost(plMsgRagdollBalanceLost& ref_msg) { ++m_uiBalanceLost; }

  plSkeletonResourceHandle m_hSkeleton;
  plUInt32 m_uiImpacts = 0;
  plUInt32 m_uiDislocations = 0;
  plUInt32 m_uiBalanceLost = 0;
};

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltTestRagdollHost, 1, plComponentMode::Static)
{
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgQueryAnimationSkeleton, OnQuerySkeleton),
    PL_MESSAGE_HANDLER(plMsgRagdollImpact, OnImpact),
    PL_MESSAGE_HANDLER(plMsgRagdollJointDislocated, OnDislocated),
    PL_MESSAGE_HANDLER(plMsgRagdollBalanceLost, OnBalanceLost),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

namespace
{
  //////////////////////////////////////////////////////////////////////////
  // Fixture: a 1.8 m humanoid made of boxes, Z up, facing +X, its right side on +Y (the engine's convention: forward is
  // right x up)

  struct JointDef
  {
    const char* m_szName;
    plInt32 m_iParent;
    plVec3 m_vPos;
    float m_fLength; ///< box along the bone; 0 = no body
    float m_fWidth;
    float m_fThickness;
    float m_fSwingY; ///< half angles, degrees
    float m_fSwingZ;
    float m_fTwist;
    plVec3 m_vCentre; ///< direction of the middle of the range; zero = the rest direction
    plVec3 m_vLeafDir; ///< bone direction of joints without a child
  };

  plVec3 Mirror(const plVec3& v) { return plVec3(v.x, -v.y, v.z); }

  /// A surface with the given friction; the physics material comes from the plugin's surface event handler.
  plSurfaceResourceHandle GetSurface(const char* szName, float fFriction)
  {
    plSurfaceResourceHandle hSurface = plResourceManager::GetExistingResource<plSurfaceResource>(szName);
    if (hSurface.IsValid())
      return hSurface;

    plSurfaceResourceDescriptor desc;
    desc.m_fPhysicsFrictionStatic = fFriction;
    desc.m_fPhysicsFrictionDynamic = fFriction;
    desc.m_fPhysicsRestitution = 0.1f;
    return plResourceManager::CreateResource<plSurfaceResource>(szName, std::move(desc));
  }

  /// With bToes off, the feet have no child joint, like many real characters; the foot then points forward on its own.
  plSkeletonResourceHandle CreateHumanoidSkeleton(bool bToes)
  {
    // skin against ground grips like a person on a slope, not like the 0.2 default material
    GetSurface("JoltTestSkin", 0.7f);

    const plVec3 vDown(0, 0, -1), vUp(0, 0, 1);
    const plVec3 vKnee = plVec3(-0.87f, 0, -0.5f).GetNormalized();  // shin bent 60° back
    const plVec3 vElbow = plVec3(0.94f, 0, -0.34f).GetNormalized(); // forearm bent 70° forward
    const plVec3 vHip = plVec3(0.42f, 0, -0.91f).GetNormalized();   // thigh 25° forward
    const plVec3 vShoulder = plVec3(0.34f, 0.26f, -0.9f).GetNormalized();

    plHybridArray<JointDef, 32> joints;
    auto Add = [&](const JointDef& j) { joints.PushBack(j); return (plInt32)joints.GetCount() - 1; };

    const plInt32 iPelvis = Add({"pelvis", -1, plVec3(0, 0, 1.0f), 0.15f, 0.3f, 0.18f, 0, 0, 0, plVec3::MakeZero(), vUp});
    const plInt32 iSpine = Add({"spine", iPelvis, plVec3(0, 0, 1.15f), 0.17f, 0.28f, 0.16f, 20, 20, 15, plVec3::MakeZero(), vUp});
    const plInt32 iChest = Add({"chest", iSpine, plVec3(0, 0, 1.32f), 0.2f, 0.34f, 0.2f, 20, 20, 15, plVec3::MakeZero(), vUp});
    const plInt32 iNeck = Add({"neck", iChest, plVec3(0, 0, 1.52f), 0.08f, 0.1f, 0.1f, 30, 30, 30, plVec3::MakeZero(), vUp});
    Add({"head", iNeck, plVec3(0, 0, 1.6f), 0.22f, 0.18f, 0.2f, 30, 20, 30, plVec3::MakeZero(), vUp});

    static const char* s_Names[2][7] = {{"thigh_l", "shin_l", "foot_l", "toes_l", "upperarm_l", "lowerarm_l", "hand_l"},
      {"thigh_r", "shin_r", "foot_r", "toes_r", "upperarm_r", "lowerarm_r", "hand_r"}};

    for (int iSide = 0; iSide < 2; ++iSide)
    {
      // the joint positions below are written for the right side (+Y); the left side is their mirror image
      auto P = [&](const plVec3& v) { return iSide == 0 ? Mirror(v) : v; };
      const char* const* n = s_Names[iSide];

      // with this frame setup, bending forward and back is a swing about the constraint's Z axis (the normal half angle,
      // the second value) and sideways bending a swing about Y (the first value)
      const plInt32 iThigh = Add({n[0], iPelvis, P(plVec3(0, 0.1f, 0.95f)), 0.43f, 0.13f, 0.13f, 40, 65, 30, P(vHip), vDown});
      const plInt32 iShin = Add({n[1], iThigh, P(plVec3(0, 0.1f, 0.52f)), 0.43f, 0.1f, 0.1f, 10, 65, 5, P(vKnee), vDown});
      const plInt32 iFoot = Add({n[2], iShin, P(plVec3(0, 0.1f, 0.09f)), 0.17f, 0.09f, 0.07f, 10, 25, 10, plVec3::MakeZero(), plVec3(0.92f, 0, -0.4f).GetNormalized()});
      if (bToes)
        Add({n[3], iFoot, P(plVec3(0.16f, 0.1f, 0.02f)), 0.0f, 0, 0, 0, 0, 0, plVec3::MakeZero(), plVec3(1, 0, 0)});
      const plInt32 iUpper = Add({n[4], iChest, P(plVec3(0, 0.21f, 1.47f)), 0.29f, 0.09f, 0.09f, 70, 70, 40, P(vShoulder), vDown});
      const plInt32 iLower = Add({n[5], iUpper, P(plVec3(0.02f, 0.23f, 1.18f)), 0.25f, 0.08f, 0.08f, 10, 75, 40, P(vElbow), vDown});
      Add({n[6], iLower, P(plVec3(0.04f, 0.23f, 0.93f)), 0.15f, 0.08f, 0.04f, 20, 40, 10, plVec3::MakeZero(), vDown});
    }

    // global rotations: the bone points along local +Y, towards the first child
    plHybridArray<plQuat, 32> globalRot;
    globalRot.SetCount(joints.GetCount());
    for (plUInt32 i = 0; i < joints.GetCount(); ++i)
    {
      plVec3 vDir = joints[i].m_vLeafDir;
      for (plUInt32 c = i + 1; c < joints.GetCount(); ++c)
      {
        if (joints[c].m_iParent == (plInt32)i)
        {
          vDir = joints[c].m_vPos - joints[i].m_vPos;
          break;
        }
      }
      globalRot[i] = plQuat::MakeShortestRotation(plVec3(0, 1, 0), vDir.GetNormalized());
    }

    plSkeletonBuilder builder;
    for (plUInt32 i = 0; i < joints.GetCount(); ++i)
    {
      const JointDef& j = joints[i];
      const plTransform tGlobal(j.m_vPos, globalRot[i]);
      plTransform tLocal = tGlobal;
      if (j.m_iParent >= 0)
        tLocal = plTransform::MakeLocalTransform(plTransform(joints[j.m_iParent].m_vPos, globalRot[j.m_iParent]), tGlobal);

      const plUInt16 uiIdx = builder.AddJoint(j.m_szName, tLocal, j.m_iParent >= 0 ? (plUInt16)j.m_iParent : plInvalidJointIndex);
      builder.SetJointSurface(uiIdx, "JoltTestSkin");

      if (j.m_iParent >= 0 && j.m_fLength > 0.0f)
      {
        // the middle of the range, in the parent's frame: the child's rest frame swung to the centre direction, so the rest
        // pose has no twist
        const plVec3 vRestDir = globalRot[i] * plVec3(0, 1, 0);
        const plVec3 vCentre = j.m_vCentre.IsZero() ? vRestDir : j.m_vCentre;
        const plQuat qCentreGlobal = plQuat::MakeShortestRotation(vRestDir, vCentre.GetNormalized()) * globalRot[i];
        const plQuat qOffset = globalRot[j.m_iParent].GetInverse() * qCentreGlobal;
        builder.SetJointLimit(uiIdx, qOffset, plSkeletonJointType::SwingTwist, plAngle::MakeFromDegree(j.m_fSwingY), plAngle::MakeFromDegree(j.m_fSwingZ), plAngle::MakeFromDegree(j.m_fTwist), plAngle::MakeZero(), 1.0f);
      }
    }

    plSkeletonResourceDescriptor desc;
    builder.BuildSkeleton(desc.m_Skeleton);

    // The bone direction stays at its default, +X: plSkeleton's move does not carry m_BoneDirection, so a skeleton created
    // from a descriptor always ends up with +X. The boxes are therefore placed explicitly along the bone (+Y). The ragdoll
    // offsets a box by half its length along X, so the position takes that back and moves it half a length along Y. The
    // rotation maps the box's length, width and thickness onto the joint's Y, Z and X: along the bone, sideways, forward.
    const plQuat qAlongBone = plQuat::MakeFromAxisAndAngle(plVec3(1, 1, 1).GetNormalized(), plAngle::MakeFromDegree(120.0f));
    for (plUInt32 i = 0; i < joints.GetCount(); ++i)
    {
      if (joints[i].m_fLength <= 0.0f)
        continue;

      const float fLength = joints[i].m_fLength;
      auto& geo = desc.m_Geometry.ExpandAndGetRef();
      geo.m_uiAttachedToJoint = (plUInt16)i;
      geo.m_Type = plSkeletonJointGeometryType::Box;
      geo.m_Transform = plTransform::MakeIdentity();
      geo.m_Transform.m_qRotation = qAlongBone;
      geo.m_Transform.m_vPosition.Set(-0.5f * fLength, 0.5f * fLength, 0.0f);
      geo.m_Transform.m_vScale.Set(fLength, joints[i].m_fWidth, joints[i].m_fThickness);
    }

    plHybridArray<plString, plHumanoidBone::COUNT> names;
    names.SetCount(plHumanoidBone::COUNT);
    names[plHumanoidBone::Hips] = "pelvis";
    names[plHumanoidBone::Spine] = "spine";
    names[plHumanoidBone::Chest] = "chest";
    names[plHumanoidBone::Neck] = "neck";
    names[plHumanoidBone::Head] = "head";
    names[plHumanoidBone::LeftUpperLeg] = "thigh_l";
    names[plHumanoidBone::LeftLowerLeg] = "shin_l";
    names[plHumanoidBone::LeftFoot] = "foot_l";
    names[plHumanoidBone::LeftToes] = bToes ? "toes_l" : "";
    names[plHumanoidBone::RightUpperLeg] = "thigh_r";
    names[plHumanoidBone::RightLowerLeg] = "shin_r";
    names[plHumanoidBone::RightFoot] = "foot_r";
    names[plHumanoidBone::RightToes] = bToes ? "toes_r" : "";
    names[plHumanoidBone::LeftUpperArm] = "upperarm_l";
    names[plHumanoidBone::LeftLowerArm] = "lowerarm_l";
    names[plHumanoidBone::LeftHand] = "hand_l";
    names[plHumanoidBone::RightUpperArm] = "upperarm_r";
    names[plHumanoidBone::RightLowerArm] = "lowerarm_r";
    names[plHumanoidBone::RightHand] = "hand_r";
    desc.m_HumanoidAvatar.BuildFromSkeleton(desc.m_Skeleton, names);

    return plResourceManager::CreateResource<plSkeletonResource>(bToes ? "JoltTestHumanoidSkeleton" : "JoltTestHumanoidSkeletonNoToes", std::move(desc));
  }

  plSkeletonResourceHandle GetSkeleton(bool bToes = true)
  {
    static plSkeletonResourceHandle s_hSkeleton = CreateHumanoidSkeleton(true);
    static plSkeletonResourceHandle s_hSkeletonNoToes = CreateHumanoidSkeleton(false);
    return bToes ? s_hSkeleton : s_hSkeletonNoToes;
  }

  plJoltRagdollProfileResourceHandle MakeProfile(const plJoltRagdollProfileDescriptor& desc)
  {
    static plUInt32 s_uiCounter = 0;
    plStringBuilder sId;
    sId.SetFormat("JoltTestRagdollProfile{}", s_uiCounter++);
    plJoltRagdollProfileDescriptor copy = desc;
    return plResourceManager::CreateResource<plJoltRagdollProfileResource>(sId, std::move(copy));
  }

  plJoltRagdollProfileDescriptor Preset(plJoltRagdollPreset::Enum preset)
  {
    plJoltRagdollProfileDescriptor d;
    d.ApplyPreset(preset);
    return d;
  }

  /// Today's ragdoll measured through the controller: no soft zone, no neutral pull, no damping.
  plJoltRagdollProfileDescriptor HardLimits()
  {
    plJoltRagdollProfileDescriptor d = Preset(plJoltRagdollPreset::Dead);
    d.m_fSoftZoneDeg = 0.0f;
    d.m_fLimpDampingFloor = 0.0f;
    for (auto& m : d.m_Muscles)
    {
      m.m_fNeutralPull = 0.0f;
      m.m_fDampingRatio = 0.0f;
    }
    return d;
  }

  struct Spawn
  {
    plVec3 m_vPosition = plVec3(0, 0, -0.02f); ///< the feet just touch the floor
    plQuat m_qRotation = plQuat::MakeIdentity();
    plJoltRagdollProfileResourceHandle m_hProfile;
    plUInt16 m_uiBehaviours = plJoltRagdollBehaviour::Default;
    float m_fConsciousness = 1.0f;
    plJoltRagdollAnimMode::Enum m_AnimMode = plJoltRagdollAnimMode::Limp;
    float m_fScale = 1.0f;
    float m_fGravityFactor = 1.0f;
    plVec3 m_vImpulse = plVec3::MakeZero();
    plVec3 m_vImpulseOffset = plVec3(0, 0, 1.3f); ///< relative to the object position
    bool m_bToes = true;                              ///< false: a skeleton whose feet have no toe joints
  };

  struct Ragdoll
  {
    plJoltRagdollComponent* m_pRagdoll = nullptr;
    plJoltTestRagdollHost* m_pHost = nullptr;
  };

  Ragdoll SpawnRagdoll(World& w, const Spawn& s)
  {
    plGameObject* pObject = w.CreateObject(s.m_vPosition, s.m_qRotation);

    PL_LOCK(w.GetWorld().GetWriteMarker());
    pObject->SetLocalScaling(plVec3(s.m_fScale));

    Ragdoll r;
    plJoltTestRagdollHost::CreateComponent(pObject, r.m_pHost);
    r.m_pHost->m_hSkeleton = GetSkeleton(s.m_bToes);

    plJoltRagdollComponent::CreateComponent(pObject, r.m_pRagdoll);
    r.m_pRagdoll->m_fWeightMass = 75.0f;
    r.m_pRagdoll->SetAnimMode(s.m_AnimMode);
    r.m_pRagdoll->SetGravityFactor(s.m_fGravityFactor);
    r.m_pRagdoll->SetProfile(s.m_hProfile);
    r.m_pRagdoll->SetConsciousness(s.m_fConsciousness);

    const char* szBehaviours[] = {"NeutralPose", "ImpactSpin", "FallPose", "ProtectFace", "RollGuard", "Injury", "Balance"};
    for (plUInt32 i = 0; i < 7; ++i)
      r.m_pRagdoll->SetBehaviourEnabled(szBehaviours[i], (s.m_uiBehaviours & PL_BIT(i)) != 0);

    if (!s.m_vImpulse.IsZero())
      r.m_pRagdoll->SetInitialImpulse(s.m_vPosition + s.m_vImpulseOffset, s.m_vImpulse);

    return r;
  }

  plJoltRagdollController::Metrics GetMetrics(World& w, const Ragdoll& r)
  {
    PL_LOCK(w.GetWorld().GetReadMarker());
    const plJoltRagdollController* pController = r.m_pRagdoll->GetController();
    return pController ? pController->ComputeMetrics() : plJoltRagdollController::Metrics();
  }

  const plJoltRagdollController* Controller(const Ragdoll& r) { return r.m_pRagdoll->GetController(); }

  /// A tumbling drop: random orientation around the vertical and a random tilt, a random push at chest height.
  Spawn TumbleDrop(plUInt32 uiSeed, float fHeight, const plJoltRagdollProfileResourceHandle& hProfile, plUInt16 uiBehaviours, float fConsciousness)
  {
    plRandom rng;
    rng.Initialize(uiSeed * 2654435761u + 17);

    Spawn s;
    s.m_vPosition = plVec3(0, 0, fHeight);
    const plQuat qYaw = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree((float)rng.DoubleMinMax(0, 360)));
    const plQuat qTilt = plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree((float)rng.DoubleMinMax(-70, 70)));
    s.m_qRotation = qYaw * qTilt;
    s.m_hProfile = hProfile;
    s.m_uiBehaviours = uiBehaviours;
    s.m_fConsciousness = fConsciousness;
    s.m_vImpulse = plVec3((float)rng.DoubleMinMax(-1, 1), (float)rng.DoubleMinMax(-1, 1), 0).GetNormalized() * (float)rng.DoubleMinMax(40, 90);
    return s;
  }

  float Mean(const plDynamicArray<float>& values)
  {
    float f = 0.0f;
    for (float v : values)
      f += v;
    return values.IsEmpty() ? 0.0f : f / values.GetCount();
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-00: fixes, step listener, frame-rate invariance

  void TestPlainRagdollStillWorks()
  {
    World w;
    w.AddFloor();
    Spawn s;
    s.m_vPosition.z = 1.5f;
    Ragdoll r = SpawnRagdoll(w, s);
    w.StepSeconds(4.0f);

    const plVec3 vPos = w.GetPosition(r.m_pRagdoll->GetOwner()->GetHandle());
    Check(vPos.IsValid() && vPos.z < 0.6f, "a ragdoll without a profile falls and lies on the floor");
    Check(r.m_pRagdoll->GetController() == nullptr, "no profile means no controller");
  }

  void TestControllerIsCreated()
  {
    World w;
    w.AddFloor();
    Spawn s;
    s.m_hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    Ragdoll r = SpawnRagdoll(w, s);
    w.Step(2);

    Check(Controller(r) != nullptr, "a profile creates the controller");
    Check(Controller(r) != nullptr && Controller(r)->IsHumanoid(), "the test skeleton's avatar is recognized as humanoid");
    Check(Controller(r) != nullptr && Controller(r)->GetRestForward().x > 0.9f, "the body faces the way the skeleton faces (+X)");

    // the body frame must not depend on toe joints: without them the poses would face backwards
    Spawn noToes;
    noToes.m_vPosition.y = 3.0f;
    noToes.m_hProfile = s.m_hProfile;
    noToes.m_bToes = false;
    Ragdoll r2 = SpawnRagdoll(w, noToes);
    w.Step(2);
    Check(Controller(r2) != nullptr && Controller(r2)->GetRestForward().x > 0.9f, "a skeleton without toes faces forward too");
  }

  void TestFrameRateInvariance()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    const float fRates[3] = {30.0f, 60.0f, 144.0f};
    float fHeight[3] = {}, fContortion[3] = {};

    for (int i = 0; i < 3; ++i)
    {
      World w(fRates[i]);
      w.AddFloor();
      Ragdoll r = SpawnRagdoll(w, TumbleDrop(7, 1.2f, hProfile, plJoltRagdollBehaviour::NeutralPose, 0.0f));
      w.StepSeconds(4.0f);
      const auto m = GetMetrics(w, r);
      fHeight[i] = m.m_vCom.z;
      fContortion[i] = m.m_fContortionDeg;
      Check(m.m_vCom.IsValid(), "the ragdoll stays finite at every frame rate");
    }

    Report("resting centre of mass height at 30 / 60 / 144 fps", fHeight[0]);
    Report("  60 fps", fHeight[1]);
    Report("  144 fps", fHeight[2]);
    Report("contortion at 30 / 60 / 144 fps", fContortion[0], " deg");
    Report("  60 fps", fContortion[1], " deg");
    Report("  144 fps", fContortion[2], " deg");

    const float fMax = plMath::Max(fHeight[0], fHeight[1], fHeight[2]);
    const float fMin = plMath::Min(fHeight[0], fHeight[1], fHeight[2]);
    Check(fMax - fMin <= 0.1f * fMax + 0.02f, "RD-00: the resting pose is the same at 30, 60 and 144 fps within 10%", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-01: muscles, damping, soft limits

  void TestSoftLimits()
  {
    const auto hHard = MakeProfile(HardLimits());
    const auto hSoft = MakeProfile(Preset(plJoltRagdollPreset::Dead));

    plUInt32 uiHard = 0, uiSoft = 0;
    plDynamicArray<float> settleHard, settleSoft;

    for (plUInt32 seed = 0; seed < 20; ++seed)
    {
      for (int v = 0; v < 2; ++v)
      {
        World w;
        w.AddFloor();
        Ragdoll r = SpawnRagdoll(w, TumbleDrop(seed, 1.6f, v == 0 ? hHard : hSoft, v == 0 ? 0 : plJoltRagdollBehaviour::NeutralPose, 0.0f));

        // landed: the torso touched and the pelvis has slowed down; settled: no joint turns faster than 0.5 rad/s any more
        float fLanded = -1.0f, fSettled = -1.0f;
        for (plUInt32 f = 0; f < 60 * 5; ++f)
        {
          w.Step(1);
          PL_LOCK(w.GetWorld().GetReadMarker());
          const auto* pC = Controller(r);
          if (pC == nullptr)
            continue;
          const auto m = pC->ComputeMetrics();
          if (fLanded < 0.0f && pC->HasTorsoContact() && m.m_vPelvisVelocity.GetLength() < 0.3f)
            fLanded = f / 60.0f;
          if (fLanded >= 0.0f)
          {
            if (m.m_fMaxJointSpeed >= 0.5f)
              fSettled = -1.0f;
            else if (fSettled < 0.0f)
              fSettled = f / 60.0f - fLanded;
          }
        }

        PL_LOCK(w.GetWorld().GetReadMarker());
        const plUInt32 uiBounces = Controller(r) ? Controller(r)->GetNumLimitBounces() : 0;
        (v == 0 ? uiHard : uiSoft) += uiBounces;
        (v == 0 ? settleHard : settleSoft).PushBack(fSettled < 0.0f ? 5.0f : fSettled);
      }
    }

    Report("limit bounces over 20 drops, hard limits", (float)uiHard);
    Report("limit bounces over 20 drops, soft limits (Dead preset)", (float)uiSoft);
    Report("mean time until the joints are still after landing, hard", Mean(settleHard), " s");
    Report("mean time until the joints are still after landing, soft", Mean(settleSoft), " s");

    Check(uiHard > 0, "the hard-limit baseline does bounce off its limits");
    Check(uiSoft <= uiHard / 5, "RD-01: limit bounces down at least 80% against hard limits", Category::PlanGate);
    Check(Mean(settleSoft) <= 0.5f, "RD-01: with the Dead preset, limbs come to rest within 0.5 s of landing", Category::PlanGate);
  }

  void TestScaleNeedsNoRetuning()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));

    for (float fScale : {0.5f, 2.0f})
    {
      World w;
      w.AddFloor();
      Spawn s = TumbleDrop(3, 1.5f * fScale, hProfile, plJoltRagdollBehaviour::Default, 0.5f);
      s.m_fScale = fScale;
      Ragdoll r = SpawnRagdoll(w, s);
      w.StepSeconds(5.0f);

      const auto m = GetMetrics(w, r);
      Check(m.m_vCom.IsValid() && m.m_fMaxSpeed < 0.5f, fScale < 1.0f ? "a 0.5x ragdoll settles with the same profile" : "a 2x ragdoll settles with the same profile");
    }
  }

  void TestStackedRagdolls()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Dead));
    World w;
    w.AddFloor();

    plHybridArray<Ragdoll, 4> ragdolls;
    for (plUInt32 i = 0; i < 3; ++i)
      ragdolls.PushBack(SpawnRagdoll(w, TumbleDrop(40 + i, 0.6f + i * 0.7f, hProfile, plJoltRagdollBehaviour::NeutralPose, 0.0f)));

    w.StepSeconds(6.0f);

    float fMaxSpeed = 0.0f;
    for (const Ragdoll& r : ragdolls)
      fMaxSpeed = plMath::Max(fMaxSpeed, GetMetrics(w, r).m_fMaxSpeed);

    Report("fastest body of three stacked ragdolls after 6 s", fMaxSpeed, " m/s");
    Check(fMaxSpeed < 0.1f, "RD-01: stacked ragdolls come to rest without jitter", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-02: neutral pose

  void TestNeutralPose()
  {
    // the same profile with and without the neutral pose, so only the neutral pull differs
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    const auto hFlat = hProfile;

    plDynamicArray<float> withNeutral, withoutNeutral;
    float fTrajectoryDiff = 0.0f;

    for (plUInt32 seed = 0; seed < 20; ++seed)
    {
      plVec3 vPelvisAtHalf[2];

      for (int v = 0; v < 2; ++v)
      {
        World w;
        w.AddFloor();
        Ragdoll r = SpawnRagdoll(w, TumbleDrop(100 + seed, 1.5f, v == 0 ? hProfile : hFlat, v == 0 ? plJoltRagdollBehaviour::NeutralPose : 0, 0.0f));
        w.StepSeconds(5.0f);
        (v == 0 ? withNeutral : withoutNeutral).PushBack(GetMetrics(w, r).m_fContortionDeg);

        // the first 0.5 s of a fall from 3 m, before anything touches the floor
        World w2;
        w2.AddFloor();
        Ragdoll r2 = SpawnRagdoll(w2, TumbleDrop(100 + seed, 3.0f, v == 0 ? hProfile : hFlat, v == 0 ? plJoltRagdollBehaviour::NeutralPose : 0, 0.0f));
        w2.StepSeconds(0.5f);
        vPelvisAtHalf[v] = GetMetrics(w2, r2).m_vCom;
      }

      fTrajectoryDiff = plMath::Max(fTrajectoryDiff, (vPelvisAtHalf[0] - vPelvisAtHalf[1]).GetLength());
    }

    const float fWith = Mean(withNeutral), fWithout = Mean(withoutNeutral);
    Report("mean contortion without neutral pose", fWithout, " deg");
    Report("mean contortion with neutral pose", fWith, " deg");
    Report("largest centre of mass difference 0.5 s into a fall from 3 m", fTrajectoryDiff, " m");

    Check(fWith <= 0.4f * fWithout, "RD-02: contortion down at least 60% with the neutral pose", Category::PlanGate);
    Check(fTrajectoryDiff < 0.05f, "RD-02: the neutral pose does not steer the first 0.5 s of the fall (< 5 cm)", Category::PlanGate);
  }

  void TestNeutralPoseInZeroGravity()
  {
    World w;
    Spawn s;
    s.m_vPosition = plVec3(0, 0, 3.0f);
    s.m_hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    s.m_uiBehaviours = plJoltRagdollBehaviour::NeutralPose;
    s.m_fConsciousness = 0.0f;
    s.m_fGravityFactor = 0.0f;
    Ragdoll r = SpawnRagdoll(w, s);

    w.Step(1);
    const float fStart = GetMetrics(w, r).m_fMaxNeutralErrorDeg;
    float fReached = -1.0f;

    for (plUInt32 f = 0; f < 60 * 4 && fReached < 0.0f; ++f)
    {
      w.Step(1);
      if (GetMetrics(w, r).m_fMaxNeutralErrorDeg < 10.0f)
        fReached = f / 60.0f;
    }

    if (fReached < 0.0f && std::getenv("JOLT_RAGDOLL_DEBUG"))
    {
      PL_LOCK(w.GetWorld().GetReadMarker());
      Controller(r)->DebugPrint();
    }

    Report("largest joint distance from the neutral pose at the start", fStart, " deg");
    Report("time until every joint is within 10 deg of the neutral pose", fReached, " s");
    Check(fStart > 20.0f, "the bind pose is far from the neutral pose");
    Check(fReached >= 0.0f && fReached <= 2.0f, "RD-02: in zero gravity the body reaches its neutral pose within 2 s", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-03: impact spin

  void TestImpactSpinKeepsMomentum()
  {
    const plVec3 vImpulse(100.0f, 0, 0);

    for (int v = 0; v < 2; ++v)
    {
      World w;
      Spawn s;
      s.m_vPosition = plVec3(0, 0, 5.0f);
      s.m_hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
      s.m_uiBehaviours = v == 0 ? plJoltRagdollBehaviour::ImpactSpin : 0;
      s.m_fConsciousness = 0.0f;
      s.m_fGravityFactor = 0.0f;
      s.m_vImpulse = vImpulse;
      Ragdoll r = SpawnRagdoll(w, s);
      w.Step(3);

      const auto m = GetMetrics(w, r);
      const float fError = (m.m_vMomentum - vImpulse).GetLength() / vImpulse.GetLength();
      Report(v == 0 ? "momentum error with impact spin" : "momentum error without impact spin", fError * 100.0f, " %");

      if (v == 0)
        Check(fError <= 0.02f, "RD-03: limb spin keeps the ragdoll's momentum within 2% of the hit", Category::PlanGate);
    }
  }

  void TestImpactSpinAsymmetry()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    plDynamicArray<float> asymmetry;

    for (plUInt32 seed = 0; seed < 30; ++seed)
    {
      World w;
      w.AddFloor();
      Spawn s;
      s.m_hProfile = hProfile;
      s.m_uiBehaviours = plJoltRagdollBehaviour::ImpactSpin;
      s.m_fConsciousness = 0.0f;
      s.m_vImpulse = plVec3(-120.0f, 0, 10.0f); // a blast from the front
      {
        PL_LOCK(w.GetWorld().GetWriteMarker());
        for (plUInt32 i = 0; i < seed; ++i)
          w.GetWorld().GetRandomNumberGenerator().UInt(); // a different limb seed per run
      }
      Ragdoll r = SpawnRagdoll(w, s);
      w.StepSeconds(0.2f);

      const auto m = GetMetrics(w, r);
      const float fMax = plMath::Max(m.m_fArmSpeed[0], m.m_fArmSpeed[1]);
      asymmetry.PushBack(fMax > 1e-3f ? plMath::Abs(m.m_fArmSpeed[0] - m.m_fArmSpeed[1]) / fMax : 0.0f);
    }

    Report("mean left/right arm speed difference 0.2 s after a blast", Mean(asymmetry) * 100.0f, " %");
    Check(Mean(asymmetry) >= 0.3f, "RD-03: the arms move differently after a blast (>= 30% on average)", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-04: fall pose and face protection

  void TestFaceProtection()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    plUInt32 uiBraced = 0;
    plDynamicArray<float> headConscious, headUnconscious, guards;
    bool bBraceAtZero = false;

    for (plUInt32 seed = 0; seed < 10; ++seed)
    {
      for (int v = 0; v < 2; ++v)
      {
        World w;
        w.AddFloor();
        Spawn s = TumbleDrop(200 + seed, 2.6f, hProfile, plJoltRagdollBehaviour::Default & ~plJoltRagdollBehaviour::Balance, v == 0 ? 1.0f : 0.0f);
        // pitch forward, so the fall is head first
        s.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(seed * 36.0f)) * plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(60.0f));
        Ragdoll r = SpawnRagdoll(w, s);

        float fMaxBrace = 0.0f;
        float fMaxGuard = 0.0f;
        bool bContact = false;
        for (plUInt32 f = 0; f < 60 * 3; ++f)
        {
          w.Step(1);
          PL_LOCK(w.GetWorld().GetReadMarker());
          const auto* pC = Controller(r);
          if (pC == nullptr)
            continue;
          if (!bContact)
          {
            fMaxBrace = plMath::Max(fMaxBrace, pC->GetBraceWeight());
            if (v == 0)
              fMaxGuard = plMath::Max(fMaxGuard, pC->GetGuardBlend());
            bContact = pC->HasTorsoContact();
          }
          if (v == 1 && pC->GetBraceWeight() > 0.0f)
            bBraceAtZero = true;
        }

        PL_LOCK(w.GetWorld().GetReadMarker());
        if (v == 0)
        {
          uiBraced += fMaxBrace > 0.7f ? 1 : 0;
          guards.PushBack(fMaxGuard);
          headConscious.PushBack(Controller(r)->GetPeakHeadImpulse());
        }
        else
        {
          headUnconscious.PushBack(Controller(r)->GetPeakHeadImpulse());
        }
      }
    }

    Report("falls where the brace pose was above 70% before the torso landed", (float)uiBraced, " of 10");
    Report("mean peak head impulse, consciousness 1", Mean(headConscious), " N s");
    Report("mean peak head impulse, consciousness 0", Mean(headUnconscious), " N s");

    Report("mean guard blend reached before the torso landed (2.6 m falls)", Mean(guards));
    Check(!bBraceAtZero, "no brace pose at consciousness 0");
    Check(Mean(guards) > 0.2f, "on falls from 2.6 m the arms come in towards the guard before landing");
    Check(uiBraced >= 8, "RD-04: brace pose above 70% before contact in at least 80% of falls over 1.5 m", Category::PlanGate);
    Check(Mean(headConscious) <= 0.5f * Mean(headUnconscious), "RD-04: peak head impulse down at least 50% at consciousness 1", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-05: roll prevention

  float MeasureRoll(bool bRollGuard, float fConsciousness, plUInt32 uiSeed)
  {
    World w;
    // a 20° slope that falls towards -Y, with ground friction (the default material's 0.2 would let anything slide)
    plGameObject* pSlope = w.CreateObject(plVec3(0, 0, -0.5f), plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree(20.0f)));
    {
      PL_LOCK(w.GetWorld().GetWriteMarker());
      plJoltStaticActorComponent* pActor = nullptr;
      plJoltStaticActorComponent::CreateComponent(pSlope, pActor);
      pActor->m_hSurface = GetSurface("JoltTestGround", 0.9f);
      plJoltShapeBoxComponent* pBox = nullptr;
      plJoltShapeBoxComponent::CreateComponent(pSlope, pBox);
      pBox->SetHalfExtents(plVec3(30.0f, 30.0f, 0.5f));
    }

    Spawn s;
    s.m_vPosition = plVec3(-0.8f, 0, 0.6f);
    // lying face down with the spine along +X, across the slope
    s.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree((float)uiSeed * 7.0f)) * plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(90.0f));
    s.m_hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    s.m_uiBehaviours = plJoltRagdollBehaviour::NeutralPose | (bRollGuard ? plJoltRagdollBehaviour::RollGuard : 0);
    s.m_fConsciousness = fConsciousness;
    // a kick high on the back: the body starts tumbling down the slope
    s.m_vImpulse = plVec3(0, -160.0f, 40.0f);
    s.m_vImpulseOffset = plVec3(0, 0, 0.35f);
    Ragdoll r = SpawnRagdoll(w, s);

    plVec3 vStart = plVec3::MakeZero();
    bool bStarted = false;
    const bool bTrace = std::getenv("JOLT_RAGDOLL_DEBUG") != nullptr && uiSeed == 0 && bRollGuard && fConsciousness > 0.5f;
    for (plUInt32 f = 0; f < 60 * 6; ++f)
    {
      w.Step(1);
      PL_LOCK(w.GetWorld().GetReadMarker());
      const auto* pC = Controller(r);
      if (pC && !bStarted && pC->HasTorsoContact())
      {
        bStarted = true;
        vStart = pC->ComputeMetrics().m_vPelvis;
      }

      if (bTrace && pC && f % 15 == 0)
      {
        const auto m = pC->ComputeMetrics();
        std::printf("    roll t %.2f pelvis (%.2f %.2f %.2f) rate %.2f rolling %d torso %d\n", f / 60.0f, m.m_vPelvis.x, m.m_vPelvis.y, m.m_vPelvis.z, pC->GetRollRate(), pC->IsRolling(), pC->HasTorsoContact());
      }
    }

    const plVec3 vEnd = GetMetrics(w, r).m_vPelvis;
    return bStarted ? (vEnd - vStart).GetLength() : 0.0f;
  }

  void TestRollGuard()
  {
    plDynamicArray<float> off1, on1, off0, on0;
    for (plUInt32 seed = 0; seed < 5; ++seed)
    {
      off1.PushBack(MeasureRoll(false, 1.0f, seed));
      on1.PushBack(MeasureRoll(true, 1.0f, seed));
      off0.PushBack(MeasureRoll(false, 0.0f, seed));
      on0.PushBack(MeasureRoll(true, 0.0f, seed));
    }

    Report("mean roll distance on a 20 deg slope, roll guard off", Mean(off1), " m");
    Report("mean roll distance, roll guard on, consciousness 1", Mean(on1), " m");
    Report("mean roll distance, roll guard on, consciousness 0", Mean(on0), " m");
    Report("  (roll guard off, consciousness 0)", Mean(off0), " m");

    Check(Mean(on1) <= 0.3f * Mean(off1), "RD-05: roll distance down at least 70% at consciousness 1", Category::PlanGate);
    Check(Mean(on0) <= 0.7f * Mean(off0), "RD-05: roll distance down at least 30% at consciousness 0", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-06: dislocation

  void TestInjury()
  {
    plJoltRagdollProfileDescriptor conscious = Preset(plJoltRagdollPreset::Conscious);
    conscious.m_bInjuryEnabled = true;
    const auto hConscious = MakeProfile(conscious);
    const auto hSlapstick = MakeProfile(Preset(plJoltRagdollPreset::Slapstick));

    plUInt32 uiStandard = 0;
    plDynamicArray<float> arrivals;
    for (plUInt32 seed = 0; seed < 20; ++seed)
    {
      World w;
      w.AddFloor();
      Ragdoll r = SpawnRagdoll(w, TumbleDrop(300 + seed, 1.5f, hConscious, (plJoltRagdollBehaviour::Default | plJoltRagdollBehaviour::Injury).GetValue(), 0.0f));
      w.StepSeconds(4.0f);
      PL_LOCK(w.GetWorld().GetReadMarker());
      uiStandard += r.m_pRagdoll->GetDislocatedJointCount();
      arrivals.PushBack(Controller(r)->GetMaxArrivalSpeedDeg());
    }

    arrivals.Sort();
    Report("fastest arrival at a joint limit in standard drops, median", arrivals[arrivals.GetCount() / 2], " deg/s");
    Report("fastest arrival at a joint limit in standard drops, max", arrivals.PeekBack(), " deg/s");

    plUInt32 uiRunsWithDislocation = 0;
    float fWorstLaunch = 0.0f;
    float fWorstEnvelope = 0.0f;
    bool bResetWorks = true;
    bool bMessages = true;

    for (plUInt32 seed = 0; seed < 10; ++seed)
    {
      World w;
      w.AddFloor();
      // stairs
      for (int i = 0; i < 6; ++i)
        w.AddStaticBox(plVec3(1.0f + i * 0.3f, 0, 0.09f + i * 0.18f), plVec3(0.15f, 2.0f, 0.09f + i * 0.18f));

      Spawn s = TumbleDrop(400 + seed, 4.0f, hSlapstick, (plJoltRagdollBehaviour::Default | plJoltRagdollBehaviour::Injury).GetValue(), 0.0f);
      s.m_vPosition.x = 1.5f;
      s.m_vImpulse *= 2.0f;
      Ragdoll r = SpawnRagdoll(w, s);

      plUInt32 uiLast = 0;
      float fSpeedBefore = 0.0f;
      plUInt32 uiWatch = 0;

      for (plUInt32 f = 0; f < 60 * 4; ++f)
      {
        w.Step(1);
        PL_LOCK(w.GetWorld().GetReadMarker());
        const plUInt32 uiNow = r.m_pRagdoll->GetDislocatedJointCount();
        const auto m = Controller(r) ? Controller(r)->ComputeMetrics() : plJoltRagdollController::Metrics();
        fWorstEnvelope = plMath::Max(fWorstEnvelope, m.m_fMaxEnvelopeExcessDeg);
        if (m.m_fMaxEnvelopeExcessDeg > 5.0f && std::getenv("JOLT_RAGDOLL_DEBUG"))
          std::printf("    envelope seed %u frame %u excess %.1f torso %d\n", seed, f, m.m_fMaxEnvelopeExcessDeg, Controller(r)->HasTorsoContact());

        // a launch is the pelvis gaining upward speed in the 0.1 s after the dislocation; gravity alone only takes speed away
        if (uiNow > uiLast)
        {
          uiWatch = 6;
          fSpeedBefore = m.m_vPelvisVelocity.z;
        }
        else if (uiWatch > 0)
        {
          --uiWatch;
          fWorstLaunch = plMath::Max(fWorstLaunch, m.m_vPelvisVelocity.z - fSpeedBefore);
        }
        uiLast = uiNow;
      }

      if (uiLast > 0)
      {
        ++uiRunsWithDislocation;
        w.Step(2);
        if (r.m_pHost->m_uiDislocations != uiLast)
          bMessages = false;

        PL_LOCK(w.GetWorld().GetWriteMarker());
        r.m_pRagdoll->ResetInjuries();
        bResetWorks = bResetWorks && r.m_pRagdoll->GetDislocatedJointCount() == 0;
      }
    }

    Report("dislocations in 20 standard drops at the default threshold", (float)uiStandard);
    Report("Slapstick runs on the stairs with a dislocation", (float)uiRunsWithDislocation, " of 10");
    Report("  (largest upward pelvis speed gain after a dislocation on the stairs, impacts included)", fWorstLaunch, " m/s");

    Report("  furthest any joint went past its hard envelope on the stairs", fWorstEnvelope, " deg");
    Check(fWorstEnvelope <= 5.0f, "dislocated or not, no joint goes more than 5 deg past its hard envelope on the stairs");
    Check(bResetWorks, "ResetInjuries() restores every joint");
    Check(bMessages, "every dislocation sends one message");
    Check(uiStandard == 0, "RD-06: no dislocations in standard drops at the default threshold", Category::PlanGate);
    Check(uiRunsWithDislocation >= 5, "RD-06: Slapstick dislocates a joint in at least half of the stair runs", Category::PlanGate);
  }

  /// A ragdoll lying still on the floor gets its limbs yanked until a joint dislocates. Nothing else moves the pelvis, so any
  /// upward speed it gains comes from the dislocation jolt.
  void TestDislocationDoesNotLaunch()
  {
    const auto hSlapstick = MakeProfile(Preset(plJoltRagdollPreset::Slapstick));
    float fWorstLaunch = 0.0f;
    float fWorstEnvelope = 0.0f;
    plUInt32 uiDislocations = 0;

    for (plUInt32 seed = 0; seed < 6; ++seed)
    {
      World w;
      w.AddFloor();
      Spawn s = TumbleDrop(600 + seed, 0.4f, hSlapstick, (plJoltRagdollBehaviour::Default | plJoltRagdollBehaviour::Injury).GetValue(), 0.0f);
      s.m_vImpulse.SetZero();
      Ragdoll r = SpawnRagdoll(w, s);
      w.StepSeconds(2.0f);

      for (plUInt32 attempt = 0; attempt < 8; ++attempt)
      {
        plUInt32 uiBefore = 0;
        float fBefore = 0.0f;
        {
          PL_LOCK(w.GetWorld().GetWriteMarker());
          uiBefore = r.m_pRagdoll->GetDislocatedJointCount();
          const auto m = Controller(r)->ComputeMetrics();
          fBefore = m.m_vPelvisVelocity.z;

          // yank whichever limb points that way, sideways along the floor
          const float fAngle = (seed * 8 + attempt) * 45.0f;
          const plVec3 vDir(plMath::Cos(plAngle::MakeFromDegree(fAngle)), plMath::Sin(plAngle::MakeFromDegree(fAngle)), 0);
          r.m_pRagdoll->ApplyImpulse(m.m_vPelvis + vDir * 2.0f, vDir.CrossRH(plVec3(0, 0, 1)) * 40.0f);
        }

        float fGain = 0.0f;
        bool bDislocated = false;
        for (plUInt32 f = 0; f < 6; ++f)
        {
          w.Step(1);
          PL_LOCK(w.GetWorld().GetReadMarker());
          bDislocated = bDislocated || r.m_pRagdoll->GetDislocatedJointCount() > uiBefore;
          const auto m = Controller(r)->ComputeMetrics();
          fGain = plMath::Max(fGain, m.m_vPelvisVelocity.z - fBefore);
          fWorstEnvelope = plMath::Max(fWorstEnvelope, m.m_fMaxEnvelopeExcessDeg);
        }

        if (bDislocated)
        {
          ++uiDislocations;
          fWorstLaunch = plMath::Max(fWorstLaunch, fGain);
        }

        w.StepSeconds(0.5f);
      }
    }

    Report("dislocations caused by yanking limbs of a lying ragdoll", (float)uiDislocations);
    Report("largest upward pelvis speed gain in the 0.1 s after a dislocation", fWorstLaunch, " m/s");
    Report("furthest any joint went past its hard envelope while yanked", fWorstEnvelope, " deg");
    Check(uiDislocations > 0, "yanking a Slapstick ragdoll's limbs dislocates joints");
    Check(fWorstEnvelope <= 5.0f, "a yanked, dislocated limb goes no more than 5 deg past its hard envelope");
    Check(fWorstLaunch < 0.5f, "RD-06: a dislocation never launches the body (pelvis speed gain < 0.5 m/s)", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-07: balance and takeover

  void TestStanding()
  {
    World w;
    w.AddFloor();
    Spawn s;
    s.m_hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    s.m_AnimMode = plJoltRagdollAnimMode::Active;
    Ragdoll r = SpawnRagdoll(w, s);

    float fMaxAssist = 0.0f;
    const bool bTrace = std::getenv("JOLT_RAGDOLL_DEBUG") != nullptr;
    for (plUInt32 f = 0; f < 60 * 10; ++f)
    {
      w.Step(1);
      PL_LOCK(w.GetWorld().GetReadMarker());
      if (Controller(r))
      {
        fMaxAssist = plMath::Max(fMaxAssist, Controller(r)->GetBalanceAssist());

        if (bTrace && (f < 12 || (f % 10 == 0 && f < 120)))
        {
          const auto m = Controller(r)->ComputeMetrics();
          std::printf("    t %.2f pelvis %.2f state %d assist %.2f torso %d behaviour %s\n", f / 60.0f, m.m_vPelvis.z, (int)Controller(r)->GetBalanceState(), Controller(r)->GetBalanceAssist(), Controller(r)->HasTorsoContact(), Controller(r)->GetActiveBehaviourName());
          Controller(r)->DebugPrint();
        }
      }
    }

    const auto m = GetMetrics(w, r);
    Report("pelvis height after standing for 10 s", m.m_vPelvis.z, " m");
    Report("largest balance assist", fMaxAssist * 100.0f, " % of m g h");

    PL_LOCK(w.GetWorld().GetReadMarker());
    Check(m.m_vPelvis.z > 0.85f && r.m_pRagdoll->GetBalanceState() == plJoltRagdollBalanceState::Standing, "RD-07: an Active ragdoll stands for 10 s", Category::PlanGate);
    Check(fMaxAssist <= 0.25f + 1e-3f, "RD-07: balance assist stays at or below 25% of m g h");
  }

  void TestPushRecovery(float fPush)
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    plUInt32 uiRecovered = 0, uiFell = 0, uiProtectedFalls = 0, uiMaxSteps = 0;
    const bool bGate = fPush >= 120.0f;

    for (plUInt32 seed = 0; seed < 10; ++seed)
    {
      World w;
      w.AddFloor();
      Spawn s;
      s.m_hProfile = hProfile;
      s.m_AnimMode = plJoltRagdollAnimMode::Active;
      Ragdoll r = SpawnRagdoll(w, s);
      w.StepSeconds(1.0f);

      const float fAngle = seed * 36.0f;
      const plVec3 vDir(plMath::Cos(plAngle::MakeFromDegree(fAngle)), plMath::Sin(plAngle::MakeFromDegree(fAngle)), 0);
      {
        PL_LOCK(w.GetWorld().GetWriteMarker());
        r.m_pRagdoll->ApplyImpulse(r.m_pRagdoll->GetOwner()->GetGlobalPosition() + plVec3(0, 0, 1.0f), vDir * fPush);
      }

      bool bProtectBeforeContact = false;
      bool bContact = false;
      const bool bTrace = std::getenv("JOLT_RAGDOLL_DEBUG") != nullptr && seed == 0 && !bGate;
      for (plUInt32 f = 0; f < 60 * 4; ++f)
      {
        w.Step(1);
        PL_LOCK(w.GetWorld().GetReadMarker());
        const auto* pC = Controller(r);
        if (bTrace && pC && f % 6 == 0 && f < 120)
        {
          std::printf("    push t %.2f behaviour %s brace %.2f\n", f / 60.0f, pC->GetActiveBehaviourName(), pC->GetBraceWeight());
          pC->DebugPrint();
        }
        if (pC && !bContact)
        {
          bContact = pC->HasTorsoContact();
          if (!bContact && (pC->GetBraceWeight() > 0.3f || pC->IsAirborne()))
            bProtectBeforeContact = true;
        }
      }

      PL_LOCK(w.GetWorld().GetReadMarker());
      const auto state = r.m_pRagdoll->GetBalanceState();
      const auto m = Controller(r)->ComputeMetrics();
      uiMaxSteps = plMath::Max(uiMaxSteps, Controller(r)->GetNumSteps());

      if ((state == plJoltRagdollBalanceState::Standing || state == plJoltRagdollBalanceState::Stepping) && m.m_vPelvis.z > 0.75f)
      {
        ++uiRecovered;
      }
      else
      {
        ++uiFell;
        uiProtectedFalls += bProtectBeforeContact ? 1 : 0;
      }
    }

    plStringBuilder sLabel;
    sLabel.SetFormat("pushes of {} N s recovered", (int)fPush);
    Report(sLabel, (float)uiRecovered, " of 10");
    Report("  most recovery steps in one run", (float)uiMaxSteps);
    Report("  falls with face protection before the torso landed", (float)uiProtectedFalls, " of falls");
    Report("  falls", (float)uiFell);

    Check(uiMaxSteps <= 2, "RD-07: at most two recovery steps");
    if (bGate)
    {
      Check(uiRecovered >= 7, "RD-07: rung 2 recovers from a 120 N s push in at least 70% of trials", Category::PlanGate);
      Check(uiProtectedFalls == uiFell, "RD-07: every failed recovery has protection active before the torso lands", Category::PlanGate);
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // RD-08: events and cost

  void TestEventsAndCost()
  {
    const auto hProfile = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    double fCost[3] = {};
    float fControl = 0.0f, fControlNeutralOnly = 0.0f;
    plUInt32 uiImpacts = 0, uiProjections = 0, uiCorrections = 0;

    for (int v = 0; v < 3; ++v)
    {
      World w;
      w.AddFloor();
      plHybridArray<Ragdoll, 8> ragdolls;
      for (plUInt32 i = 0; i < 8; ++i)
      {
        Spawn s = TumbleDrop(500 + i, 1.5f, v != 1 ? hProfile : plJoltRagdollProfileResourceHandle(), v == 2 ? (plUInt16)plJoltRagdollBehaviour::NeutralPose : (plUInt16)plJoltRagdollBehaviour::Default, 1.0f);
        s.m_vPosition.x = (float)i * 3.0f;
        ragdolls.PushBack(SpawnRagdoll(w, s));
      }

      w.Step(2);
      plJoltRagdollComponentManager* pMan = nullptr;
      {
        PL_LOCK(w.GetWorld().GetWriteMarker());
        pMan = w.GetWorld().GetOrCreateComponentManager<plJoltRagdollComponentManager>();
        pMan->ResetControlTime();
      }
      const plTime tStart = plTime::Now();
      w.StepSeconds(3.0f);
      fCost[v] = (plTime::Now() - tStart).GetMilliseconds() / (3.0 * 60.0);

      if (v == 0)
        fControl = (float)(pMan->GetControlTime().GetMilliseconds() / (3.0 * 60.0));
      if (v == 2)
        fControlNeutralOnly = (float)(pMan->GetControlTime().GetMilliseconds() / (3.0 * 60.0));

      if (v == 0)
      {
        PL_LOCK(w.GetWorld().GetReadMarker());
        for (const Ragdoll& r : ragdolls)
        {
          uiImpacts = plMath::Max(uiImpacts, r.m_pHost->m_uiImpacts);
          uiProjections += Controller(r) ? Controller(r)->GetNumEnvelopeProjections() : 0;
          uiCorrections += Controller(r) ? Controller(r)->GetNumEnvelopeCorrections() : 0;
        }
      }
    }

    Report("frame time, 8 ragdolls with all behaviours (physics included)", (float)fCost[0], " ms");
    Report("frame time, 8 plain ragdolls (physics included)", (float)fCost[1], " ms");
    Report("active ragdoll controllers, 8 ragdolls, physics step excluded", fControl, " ms per frame");
    Report("  the same with only the neutral pose (sensors, muscles, soft limits)", fControlNeutralOnly, " ms per frame");    Report("most impact messages from one ragdoll in 3 s", (float)uiImpacts);

    Report("  joints moved back onto their envelope, 8 ragdolls in 3 s", (float)uiProjections);
    Report("  steps with an envelope spin correction, 8 ragdolls in 3 s", (float)uiCorrections);
    Check(uiImpacts > 0, "landing ragdolls send impact messages");
    Check(uiImpacts <= 90, "impact messages stay within 30 per second per ragdoll");
    Check(fControl < 0.6f, "RD-08: 8 active ragdolls with all behaviours cost under 0.6 ms per frame, physics step excluded", Category::PlanGate);
  }

  //////////////////////////////////////////////////////////////////////////
  // Recordings: each scenario panel is one text file with every body's triangles (once) and transform (every frame)
  //
  //   L <label>                                  panel caption
  //   R <frames per second>                      how often a frame was recorded
  //   E <name> <values...>                       an event, e.g. a shot; belongs to the frame that follows
  //   B <id> <static> <group> <triangles>        a body; static bodies are in world space, dynamic ones relative to their centre of mass
  //   T x0 y0 z0 x1 y1 z1 x2 y2 z2               one triangle of the body above
  //   F <frame>                                  starts a frame
  //   P <id> px py pz qx qy qz qw                a dynamic body's centre of mass and rotation
  //   H <ragdoll> <behaviour>|<brace>|<balance state>|<consciousness>|<dislocations>|<guard blend>

  class Recorder
  {
  public:
    Recorder(World& ref_world, const char* szFolder, const char* szFile, const char* szLabel, float fFrameRate)
      : m_World(ref_world)
    {
      plStringBuilder sPath(szFolder, "/", szFile, ".txt");
      m_pFile = std::fopen(sPath, "w");
      if (m_pFile)
        std::fprintf(m_pFile, "L %s\nR %g\n", szLabel, fFrameRate);
    }

    /// Writes an event line; it belongs to the frame recorded next.
    void Event(const char* szName, const plVec3& a, const plVec3& b)
    {
      if (m_pFile)
        std::fprintf(m_pFile, "E %s %.4f %.4f %.4f %.4f %.4f %.4f\n", szName, a.x, a.y, a.z, b.x, b.y, b.z);
    }

    ~Recorder()
    {
      if (m_pFile)
        std::fclose(m_pFile);
    }

    void AddRagdoll(const Ragdoll& r) { m_Ragdolls.PushBack(r); }

    /// Call after each world step.
    void Frame()
    {
      if (m_pFile == nullptr)
        return;

      PL_LOCK(m_World.GetWorld().GetReadMarker());
      JPH::PhysicsSystem* pSystem = m_World.GetJolt().GetJoltSystem();
      JPH::BodyIDVector ids;
      pSystem->GetBodies(ids);

      std::fprintf(m_pFile, "F %u\n", m_uiFrame++);

      for (const JPH::BodyID& id : ids)
      {
        JPH::BodyLockRead lock(pSystem->GetBodyLockInterfaceNoLock(), id);
        if (!lock.Succeeded())
          continue;

        const JPH::Body& body = lock.GetBody();
        const plUInt32 uiId = id.GetIndexAndSequenceNumber();
        if (!m_Known.Contains(uiId))
        {
          m_Known.Insert(uiId);
          WriteBody(uiId, body);
        }

        if (body.IsStatic())
          continue;

        const JPH::Vec3 vPos = JPH::Vec3(body.GetCenterOfMassPosition());
        const JPH::Quat qRot = body.GetRotation();
        std::fprintf(m_pFile, "P %u %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", uiId, vPos.GetX(), vPos.GetY(), vPos.GetZ(), qRot.GetX(), qRot.GetY(), qRot.GetZ(), qRot.GetW());
      }

      for (plUInt32 i = 0; i < m_Ragdolls.GetCount(); ++i)
      {
        const Ragdoll& r = m_Ragdolls[i];
        const plJoltRagdollController* pC = r.m_pRagdoll->GetController();
        std::fprintf(m_pFile, "H %u %s|%.2f|%d|%.2f|%u|%.2f\n", i, pC ? pC->GetActiveBehaviourName() : "none", pC ? pC->GetBraceWeight() : 0.0f, pC ? (int)pC->GetBalanceState() : -1, r.m_pRagdoll->GetConsciousness(), r.m_pRagdoll->GetDislocatedJointCount(), pC ? pC->GetGuardBlend() : 0.0f);
      }
    }

  private:
    void WriteBody(plUInt32 uiId, const JPH::Body& body)
    {
      const JPH::Shape* pShape = body.GetShape();
      const bool bStatic = body.IsStatic();

      // the floor is drawn as a grid
      if (bStatic && pShape->GetLocalBounds().GetSize().ReduceMax() > 20.0f)
      {
        std::fprintf(m_pFile, "B %u 1 0 0\n", uiId);
        return;
      }

      const JPH::Vec3 vPos = bStatic ? JPH::Vec3(body.GetCenterOfMassPosition()) : JPH::Vec3::sZero();
      const JPH::Quat qRot = bStatic ? body.GetRotation() : JPH::Quat::sIdentity();

      // compound and offset shapes only give triangles through their leaves
      JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
      pShape->CollectTransformedShapes(JPH::AABox::sBiggest(), vPos, qRot, JPH::Vec3::sReplicate(1.0f), JPH::SubShapeIDCreator(), leaves, JPH::ShapeFilter());

      plDynamicArray<float> coords; // x y z per vertex
      for (const JPH::TransformedShape& leaf : leaves.mHits)
      {
        JPH::Shape::GetTrianglesContext context;
        leaf.GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::RVec3::sZero());

        JPH::Float3 batch[3 * JPH::Shape::cGetTrianglesMinTrianglesRequested];
        while (true)
        {
          const int iCount = leaf.GetTrianglesNext(context, JPH::Shape::cGetTrianglesMinTrianglesRequested, batch);
          if (iCount <= 0)
            break;
          for (int i = 0; i < iCount * 3; ++i)
          {
            coords.PushBack(batch[i].x);
            coords.PushBack(batch[i].y);
            coords.PushBack(batch[i].z);
          }
        }
      }

      std::fprintf(m_pFile, "B %u %d %u %u\n", uiId, bStatic ? 1 : 0, body.GetCollisionGroup().GetGroupID(), coords.GetCount() / 9);
      for (plUInt32 t = 0; t + 8 < coords.GetCount(); t += 9)
      {
        const float* v = &coords[t];
        std::fprintf(m_pFile, "T %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
      }
    }

    World& m_World;
    FILE* m_pFile = nullptr;
    plHybridArray<Ragdoll, 4> m_Ragdolls;
    plSet<plUInt32> m_Known;
    plUInt32 m_uiFrame = 0;
  };

  /// Only panels whose file name starts with this are recorded; empty records all.
  plStringBuilder s_sRecordFilter;

  /// Simulates one panel at the given frame rate: setup() spawns into the world and returns the ragdolls,
  /// onFrame(world, ragdolls, frame, recorder) runs before each step.
  template <typename SETUP, typename ONFRAME>
  void RecordPanelAt(const char* szFolder, const char* szFile, const char* szLabel, float fSeconds, float fFrameRate, SETUP setup, ONFRAME onFrame)
  {
    if (!s_sRecordFilter.IsEmpty() && !plStringView(szFile).StartsWith(s_sRecordFilter))
      return;

    World w(fFrameRate);
    w.AddFloor();
    Recorder rec(w, szFolder, szFile, szLabel, fFrameRate);
    plHybridArray<Ragdoll, 4> ragdolls;
    setup(w, ragdolls);
    for (const Ragdoll& r : ragdolls)
      rec.AddRagdoll(r);

    const plUInt32 uiFrames = (plUInt32)(fSeconds * fFrameRate);
    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      onFrame(w, ragdolls, f, rec);
      w.Step(1);
      rec.Frame();
    }

    std::printf("  recorded %s (%u frames at %g Hz)\n", szFile, uiFrames, fFrameRate);
  }

  template <typename SETUP, typename ONFRAME>
  void RecordPanel(const char* szFolder, const char* szFile, const char* szLabel, float fSeconds, SETUP setup, ONFRAME onFrame)
  {
    RecordPanelAt(szFolder, szFile, szLabel, fSeconds, 60.0f, setup, onFrame);
  }

  template <typename SETUP>
  void RecordPanel(const char* szFolder, const char* szFile, const char* szLabel, float fSeconds, SETUP setup)
  {
    RecordPanelAt(szFolder, szFile, szLabel, fSeconds, 60.0f, setup, [](World&, plHybridArray<Ragdoll, 4>&, plUInt32, Recorder&) {});
  }

  void RecordAll(const char* szFolder)
  {
    const auto hConscious = MakeProfile(Preset(plJoltRagdollPreset::Conscious));
    const auto hDead = MakeProfile(Preset(plJoltRagdollPreset::Dead));
    const auto hSlapstick = MakeProfile(Preset(plJoltRagdollPreset::Slapstick));
    plJoltRagdollProfileDescriptor consciousInjury = Preset(plJoltRagdollPreset::Conscious);
    consciousInjury.m_bInjuryEnabled = true;
    const auto hConsciousInjury = MakeProfile(consciousInjury);
    const plUInt16 uiNoBalance = plJoltRagdollBehaviour::Default & ~plJoltRagdollBehaviour::Balance;
    const plUInt16 uiInjury = (plJoltRagdollBehaviour::Default | plJoltRagdollBehaviour::Injury).GetValue();

    // 1: the same tumbling drop with today's ragdoll, the Dead preset and the Conscious preset
    {
      const plJoltRagdollProfileResourceHandle profiles[3] = {plJoltRagdollProfileResourceHandle(), hDead, hConscious};
      const plUInt16 behaviours[3] = {0, plJoltRagdollBehaviour::NeutralPose, uiNoBalance};
      const float consciousness[3] = {0.0f, 0.0f, 1.0f};
      const char* files[3] = {"drop_0", "drop_1", "drop_2"};
      const char* labels[3] = {"No profile", "Dead preset", "Conscious preset"};
      for (int v = 0; v < 3; ++v)
      {
        RecordPanel(szFolder, files[v], labels[v], 5.0f, [&](World& w, plHybridArray<Ragdoll, 4>& out) { out.PushBack(SpawnRagdoll(w, TumbleDrop(3, 2.2f, profiles[v], behaviours[v], consciousness[v]))); });
      }
    }

    // 2: zero gravity, from the bind pose
    for (int v = 0; v < 2; ++v)
    {
      RecordPanel(szFolder, v == 0 ? "zerog_0" : "zerog_1", v == 0 ? "No profile" : "Neutral pose", 3.0f, [&](World& w, plHybridArray<Ragdoll, 4>& out) {
        Spawn s;
        s.m_vPosition = plVec3(0, 0, 0.6f);
        s.m_hProfile = v == 0 ? plJoltRagdollProfileResourceHandle() : hConscious;
        s.m_uiBehaviours = plJoltRagdollBehaviour::NeutralPose;
        s.m_fConsciousness = 0.0f;
        s.m_fGravityFactor = 0.0f;
        out.PushBack(SpawnRagdoll(w, s));
      });
    }

    // 3: a head-first fall, unconscious and conscious
    for (int v = 0; v < 2; ++v)
    {
      RecordPanel(szFolder, v == 0 ? "face_0" : "face_1", v == 0 ? "Consciousness 0" : "Consciousness 1", 3.5f, [&](World& w, plHybridArray<Ragdoll, 4>& out) {
        Spawn s = TumbleDrop(200, 2.6f, hConscious, uiNoBalance, v == 0 ? 0.0f : 1.0f);
        s.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(60.0f));
        out.PushBack(SpawnRagdoll(w, s));
      });
    }

    // 4: a blast from the front
    for (int v = 0; v < 2; ++v)
    {
      RecordPanel(szFolder, v == 0 ? "blast_0" : "blast_1", v == 0 ? "Impact spin off" : "Impact spin on", 3.0f, [&](World& w, plHybridArray<Ragdoll, 4>& out) {
        Spawn s;
        s.m_hProfile = hConscious;
        s.m_uiBehaviours = (plUInt16)(plJoltRagdollBehaviour::NeutralPose | (v == 1 ? plJoltRagdollBehaviour::ImpactSpin : 0));
        s.m_fConsciousness = 0.0f;
        s.m_vImpulse = plVec3(-120.0f, 0, 10.0f);
        out.PushBack(SpawnRagdoll(w, s));
      });
    }

    // 5: down the stairs with injury enabled
    for (int v = 0; v < 2; ++v)
    {
      RecordPanel(szFolder, v == 0 ? "stairs_0" : "stairs_1", v == 0 ? "Conscious, injury on" : "Slapstick", 4.0f, [&](World& w, plHybridArray<Ragdoll, 4>& out) {
        for (int i = 0; i < 6; ++i)
          w.AddStaticBox(plVec3(1.0f + i * 0.3f, 0, 0.09f + i * 0.18f), plVec3(0.15f, 2.0f, 0.09f + i * 0.18f));
        Spawn s = TumbleDrop(402, 4.0f, v == 0 ? hConsciousInjury : hSlapstick, uiInjury, 0.0f);
        s.m_vPosition.x = 1.5f;
        s.m_vImpulse *= 2.0f;
        out.PushBack(SpawnRagdoll(w, s));
      });
    }

    // 6: Active mode standing, then pushed in the back at 1.5 s
    {
      const float pushes[3] = {0.0f, 60.0f, 120.0f};
      const char* files[3] = {"push_0", "push_1", "push_2"};
      const char* labels[3] = {"Standing", "Push 60 N s", "Push 120 N s"};
      for (int v = 0; v < 3; ++v)
      {
        RecordPanel(
          szFolder, files[v], labels[v], 5.0f,
          [&](World& w, plHybridArray<Ragdoll, 4>& out) {
            Spawn s;
            s.m_hProfile = hConscious;
            s.m_AnimMode = plJoltRagdollAnimMode::Active;
            out.PushBack(SpawnRagdoll(w, s));
          },
          [&](World& w, plHybridArray<Ragdoll, 4>& ragdolls, plUInt32 uiFrame, Recorder&) {
            if (uiFrame != 90 || pushes[v] <= 0.0f)
              return;
            PL_LOCK(w.GetWorld().GetWriteMarker());
            plJoltRagdollComponent* pRagdoll = ragdolls[0].m_pRagdoll;
            pRagdoll->ApplyImpulse(pRagdoll->GetOwner()->GetGlobalPosition() + plVec3(0, 0, 1.2f), plVec3(pushes[v], 0, 0));
          });
      }
    }

    // 8: everything together. A character stands on a landing with a flight of stairs behind it. A shot in the chest
    // knocks it back: it stumbles backwards, steps off the edge, falls down the stairs protecting its head, and is knocked
    // out at the bottom. Recorded at 120 Hz for slow motion.
    {
      plJoltRagdollProfileDescriptor stunt = Preset(plJoltRagdollPreset::Conscious);
      stunt.m_bInjuryEnabled = true;
      const auto hStunt = MakeProfile(stunt);

      auto EnvFloat = [](const char* szName, float fDefault) { const char* sz = std::getenv(szName); return sz ? (float)std::atof(sz) : fDefault; };
      const float fShot = EnvFloat("JOLT_DEMO_SHOT", 55.0f);       // N s
      const float fFromEdge = EnvFloat("JOLT_DEMO_EDGE", 0.85f);   // m between the character and the top step
      const float fShotTime = 1.2f;
      const float fRate = 120.0f;

      // a standard flight: 0.18 m rise, 0.28 m tread, nine steps down from a 1.8 m landing that ends at x = 0
      constexpr float fRise = 0.18f, fTread = 0.28f, fLanding = 1.8f, fHalfWidth = 1.4f;

      RecordPanelAt(
        szFolder, "demo_0", "Shot, stumble, stairs", 8.0f, fRate,
        [&](World& w, plHybridArray<Ragdoll, 4>& out) {
          w.AddStaticBox(plVec3(-1.75f, 0, 0.5f * fLanding), plVec3(1.75f, fHalfWidth, 0.5f * fLanding));
          for (int i = 0; i < 9; ++i)
          {
            const float fTop = fLanding - fRise * (i + 1);
            w.AddStaticBox(plVec3(fTread * (i + 0.5f), 0, 0.5f * fTop), plVec3(0.5f * fTread, fHalfWidth, 0.5f * fTop));
          }
          // a wall along the far side of the stairs
          w.AddStaticBox(plVec3(0.5f, fHalfWidth + 0.1f, 1.75f), plVec3(4.0f, 0.1f, 1.75f));

          Spawn s;
          s.m_vPosition = plVec3(-fFromEdge, 0, fLanding - 0.02f);
          s.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(180.0f)); // facing -X, the stairs behind it
          s.m_hProfile = hStunt;
          s.m_uiBehaviours = (plJoltRagdollBehaviour::Default | plJoltRagdollBehaviour::Injury).GetValue();
          s.m_AnimMode = plJoltRagdollAnimMode::Active;
          out.PushBack(SpawnRagdoll(w, s));
        },
        [&, bKnockedOut = false](World& w, plHybridArray<Ragdoll, 4>& ragdolls, plUInt32 uiFrame, Recorder& rec) mutable {
          plJoltRagdollComponent* pRagdoll = ragdolls[0].m_pRagdoll;
          PL_LOCK(w.GetWorld().GetWriteMarker());

          if (uiFrame == (plUInt32)(fShotTime * fRate))
          {
            // from a shooter standing on the landing, a little to the character's right, into the front of the chest. In
            // Active mode the owner follows the ragdoll, so the chest is found from the pelvis.
            const plVec3 vPelvis = pRagdoll->GetController()->ComputeMetrics().m_vPelvis;
            const plVec3 vHit = vPelvis + plVec3(-0.1f, -0.06f, 0.4f);
            const plVec3 vMuzzle(-7.0f, -0.9f, fLanding + 1.45f);
            const plVec3 vDir = (vHit - vMuzzle).GetNormalized();
            pRagdoll->ApplyImpulse(vHit, vDir * fShot);
            pRagdoll->FadeConsciousness(0.6f, plTime::MakeFromSeconds(0.4));
            rec.Event("shot", vMuzzle, vHit);
          }

          if (std::getenv("JOLT_RAGDOLL_DEBUG") && uiFrame >= (plUInt32)(fShotTime * fRate) && uiFrame < (plUInt32)((fShotTime + 2.5f) * fRate) && uiFrame % 6 == 0)
          {
            const auto* pC = pRagdoll->GetController();
            std::printf("  demo t %.2f %s stagger %.2f catch %.2f impact %d brake %.2f brace %.2f guard %.2f back %.2f\n", uiFrame / fRate - fShotTime, pC->GetActiveBehaviourName(), pC->GetStaggerBlend(), pC->GetCatchBlend(), pC->HadFirstImpact(),
              pC->GetBrakeBlend(), pC->GetBraceWeight(), pC->GetGuardBlend(), pC->GetBackGuardBlend());
            pC->DebugPrint();
          }

          // knocked out once it has come to rest at the bottom
          if (!bKnockedOut && uiFrame > (plUInt32)((fShotTime + 1.0f) * fRate) && pRagdoll->GetController() != nullptr)
          {
            const auto m = pRagdoll->GetController()->ComputeMetrics();
            if (m.m_vPelvis.z < 0.5f && m.m_vPelvisVelocity.GetLength() < 0.6f)
            {
              pRagdoll->FadeConsciousness(0.0f, plTime::MakeFromSeconds(1.2));
              bKnockedOut = true;
            }
          }
        });
    }

    // 7: three Dead ragdolls dropped on each other
    RecordPanel(szFolder, "pile_0", "Dead preset, stacked", 6.0f, [&](World& w, plHybridArray<Ragdoll, 4>& out) {
      for (plUInt32 i = 0; i < 3; ++i)
        out.PushBack(SpawnRagdoll(w, TumbleDrop(40 + i, 0.6f + i * 0.9f, hDead, plJoltRagdollBehaviour::NeutralPose, 0.0f)));
    });
  }
} // namespace

void RunRagdollRecordings(const char* szFolder, const char* szFilter)
{
  s_sRecordFilter = szFilter ? szFilter : "";
  std::printf("recording ragdoll scenarios to %s\n", szFolder);

  // this executable links its own copy of Jolt for the inline code; only the plugin's copy has its allocator set
  if (JPH::Allocate == nullptr)
    JPH::RegisterDefaultAllocator();

  RecordAll(szFolder);
}

void RunRagdollTests()
{
  BeginGroup("Ragdoll");

  TestPlainRagdollStillWorks();
  TestControllerIsCreated();
  TestFrameRateInvariance();
  TestSoftLimits();
  TestScaleNeedsNoRetuning();
  TestStackedRagdolls();
  TestNeutralPose();
  TestNeutralPoseInZeroGravity();
  TestImpactSpinKeepsMomentum();
  TestImpactSpinAsymmetry();
  TestFaceProtection();
  TestRollGuard();
  TestInjury();
  TestDislocationDoesNotLaunch();
  TestStanding();
  TestPushRecovery(60.0f);
  TestPushRecovery(120.0f);
  TestEventsAndCost();
}
