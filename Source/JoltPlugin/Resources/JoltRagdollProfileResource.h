#pragma once

#include <Core/ResourceManager/Resource.h>
#include <JoltPlugin/JoltPluginDLL.h>

using plJoltRagdollProfileResourceHandle = plTypedResourceHandle<class plJoltRagdollProfileResource>;

/// \brief The body region a ragdoll part belongs to. Derived from the skeleton's humanoid avatar.
struct PL_JOLTPLUGIN_DLL plJoltRagdollRegion
{
  using StorageType = plUInt8;

  enum Enum
  {
    Other,
    Head,
    Neck,
    Chest,
    Spine,
    Pelvis,
    UpperArm,
    LowerArm,
    Hand,
    Thigh,
    Shin,
    Foot,

    Default = Other
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltRagdollRegion);

/// \brief The groups of regions that share one set of muscle values in a profile.
struct PL_JOLTPLUGIN_DLL plJoltRagdollMuscleGroup
{
  enum Enum : plUInt8
  {
    Head,
    Neck,
    Spine, ///< Chest, spine and shoulders
    Pelvis,
    UpperArm,
    Forearm, ///< Lower arm and hand
    Thigh,
    Shin, ///< Shin and foot

    COUNT
  };

  static Enum FromRegion(plJoltRagdollRegion::Enum region);
  static const char* GetName(Enum group);
};

/// \brief The starting point a profile was made from. Custom means the values were edited.
struct PL_JOLTPLUGIN_DLL plJoltRagdollPreset
{
  using StorageType = plUInt8;

  enum Enum
  {
    Dead,
    Stunned,
    Conscious,
    Athlete,
    Slapstick,
    Custom,

    Default = Conscious
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltRagdollPreset);

/// \brief Muscle values for one muscle group.
///
/// Strength is a spring frequency and damping a ratio, so the values do not depend on mass or scale.
/// The torque cap is a multiple of the gravity torque of the limb chain that hangs off the joint.
struct PL_JOLTPLUGIN_DLL plJoltRagdollMuscleSettings
{
  float m_fFrequency = 4.0f;          ///< Hz. 0 = the joint has no muscle and only gets damping.
  float m_fDampingRatio = 0.8f;       ///< ζ
  float m_fTorqueCap = 2.0f;          ///< multiple of the chain's own gravity torque
  float m_fNeutralPull = 0.3f;        ///< 0..1 share of the torque cap the neutral pose may use
  float m_fDislocationSpeed = 0.0f;   ///< deg/s at the hard limit that dislocates the joint; 0 = never
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltRagdollMuscleSettings);

/// \brief All tuning values of a ragdoll profile.
struct PL_JOLTPLUGIN_DLL plJoltRagdollProfileDescriptor
{
  plJoltRagdollProfileDescriptor();

  plEnum<plJoltRagdollPreset> m_Preset;
  plJoltRagdollMuscleSettings m_Muscles[plJoltRagdollMuscleGroup::COUNT];

  // damping and soft limits
  float m_fLimpDampingFloor = 0.15f; ///< ζ that limp joints always keep
  float m_fSoftZoneDeg = 12.0f;      ///< the last part of each joint's travel in which motion into the limit is resisted
  float m_fSoftZoneStiffness = 8.0f; ///< Hz at the end of the soft zone

  // neutral pose
  float m_fNeutralVariationDeg = 8.0f; ///< seeded per character and region

  // impact spin
  float m_fImpactSpin = 1.0f;          ///< 0 = off
  float m_fImpactSpinRandomness = 0.5f;
  bool m_bLegsFollowGroundedFeet = true;

  // fall pose
  float m_fFallPoseStrength = 0.5f; ///< 0 = off
  float m_fFallPoseMinRate = 0.5f;  ///< playback rate while falling fast
  float m_fFallPoseMaxRate = 2.0f;  ///< playback rate while rising fast

  // protect face
  float m_fBraceStrength = 0.8f;      ///< 0 = off
  float m_fBraceHeight = 1.2f;        ///< m above the floor where the brace pose starts to blend in
  float m_fBraceFallSpeed = 2.5f;     ///< m/s downward speed for the full brace pose
  float m_fReactionDelay = 0.08f;     ///< s
  float m_fChinTuckDeg = 22.0f;
  float m_fFaceTurnDeg = 35.0f;
  float m_fKnockOutHeadImpulse = 45.0f; ///< N·s on the head that drops consciousness; 0 = never

  // roll guard
  float m_fRollRateThreshold = 1.5f;  ///< rad/s about the spine
  float m_fRollDetectTime = 0.2f;     ///< s
  float m_fRollSpineDamping = 3.0f;   ///< multiplier on the spine-axis damping while rolling
  float m_fRollFrictionBoost = 1.5f;  ///< friction multiplier on hands, forearms and shins while rolling

  // injury
  bool m_bInjuryEnabled = false;
  float m_fInjuryJolt = 0.15f; ///< share of the limb's angular momentum given as a jolt

  // balance
  float m_fMaxBalanceAssist = 0.25f; ///< share of m·g·h
  plUInt8 m_uiMaxRecoverySteps = 2;
  float m_fMinSwingTime = 0.25f;     ///< s
  float m_fMaxFootSpeed = 4.0f;      ///< m/s
  float m_fTakeoverUpperTime = 0.1f; ///< s until the upper body is fully physical after a hit
  float m_fTakeoverLegsTime = 0.6f;  ///< s until the legs are fully physical after a hit

  /// \brief Fills all values from a preset. Leaves m_Preset set to that preset.
  void ApplyPreset(plJoltRagdollPreset::Enum preset);

  const plJoltRagdollMuscleSettings& GetMuscles(plJoltRagdollRegion::Enum region) const { return m_Muscles[plJoltRagdollMuscleGroup::FromRegion(region)]; }

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);
};

/// \brief Runtime resource of a 'Jolt Ragdoll Profile' asset.
class PL_JOLTPLUGIN_DLL plJoltRagdollProfileResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltRagdollProfileResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plJoltRagdollProfileResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plJoltRagdollProfileResource, plJoltRagdollProfileDescriptor);

public:
  plJoltRagdollProfileResource();
  ~plJoltRagdollProfileResource();

  const plJoltRagdollProfileDescriptor& GetDescriptor() const { return m_Descriptor; }

  /// \brief Incremented whenever the content changes, so ragdolls can pick up live edits.
  plUInt32 GetContentVersion() const { return m_uiContentVersion; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plJoltRagdollProfileDescriptor m_Descriptor;
  plUInt32 m_uiContentVersion = 0;
};
