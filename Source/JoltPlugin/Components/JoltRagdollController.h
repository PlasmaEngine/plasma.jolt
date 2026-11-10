#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Random.h>
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <JoltPlugin/Components/JoltRagdollMessages.h>
#include <JoltPlugin/Resources/JoltRagdollProfileResource.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <RendererCore/AnimationSystem/HumanoidBones.h>
#include <atomic>

struct plSkeletonResourceDescriptor;
class plDebugRendererContext;

namespace JPH
{
  class Body;
  class PhysicsSystem;
  class Ragdoll;
  class SkeletonPose;
  class SwingTwistConstraint;
  class TwoBodyConstraint;
} // namespace JPH

/// \brief The behaviours of an active ragdoll. Each one can be switched off on the component.
struct plJoltRagdollBehaviour
{
  using StorageType = plUInt16;

  enum Enum
  {
    NeutralPose = PL_BIT(0),
    ImpactSpin = PL_BIT(1),
    FallPose = PL_BIT(2),
    ProtectFace = PL_BIT(3),
    RollGuard = PL_BIT(4),
    Injury = PL_BIT(5),
    Balance = PL_BIT(6),

    Default = NeutralPose | ImpactSpin | FallPose | ProtectFace | RollGuard | Balance
  };

  struct Bits
  {
    StorageType NeutralPose : 1;
    StorageType ImpactSpin : 1;
    StorageType FallPose : 1;
    StorageType ProtectFace : 1;
    StorageType RollGuard : 1;
    StorageType Injury : 1;
    StorageType Balance : 1;
  };

  /// \brief Maps the names used by SetBehaviourEnabled() to flags. Returns 0 for unknown names.
  static StorageType FromName(plStringView sName);
};

PL_DECLARE_FLAGS_OPERATORS(plJoltRagdollBehaviour);

/// \brief Per-ragdoll runtime of the active ragdoll: body map, muscles, joint limits, sensors, behaviours and the target arbiter.
///
/// The controller never creates or removes Jolt objects during a step. It is driven in three places:
/// Prepare() on the main thread before the simulate task starts, Step() from the world's PhysicsStepListener once per
/// collision step, and PostStep() on the main thread after the task has been joined.
class PL_JOLTPLUGIN_DLL plJoltRagdollController
{
public:
  /// \brief Values the component hands over before every simulate task. The step only reads this copy.
  struct Inputs
  {
    float m_fConsciousness = 1.0f;
    float m_fStrengthScale = 1.0f; ///< SetJointMotorStrength() / 100
    plBitflags<plJoltRagdollBehaviour> m_Behaviours = plJoltRagdollBehaviour::Default;
    bool m_bAnimationDriven = false; ///< Powered or Active anim mode: the animation pose is a target
    bool m_bActiveMode = false;      ///< Active anim mode: balance corrects the animation, hits take the body over
    bool m_bSuspended = false;       ///< Controlled (kinematic) mode: the controller stays out of it
    float m_fFrameTime = 1.0f / 60.0f;
  };

  plJoltRagdollController();
  ~plJoltRagdollController();

  /// \brief Builds the body map and all per-part data. Returns false if the ragdoll cannot be driven (no constraints).
  bool Init(JPH::Ragdoll* pRagdoll, JPH::PhysicsSystem* pSystem, const plSkeletonResourceDescriptor& skeleton, plArrayPtr<const plUInt16> partToJoint, plUInt32 uiCollisionGroup, plUInt32 uiSeed);

  /// \brief Removes dislocation constraints. Must be called on the main thread before the ragdoll is removed.
  void Shutdown();

  void SetProfile(const plJoltRagdollProfileDescriptor& profile);

  /// \brief Main thread, before the simulate task. Copies the inputs and the newest animation targets (may be null).
  void Prepare(const Inputs& inputs, const JPH::SkeletonPose* pAnimPose);

  /// \brief Records a hit. Applied as limb spin at the next step, and in Active mode it starts the takeover.
  void AddHit(const plVec3& vImpulse, const plVec3& vPosition);

