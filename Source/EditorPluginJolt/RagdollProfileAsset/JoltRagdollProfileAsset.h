#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <JoltPlugin/Resources/JoltRagdollProfileResource.h>

/// \brief Editor-side object model of the 'Jolt Ragdoll Profile' asset. Mirrors plJoltRagdollProfileDescriptor.
class plJoltRagdollProfileAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltRagdollProfileAssetProperties, plReflectedClass);

public:
  plJoltRagdollProfileAssetProperties();

  void ToDescriptor(plJoltRagdollProfileDescriptor& out_desc) const;
  void FromDescriptor(const plJoltRagdollProfileDescriptor& desc);

  plEnum<plJoltRagdollPreset> m_Preset;

  // one member per plJoltRagdollMuscleGroup, in its order
  plJoltRagdollMuscleSettings m_Head;
  plJoltRagdollMuscleSettings m_Neck;
  plJoltRagdollMuscleSettings m_Spine;
  plJoltRagdollMuscleSettings m_Pelvis;
  plJoltRagdollMuscleSettings m_UpperArm;
  plJoltRagdollMuscleSettings m_Forearm;
  plJoltRagdollMuscleSettings m_Thigh;
  plJoltRagdollMuscleSettings m_Shin;

  plJoltRagdollMuscleSettings* GetMuscles(plUInt32 uiGroup);
  const plJoltRagdollMuscleSettings* GetMuscles(plUInt32 uiGroup) const;

  float m_fLimpDampingFloor;
  float m_fSoftZoneDeg;
  float m_fSoftZoneStiffness;
  float m_fNeutralVariationDeg;
  float m_fImpactSpin;
  float m_fImpactSpinRandomness;
  bool m_bLegsFollowGroundedFeet;
  float m_fFallPoseStrength;
  float m_fFallPoseMinRate;
  float m_fFallPoseMaxRate;
  float m_fBraceStrength;
  float m_fBraceHeight;
  float m_fBraceFallSpeed;
  float m_fReactionDelay;
  float m_fChinTuckDeg;
  float m_fFaceTurnDeg;
  float m_fKnockOutHeadImpulse;
  float m_fRollRateThreshold;
  float m_fRollDetectTime;
  float m_fRollSpineDamping;
  float m_fRollFrictionBoost;
  bool m_bInjuryEnabled;
  float m_fInjuryJolt;
  float m_fMaxBalanceAssist;
  plUInt8 m_uiMaxRecoverySteps;
  float m_fMinSwingTime;
  float m_fMaxFootSpeed;
  float m_fTakeoverUpperTime;
  float m_fTakeoverLegsTime;
};

class plJoltRagdollProfileAssetDocument : public plSimpleAssetDocument<plJoltRagdollProfileAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltRagdollProfileAssetDocument, plSimpleAssetDocument<plJoltRagdollProfileAssetProperties>);

public:
  plJoltRagdollProfileAssetDocument(plStringView sDocumentPath);

  plStatus WriteAsset(plStreamWriter& inout_stream, const plPlatformProfile* pAssetProfile) const;

  /// \brief Writes the values of a preset into the asset as one undoable step. Custom does nothing.
  void ApplyPreset(plJoltRagdollPreset::Enum preset);

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& inout_stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};