  /// \brief Called from the step listener before every collision step.
  void Step(float fDeltaTime);

  /// \brief Called by the contact listener, from any job thread, when one of this ragdoll's bodies touches something else.
  void ReportContact(const JPH::BodyID& bodyId) const;

  /// \brief Main thread, after the simulate task. Applies dislocations and hands out the events of the last steps.
  void PostStep(plDynamicArray<plMsgRagdollImpact>& out_impacts, plDynamicArray<plMsgRagdollJointDislocated>& out_dislocations, plDynamicArray<plMsgRagdollBalanceLost>& out_balanceLost, float& out_fConsciousnessDrop);

  /// \brief Restores every dislocated joint. Main thread only.
  void ResetInjuries();

  void DebugDraw(const plDebugRendererContext& context, bool bTimeline) const;

  plJoltRagdollBalanceState::Enum GetBalanceState() const { return m_BalanceState; }
  bool IsHumanoid() const { return m_bHumanoid; }
  const char* GetActiveBehaviourName() const;

  /// \brief The plan's validation metrics, read on the main thread while no step runs.
  struct Metrics
  {
    float m_fContortionDeg = 0.0f;      ///< degrees past 80% of each joint limit, summed over joints
    float m_fMaxNeutralErrorDeg = 0.0f; ///< largest angle between a joint and its neutral pose target
    plVec3 m_vMomentum = plVec3::MakeZero();
    float m_fArmSpeed[2] = {};          ///< upper arm angular speed relative to its parent, left and right
    float m_fLegSpeed[2] = {};          ///< thigh angular speed relative to the pelvis, left and right
    float m_fMaxJointSpeed = 0.0f;      ///< fastest relative rotation of any joint, rad/s
    plVec3 m_vPelvis = plVec3::MakeZero();
    plVec3 m_vPelvisVelocity = plVec3::MakeZero();
    plVec3 m_vCom = plVec3::MakeZero();
    float m_fMaxSpeed = 0.0f;           ///< fastest body, m/s
    float m_fMaxEnvelopeExcessDeg = 0.0f; ///< furthest any joint is past its hard envelope
  };

  Metrics ComputeMetrics() const;

  /// \brief Prints the sensors and, with bParts, every joint's constraint-space state to stdout. For test diagnosis.
  void DebugPrint(bool bParts = false) const;

  /// \brief Debug and test accessors.
  plUInt32 GetNumDislocatedJoints() const;
  plUInt32 GetNumLimitBounces() const { return m_uiLimitBounces; }
  float GetPeakHeadImpulse() const { return m_fPeakHeadImpulse; }
  float GetBraceWeight() const { return m_fBraceWeight; }
  float GetGuardBlend() const { return m_fGuardBlend; }
  float GetBackGuardBlend() const { return m_fBackBlend; }
  float GetCatchBlend() const { return m_fCatchBlend; }
  float GetBrakeBlend() const { return m_fBrakeBlend; }
  float GetStaggerBlend() const { return m_fStaggerBlend; }
  bool HadFirstImpact() const { return m_bFirstImpact; }
  float GetSideLandBlend() const { return m_fSideLand; }
  int GetLandSide() const { return m_iLandSide; }
  bool IsSaveStepping() const { return m_bSaveStep; }
  bool IsGripping(int iSide) const { return m_Grab[iSide].m_pGrip != nullptr; }
  float GetGrabReach(int iSide) const { return m_Grab[iSide].m_fReach; }
  plUInt32 GetNumGripsTaken() const { return m_uiGripsTaken; }
  plUInt32 GetNumGripsBroken() const { return m_uiGripsBroken; }
  plVec3 GetRestForward() const { return plVec3(m_RestFrame.m_vForward.GetX(), m_RestFrame.m_vForward.GetY(), m_RestFrame.m_vForward.GetZ()); } ///< model space
  plUInt32 GetNumEnvelopeCorrections() const { return m_uiEnvelopeCorrections; }
  plUInt32 GetNumEnvelopeProjections() const { return m_uiEnvelopeProjections; }
  bool IsRolling() const { return m_bRolling; }
  bool HasTorsoContact() const { return m_bTorsoContact; }
  bool IsAirborne() const { return m_bAirborne; }
  float GetBalanceAssist() const { return m_fBalanceAssist; }
  plUInt32 GetNumSteps() const { return m_uiStepsTaken; }
  const char* GetFallReason() const { return m_szFallReason; } ///< why balance last gave up, empty while it never has
  float GetMaxArrivalSpeedDeg() const { return m_fMaxArrivalSpeed * 57.2958f; } ///< fastest arrival at a joint limit so far
  float GetRollRate() const { return m_fRollRate; }

  struct TimelineSample
  {
    float m_fTime = 0.0f;
    float m_fConsciousness = 0.0f;
    float m_fWeights[8] = {}; ///< animation, takeover, balance, fall pose, protect, roll guard, neutral, injury flash
  };

  /// \brief A procedural pose: a target direction per humanoid bone, in the body frame of the rest pose.
  struct PoseBone
  {
    plUInt8 m_uiBone = plHumanoidBone::COUNT;
    float m_fForward = 0.0f; ///< components of the target direction
    float m_fUp = 0.0f;
    float m_fOut = 0.0f;     ///< towards the bone's own side
    float m_fTwistDeg = 0.0f;
  };

private:
  struct Part
  {
    plUInt16 m_uiJoint = 0;
    plInt16 m_iParent = -1;
    plJoltRagdollRegion::Enum m_Region = plJoltRagdollRegion::Other;
    plInt8 m_iSide = 0; ///< -1 left, +1 right, 0 centre
    plUInt8 m_uiBone = plHumanoidBone::COUNT;

    JPH::Body* m_pBody = nullptr;
    JPH::SwingTwistConstraint* m_pOriginal = nullptr;    ///< from the ragdoll, nullptr for the root and non swing-twist joints
    JPH::SwingTwistConstraint* m_pConstraint = nullptr;  ///< the one the muscles drive: the original or the dislocation replacement

    // the hard envelope, in the original constraint's space; fixed after Init()
    JPH::Quat m_qOriginalC1 = JPH::Quat::sIdentity(); ///< original constraint space to body 1
    JPH::Quat m_qOriginalC2 = JPH::Quat::sIdentity(); ///< original constraint space to body 2
    float m_fEnvelopePlane = 0.0f;
    float m_fEnvelopeNormal = 0.0f;
    float m_fEnvelopeTwistMin = 0.0f;
    float m_fEnvelopeTwistMax = 0.0f;

    float m_fMass = 0.0f;
    float m_fChainMass = 0.0f;   ///< this part and everything below it
    float m_fChainInertia = 0.0f; ///< about the joint, from the chain's mass and centre
    float m_fGravityTorque = 0.0f; ///< chain mass * g * distance to the chain centre
    float m_fSupportInertia = 0.0f; ///< inertia of the rest of the body about this joint: what a standing leg or spine carries
    float m_fFriction = 0.0f;

    JPH::Quat m_qRestGlobal = JPH::Quat::sIdentity();
    JPH::Quat m_qRelRest = JPH::Quat::sIdentity();
    JPH::Quat m_qNeutral = JPH::Quat::sIdentity();
    JPH::Quat m_qBrace = JPH::Quat::sIdentity();
    JPH::Quat m_qGuard = JPH::Quat::sIdentity(); ///< the brace pose with the arms in: elbows tucked, forearms in front of the face
    JPH::Quat m_qBackGuard = JPH::Quat::sIdentity(); ///< protection when going over backwards: chin tucked, hands behind the head, knees up
    JPH::Quat m_qFall = JPH::Quat::sIdentity();
    JPH::Quat m_qSplay = JPH::Quat::sIdentity();
    JPH::Quat m_qCatchBack = JPH::Quat::sIdentity();  ///< going over backwards: arms reach back and down for the ground, chin tucked, hips and knees bend
    JPH::Quat m_qCatchFront = JPH::Quat::sIdentity(); ///< going over forwards: arms reach forwards and down, elbows soft, head up
    JPH::Quat m_qCatchSide = JPH::Quat::sIdentity();  ///< going over sideways: the arm on the falling side reaches out and down
    JPH::Quat m_qStaggerBack = JPH::Quat::sIdentity();  ///< stumbling backwards: arms thrown forwards and out
    JPH::Quat m_qStaggerFront = JPH::Quat::sIdentity(); ///< stumbling forwards: arms out to the sides and back
    JPH::Quat m_qLandNear = JPH::Quat::sIdentity(); ///< landing on a side, for the parts on that side: arm reaches down and out, leg long
    JPH::Quat m_qLandFar = JPH::Quat::sIdentity();  ///< landing on a side, for the parts on the other side: arm guards the head, knee up
    JPH::Quat m_qAnimPrev = JPH::Quat::sIdentity();
    JPH::Quat m_qAnimCur = JPH::Quat::sIdentity();
    JPH::Quat m_qIk = JPH::Quat::sIdentity();
    JPH::Quat m_qDislocated = JPH::Quat::sIdentity(); ///< neutral target inside the dislocated range
    bool m_bHasIk = false;

    // runtime
    JPH::Vec3 m_vPrevLinVel = JPH::Vec3::sZero();
    JPH::Vec3 m_vPrevRelAngVel = JPH::Vec3::sZero();
    JPH::Vec3 m_vPosition = JPH::Vec3::sZero(); ///< cached after each step for debug drawing
    JPH::Vec3 m_vTargetEnd = JPH::Vec3::sZero(); ///< where the bone would end if it were at the muscles' target, for debug drawing
    bool m_bHasTargetEnd = false;
    float m_fEffort = 0.0f;
    float m_fYield = 0.0f; ///< 0..1, grows while the muscle pushes at its limit without moving: a limb pressed into the ground
    bool m_bContact = false; ///< touched something other than its own ragdoll in the last step
    const JPH::SwingTwistConstraint* m_pFrameOwner = nullptr; ///< the constraint m_qConstraintToBody1 was cached from
    JPH::Quat m_qConstraintToBody1 = JPH::Quat::sIdentity();
    bool m_bInSoftZone = false;
    bool m_bWasAtLimit = false;
    float m_fArrivalSpeed = 0.0f;                     ///< rad/s into the limit with which the joint reached it this step
    JPH::Vec3 m_vArrivalAxis = JPH::Vec3::sZero();    ///< world axis of that motion
    bool m_bDislocated = false;
    bool m_bDislocationPending = false;
    JPH::Vec3 m_vDislocationAxis = JPH::Vec3::sZero(); ///< world space, at detection
    plUInt8 m_uiLaunchClamp = 0;
    float m_fLaunchSpeedLimit = 0.0f;
    float m_fImpactCooldown = 0.0f;

    // arbiter accumulation
    JPH::Quat m_qAccum = JPH::Quat::sZero();
    float m_fAccumWeight = 0.0f;
    float m_fAccumTorque = 0.0f;
  };

  struct BodyFrame
  {
    JPH::Vec3 m_vUp = JPH::Vec3::sAxisZ();
    JPH::Vec3 m_vForward = JPH::Vec3::sAxisX();
    JPH::Vec3 m_vRight = JPH::Vec3::sAxisY();
  };

  struct Hit
  {
    PL_DECLARE_POD_TYPE();

    JPH::Vec3 m_vImpulse;
    JPH::Vec3 m_vPosition;
  };

  void BuildBodyMap(const plSkeletonResourceDescriptor& skeleton, plArrayPtr<const plUInt16> partToJoint);
  void ComputeRestPose(const plSkeletonResourceDescriptor& skeleton);
  void ComputeChainProperties();
  void BuildPose(plArrayPtr<const PoseBone> bones, float fVariationDeg, plInt8 iSideOnly, plUInt32 uiPoseSeed, JPH::Quat Part::* pTarget);
  void RebuildStaticPoses();
  void BuildFallPose(float fPhase);

  // step stages
  void UpdateSensors(float fDeltaTime);
  void ProposeAll(float fDeltaTime);
  void ApplyArbiter(float fDeltaTime);
  void ApplySoftLimits(float fDeltaTime);
  void ApplyHits();
  void UpdateInjury(float fDeltaTime);
  void UpdateRollGuard(float fDeltaTime);
  void UpdateBalance(float fDeltaTime);
  void UpdateFallSequence(float fDeltaTime);
  JPH::Quat GetCatchPose(const Part& part) const;
  void UpdateSideLanding(float fDeltaTime);
  void UpdateSaveStep(float fDeltaTime);
  void UpdateGrab(float fDeltaTime);
  void UpdateGrips(); ///< main thread, after the physics update: takes and breaks grips
  void ReleaseGrip(int iSide, bool bBroken);
  void ProposeArmIk(int iSide, const JPH::Vec3& vHandTarget, float fWeight);
  bool CastRayHit(const JPH::Vec3& vFrom, const JPH::Vec3& vDir, float fLength, JPH::Vec3& out_vPoint, JPH::Vec3& out_vNormal, bool& out_bStatic) const;
  bool IsSwingLeg(const Part& part) const; ///< thigh, shin or foot of the leg taking a recovery step
  void UpdateGrip();
  void AssistSwingFoot(plInt8 iSide, const JPH::Vec3& vFootTarget, float fDeltaTime, float fStrength = 1.0f);
  void RecordEvents(float fDeltaTime);
  void RecordTimeline();

  void Propose(Part& part, const JPH::Quat& qTarget, float fWeight, float fTorqueScale);
  JPH::Quat ClampToLimits(const Part& part, const JPH::Quat& qRel) const;

  /// The limits of the joint's hard envelope, in the original constraint's space: the original range opened by the
  /// dislocation slack, capped per region. A dislocated joint uses exactly these.
  void GetEnvelopeLimits(const Part& part, float& out_fPlane, float& out_fNormal, float& out_fTwistMin, float& out_fTwistMax) const;
  void CacheEnvelopes(); ///< after the body map, which assigns the regions the caps depend on
  /// Radians past the hard envelope, 0 inside it. Optionally the swing and twist parts with the world axes that make them grow.
  float ComputeEnvelopeExcess(const Part& part, float* out_pSwingExcess = nullptr, JPH::Vec3* out_pSwingAxis = nullptr, float* out_pTwistExcess = nullptr, JPH::Vec3* out_pTwistAxis = nullptr) const;
  void EnforceEnvelope(float fDeltaTime);
  void ProjectOntoEnvelope(Part& part); ///< turns the limb and everything below it back onto the envelope's edge, about the joint
  void ProposeTwoBoneIk(plUInt8 uiThighBone, const JPH::Vec3& vFootTarget, float fWeight);
  JPH::Vec3 GetJointPosition(const Part& part) const;
  plInt16 GetPart(plHumanoidBone::Enum bone) const { return m_PartForBone[bone]; }
  bool CastRayDown(const JPH::Vec3& vFrom, float fLength, float& out_fDistance, JPH::Vec3* out_pNormal) const;
  bool CastRay(const JPH::Vec3& vFrom, const JPH::Vec3& vDir, float fLength, float& out_fDistance) const;

  JPH::Ragdoll* m_pRagdoll = nullptr;
  JPH::PhysicsSystem* m_pSystem = nullptr;
  plUInt32 m_uiCollisionGroup = 0;
  plUInt32 m_uiSeed = 0;
  plRandom m_Rng;
  bool m_bHumanoid = false;

  plDynamicArray<Part> m_Parts;
  plInt16 m_PartForBone[plHumanoidBone::COUNT];
  plVec3 m_vRestJointPos[plHumanoidBone::COUNT]; ///< model space, for directions
  bool m_bRestJointValid[plHumanoidBone::COUNT] = {};
  BodyFrame m_RestFrame;
  float m_fTotalMass = 0.0f;
  float m_fLegLength = 0.9f;

  plJoltRagdollProfileDescriptor m_Profile;
  Inputs m_Inputs;
  bool m_bHasAnimTarget = false;
  float m_fTimeInFrame = 0.0f;
  float m_fTime = 0.0f;

  plHybridArray<Hit, 4> m_PendingHits; ///< written by AddHit() on the main thread, moved to m_Hits in Prepare()
  plHybridArray<Hit, 4> m_Hits;

  // sensors
  mutable std::atomic<plUInt64> m_ContactBits{0}; ///< one bit per part, set by the contact listener during a step
  JPH::Vec3 m_vUp = JPH::Vec3::sAxisZ();
  float m_fGravity = 9.81f;
  JPH::Vec3 m_vCom = JPH::Vec3::sZero();
  JPH::Vec3 m_vComVel = JPH::Vec3::sZero();
  JPH::Vec3 m_vGroundNormal = JPH::Vec3::sAxisZ();
  float m_fPelvisHeight = 1000.0f;
  float m_fAirTime = 0.0f;
  bool m_bAirborne = false;
  bool m_bTorsoContact = false;
  bool m_bFeetContact[2] = {};
  float m_fHeadTimeToImpact = 1000.0f;
  float m_fLastHeadTimeToImpact = 1000.0f;
  float m_fRollRate = 0.0f;

  // behaviour state
  float m_fFallPhase = 0.0f;
  float m_fFallWeight = 0.0f;
  float m_fBraceRaw = 0.0f;
  float m_fBraceDelay = 0.0f;
  float m_fBraceWeight = 0.0f;
  float m_fGuardBlend = 0.0f; ///< 0 = brace pose, up to s_fGuardMax towards the guard as the fall lasts longer
  float m_fFallTime = 0.0f;   ///< seconds airborne or falling over without the torso touching anything
  float m_fBackBlend = 0.0f;  ///< 0 = brace (falling forwards, face down), 1 = back guard (going over backwards, face up)
  bool m_bFallFromFeet = false;  ///< balance gave up: the arms reach for the ground until the first impact
  float m_fGiveUp = 1.0f;        ///< 0..1 since balance gave up: the balancing muscles hand over to the fall over s_fGiveUpTime
  bool m_bFirstImpact = false;   ///< something other than the feet has hit the ground since the fall began
  float m_fCatchBlend = 0.0f;    ///< 0..1 towards the catch pose; fades out after the first impact as the elbows give
  JPH::Vec3 m_vFallDir = JPH::Vec3::sZero(); ///< horizontal direction the body is pushed or goes over in, world space
  float m_fFallFwd = 0.0f;       ///< m_vFallDir along the torso's forward, -1..1
  float m_fFallSide = 0.0f;      ///< m_vFallDir along the torso's right, -1..1
  float m_fBrakeBlend = 0.0f;    ///< 0..1 arms and legs spread to stop a slide on the ground
  float m_fStaggerBlend = 0.0f;  ///< 0..1 arms out for balance while stepping
  float m_fArmPush[2] = {0.0f, 0.0f}; ///< 0..1 per arm (left, right): a hand on the ground pushing against a roll

  // side landing: going over, the body turns so a hip and shoulder take the impact instead of the back or face
  plInt8 m_iLandSide = 0;       ///< -1 left, +1 right, 0 not chosen
  float m_fSideLand = 0.0f;     ///< 0..1

  // save step: after balance gives up, one leg still reaches for the ground the body is falling towards
  bool m_bSaveStep = false;
  plInt8 m_iSaveSide = 0;
  float m_fSaveTime = 0.0f;
  JPH::Vec3 m_vSaveStart = JPH::Vec3::sZero();
  JPH::Vec3 m_vSaveTarget = JPH::Vec3::sZero();

  // grab: a hand reaches for something solid within reach and grips it, until the grip is torn loose or no longer needed
  struct Grab
  {
    bool m_bHasTarget = false;
    JPH::Vec3 m_vPoint = JPH::Vec3::sZero();  ///< on the surface, world space
    JPH::Vec3 m_vNormal = JPH::Vec3::sZero();
    float m_fReach = 0.0f;                    ///< 0..1 how far the arm has turned to the target
    float m_fGripTime = 0.0f;
    float m_fCooldown = 0.0f;                 ///< after letting go, no new grip on this side for a while
    float m_fHandDist = 0.0f;                 ///< hand to target, for the debug overlay
    JPH::Vec3 m_vHold = JPH::Vec3::sZero();   ///< where the hand took hold
    JPH::TwoBodyConstraint* m_pGrip = nullptr;
  };
  Grab m_Grab[2];                ///< left, right
  float m_fLastStepDt = 1.0f / 60.0f;
  plUInt32 m_uiProbeCounter = 0;
  plUInt32 m_uiGripsTaken = 0;
  plUInt32 m_uiGripsBroken = 0;
  bool m_bGripBoosted = false;   ///< limb friction raised for the roll guard or the brake
  plUInt32 m_uiEnvelopeCorrections = 0; ///< steps in which a joint was pushed back inside its hard envelope, per joint
  plUInt32 m_uiEnvelopeProjections = 0; ///< times a joint was moved back onto its envelope directly
  bool m_bFaceDown = false;
  float m_fRollTimer = 0.0f;
  float m_fRollCalmTimer = 0.0f;
  float m_fRollCooldown = 0.0f;
  bool m_bRolling = false;
  plInt8 m_iUphillSide = 0;
  float m_fNeutralSettle = 0.0f;
  float m_fTakeoverTime = -1.0f; ///< < 0: no hit yet
  float m_fTakeoverDepth = 1.0f; ///< how far the upper body gives, from the size of the hit
  float m_fUpperAnimWeight = 1.0f;
  float m_fLegsAnimWeight = 1.0f;

  plJoltRagdollBalanceState::Enum m_BalanceState = plJoltRagdollBalanceState::Standing;
  float m_fBalanceAssist = 0.0f;
  float m_fStepTimer = 0.0f;
  float m_fStepDuration = 0.0f;
  float m_fStepDelay = 0.0f;
  float m_fLegEffortTimer = 0.0f;
  float m_fSteadyTime = 0.0f;
  plInt8 m_iSwingSide = 0;
  JPH::Vec3 m_vStepStart = JPH::Vec3::sZero();
  JPH::Vec3 m_vStepTarget = JPH::Vec3::sZero();
  JPH::Vec3 m_vCapturePoint = JPH::Vec3::sZero();
  plUInt32 m_uiStepsTaken = 0;
  const char* m_szFallReason = "";

  // events, written in Step(), handed out in PostStep()
  plHybridArray<plMsgRagdollImpact, 8> m_Impacts;
  plHybridArray<plMsgRagdollJointDislocated, 2> m_Dislocations;
  plHybridArray<plMsgRagdollBalanceLost, 1> m_BalanceLost;
  float m_fConsciousnessDrop = 0.0f;
  float m_fEventBudget = 0.0f; ///< refills at 30 per second, bursts of up to 10
  float m_fSlowTime = 0.0f;

  // replacement constraints of dislocated joints, owned by the controller
  plHybridArray<JPH::TwoBodyConstraint*, 4> m_Replacements;

  // metrics
  plUInt32 m_uiLimitBounces = 0;
  float m_fMaxArrivalSpeed = 0.0f;
  float m_fPeakHeadImpulse = 0.0f;

  plHybridArray<TimelineSample, 256> m_Timeline;
  plUInt32 m_uiTimelineNext = 0;
};
