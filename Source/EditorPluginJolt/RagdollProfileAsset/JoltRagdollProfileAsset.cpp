#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAsset.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltRagdollProfileAssetProperties, 1, plRTTIDefaultAllocator<plJoltRagdollProfileAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Preset", plJoltRagdollPreset, m_Preset),

    PL_MEMBER_PROPERTY("Head", m_Head)->AddAttributes(new plGroupAttribute("Muscles by region")),
    PL_MEMBER_PROPERTY("Neck", m_Neck),
    PL_MEMBER_PROPERTY("Spine", m_Spine),
    PL_MEMBER_PROPERTY("Pelvis", m_Pelvis),
    PL_MEMBER_PROPERTY("UpperArm", m_UpperArm),
    PL_MEMBER_PROPERTY("Forearm", m_Forearm),
    PL_MEMBER_PROPERTY("Thigh", m_Thigh),
    PL_MEMBER_PROPERTY("Shin", m_Shin),

    PL_MEMBER_PROPERTY("LimpDampingFloor", m_fLimpDampingFloor)->AddAttributes(new plGroupAttribute("Damping and soft limits"), new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 2.0f), new plSuffixAttribute(" ζ")),
    PL_MEMBER_PROPERTY("SoftZone", m_fSoftZoneDeg)->AddAttributes(new plDefaultValueAttribute(12.0f), new plClampValueAttribute(0.0f, 45.0f), new plSuffixAttribute("°")),
    PL_MEMBER_PROPERTY("SoftZoneStiffness", m_fSoftZoneStiffness)->AddAttributes(new plDefaultValueAttribute(8.0f), new plClampValueAttribute(0.5f, 30.0f), new plSuffixAttribute(" Hz")),

    PL_MEMBER_PROPERTY("NeutralVariation", m_fNeutralVariationDeg)->AddAttributes(new plGroupAttribute("Neutral pose"), new plDefaultValueAttribute(8.0f), new plClampValueAttribute(0.0f, 30.0f), new plSuffixAttribute("°")),

    PL_MEMBER_PROPERTY("ImpactSpin", m_fImpactSpin)->AddAttributes(new plGroupAttribute("Impact spin"), new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 5.0f), new plSuffixAttribute("×")),
    PL_MEMBER_PROPERTY("ImpactSpinRandomness", m_fImpactSpinRandomness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("LegsFollowGroundedFeet", m_bLegsFollowGroundedFeet)->AddAttributes(new plDefaultValueAttribute(true)),

    PL_MEMBER_PROPERTY("FallPoseStrength", m_fFallPoseStrength)->AddAttributes(new plGroupAttribute("Fall pose"), new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("FallPoseMinRate", m_fFallPoseMinRate)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.1f, 1.0f), new plSuffixAttribute("×")),
    PL_MEMBER_PROPERTY("FallPoseMaxRate", m_fFallPoseMaxRate)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(1.0f, 5.0f), new plSuffixAttribute("×")),

    PL_MEMBER_PROPERTY("BraceStrength", m_fBraceStrength)->AddAttributes(new plGroupAttribute("Protect face"), new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("BraceHeight", m_fBraceHeight)->AddAttributes(new plDefaultValueAttribute(1.2f), new plClampValueAttribute(0.1f, 5.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("BraceFallSpeed", m_fBraceFallSpeed)->AddAttributes(new plDefaultValueAttribute(2.5f), new plClampValueAttribute(0.1f, 20.0f), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("ReactionDelay", m_fReactionDelay)->AddAttributes(new plDefaultValueAttribute(0.08f), new plClampValueAttribute(0.0f, 1.0f), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("ChinTuck", m_fChinTuckDeg)->AddAttributes(new plDefaultValueAttribute(22.0f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute("°")),
    PL_MEMBER_PROPERTY("FaceTurn", m_fFaceTurnDeg)->AddAttributes(new plDefaultValueAttribute(35.0f), new plClampValueAttribute(0.0f, 80.0f), new plSuffixAttribute("°")),
    PL_MEMBER_PROPERTY("KnockOutHeadImpulse", m_fKnockOutHeadImpulse)->AddAttributes(new plDefaultValueAttribute(45.0f), new plClampValueAttribute(0.0f, 1000.0f), new plSuffixAttribute(" N·s")),

    PL_MEMBER_PROPERTY("RollRateThreshold", m_fRollRateThreshold)->AddAttributes(new plGroupAttribute("Roll guard"), new plDefaultValueAttribute(1.5f), new plClampValueAttribute(0.1f, 20.0f), new plSuffixAttribute(" rad/s")),
    PL_MEMBER_PROPERTY("RollDetectTime", m_fRollDetectTime)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 2.0f), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("RollSpineDamping", m_fRollSpineDamping)->AddAttributes(new plDefaultValueAttribute(3.0f), new plClampValueAttribute(0.0f, 20.0f), new plSuffixAttribute("×")),
    PL_MEMBER_PROPERTY("RollFrictionBoost", m_fRollFrictionBoost)->AddAttributes(new plDefaultValueAttribute(1.5f), new plClampValueAttribute(1.0f, 5.0f), new plSuffixAttribute("×")),

    PL_MEMBER_PROPERTY("InjuryEnabled", m_bInjuryEnabled)->AddAttributes(new plGroupAttribute("Injury"), new plDefaultValueAttribute(false)),
    PL_MEMBER_PROPERTY("InjuryJolt", m_fInjuryJolt)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 1.0f)),

    PL_MEMBER_PROPERTY("MaxBalanceAssist", m_fMaxBalanceAssist)->AddAttributes(new plGroupAttribute("Balance"), new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("MaxRecoverySteps", m_uiMaxRecoverySteps)->AddAttributes(new plDefaultValueAttribute(2), new plClampValueAttribute(0, 5)),
    PL_MEMBER_PROPERTY("MinSwingTime", m_fMinSwingTime)->AddAttributes(new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.05f, 2.0f), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("MaxFootSpeed", m_fMaxFootSpeed)->AddAttributes(new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.1f, 20.0f), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("TakeoverUpperTime", m_fTakeoverUpperTime)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 5.0f), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("TakeoverLegsTime", m_fTakeoverLegsTime)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 5.0f), new plSuffixAttribute(" s")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltRagdollProfileAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plJoltRagdollProfileAssetProperties::plJoltRagdollProfileAssetProperties()
{
  FromDescriptor(plJoltRagdollProfileDescriptor());
}

plJoltRagdollMuscleSettings* plJoltRagdollProfileAssetProperties::GetMuscles(plUInt32 uiGroup)
{
  plJoltRagdollMuscleSettings* groups[plJoltRagdollMuscleGroup::COUNT] = {&m_Head, &m_Neck, &m_Spine, &m_Pelvis, &m_UpperArm, &m_Forearm, &m_Thigh, &m_Shin};
  return groups[uiGroup];
}

const plJoltRagdollMuscleSettings* plJoltRagdollProfileAssetProperties::GetMuscles(plUInt32 uiGroup) const
{
  return const_cast<plJoltRagdollProfileAssetProperties*>(this)->GetMuscles(uiGroup);
}

void plJoltRagdollProfileAssetProperties::ToDescriptor(plJoltRagdollProfileDescriptor& out_desc) const
{
  out_desc.m_Preset = m_Preset;
  for (plUInt32 g = 0; g < plJoltRagdollMuscleGroup::COUNT; ++g)
    out_desc.m_Muscles[g] = *GetMuscles(g);

  out_desc.m_fLimpDampingFloor = m_fLimpDampingFloor;
  out_desc.m_fSoftZoneDeg = m_fSoftZoneDeg;
  out_desc.m_fSoftZoneStiffness = m_fSoftZoneStiffness;
  out_desc.m_fNeutralVariationDeg = m_fNeutralVariationDeg;
  out_desc.m_fImpactSpin = m_fImpactSpin;
  out_desc.m_fImpactSpinRandomness = m_fImpactSpinRandomness;
  out_desc.m_bLegsFollowGroundedFeet = m_bLegsFollowGroundedFeet;
  out_desc.m_fFallPoseStrength = m_fFallPoseStrength;
  out_desc.m_fFallPoseMinRate = m_fFallPoseMinRate;
  out_desc.m_fFallPoseMaxRate = m_fFallPoseMaxRate;
  out_desc.m_fBraceStrength = m_fBraceStrength;
  out_desc.m_fBraceHeight = m_fBraceHeight;
  out_desc.m_fBraceFallSpeed = m_fBraceFallSpeed;
  out_desc.m_fReactionDelay = m_fReactionDelay;
  out_desc.m_fChinTuckDeg = m_fChinTuckDeg;
  out_desc.m_fFaceTurnDeg = m_fFaceTurnDeg;
  out_desc.m_fKnockOutHeadImpulse = m_fKnockOutHeadImpulse;
  out_desc.m_fRollRateThreshold = m_fRollRateThreshold;
  out_desc.m_fRollDetectTime = m_fRollDetectTime;
  out_desc.m_fRollSpineDamping = m_fRollSpineDamping;
  out_desc.m_fRollFrictionBoost = m_fRollFrictionBoost;
  out_desc.m_bInjuryEnabled = m_bInjuryEnabled;
  out_desc.m_fInjuryJolt = m_fInjuryJolt;
  out_desc.m_fMaxBalanceAssist = m_fMaxBalanceAssist;
  out_desc.m_uiMaxRecoverySteps = m_uiMaxRecoverySteps;
  out_desc.m_fMinSwingTime = m_fMinSwingTime;
  out_desc.m_fMaxFootSpeed = m_fMaxFootSpeed;
  out_desc.m_fTakeoverUpperTime = m_fTakeoverUpperTime;
  out_desc.m_fTakeoverLegsTime = m_fTakeoverLegsTime;
}

void plJoltRagdollProfileAssetProperties::FromDescriptor(const plJoltRagdollProfileDescriptor& desc)
{
  m_Preset = desc.m_Preset;
  for (plUInt32 g = 0; g < plJoltRagdollMuscleGroup::COUNT; ++g)
    *GetMuscles(g) = desc.m_Muscles[g];

  m_fLimpDampingFloor = desc.m_fLimpDampingFloor;
  m_fSoftZoneDeg = desc.m_fSoftZoneDeg;
  m_fSoftZoneStiffness = desc.m_fSoftZoneStiffness;
  m_fNeutralVariationDeg = desc.m_fNeutralVariationDeg;
  m_fImpactSpin = desc.m_fImpactSpin;
  m_fImpactSpinRandomness = desc.m_fImpactSpinRandomness;
  m_bLegsFollowGroundedFeet = desc.m_bLegsFollowGroundedFeet;
  m_fFallPoseStrength = desc.m_fFallPoseStrength;
  m_fFallPoseMinRate = desc.m_fFallPoseMinRate;
  m_fFallPoseMaxRate = desc.m_fFallPoseMaxRate;
  m_fBraceStrength = desc.m_fBraceStrength;
  m_fBraceHeight = desc.m_fBraceHeight;
  m_fBraceFallSpeed = desc.m_fBraceFallSpeed;
  m_fReactionDelay = desc.m_fReactionDelay;
  m_fChinTuckDeg = desc.m_fChinTuckDeg;
  m_fFaceTurnDeg = desc.m_fFaceTurnDeg;
  m_fKnockOutHeadImpulse = desc.m_fKnockOutHeadImpulse;
  m_fRollRateThreshold = desc.m_fRollRateThreshold;
  m_fRollDetectTime = desc.m_fRollDetectTime;
  m_fRollSpineDamping = desc.m_fRollSpineDamping;
  m_fRollFrictionBoost = desc.m_fRollFrictionBoost;
  m_bInjuryEnabled = desc.m_bInjuryEnabled;
  m_fInjuryJolt = desc.m_fInjuryJolt;
  m_fMaxBalanceAssist = desc.m_fMaxBalanceAssist;
  m_uiMaxRecoverySteps = desc.m_uiMaxRecoverySteps;
  m_fMinSwingTime = desc.m_fMinSwingTime;
  m_fMaxFootSpeed = desc.m_fMaxFootSpeed;
  m_fTakeoverUpperTime = desc.m_fTakeoverUpperTime;
  m_fTakeoverLegsTime = desc.m_fTakeoverLegsTime;
}

//////////////////////////////////////////////////////////////////////////

plJoltRagdollProfileAssetDocument::plJoltRagdollProfileAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plJoltRagdollProfileAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

plStatus plJoltRagdollProfileAssetDocument::WriteAsset(plStreamWriter& inout_stream, const plPlatformProfile* pAssetProfile) const
{
  plJoltRagdollProfileDescriptor desc;
  GetProperties()->ToDescriptor(desc);
  PL_SUCCEED_OR_RETURN(desc.Serialize(inout_stream));
  return plStatus(PL_SUCCESS);
}

plTransformStatus plJoltRagdollProfileAssetDocument::InternalTransformAsset(plStreamWriter& inout_stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  return WriteAsset(inout_stream, pAssetProfile);
}

namespace
{
  /// Writes every member of a native object into the matching document object, recursing into embedded structs.
  void WriteMembers(plObjectAccessorBase* pAccessor, const plDocumentObject* pDocObject, const plRTTI* pType, const void* pNative)
  {
    plHybridArray<const plAbstractProperty*, 64> properties;
    pType->GetAllProperties(properties);

    for (const plAbstractProperty* pProp : properties)
    {
      if (pProp->GetCategory() != plPropertyCategory::Member || plStringUtils::IsEqual(pProp->GetPropertyName(), "Preset"))
        continue;

      auto pMember = static_cast<const plAbstractMemberProperty*>(pProp);

      if (pProp->GetFlags().IsSet(plPropertyFlags::Class) && !pProp->GetFlags().IsSet(plPropertyFlags::Pointer))
      {
        if (const plDocumentObject* pChild = pAccessor->GetChildObjectByName(pDocObject, pProp->GetPropertyName(), plVariant()))
          WriteMembers(pAccessor, pChild, pProp->GetSpecificType(), pMember->GetPropertyPointer(pNative));
      }
      else
      {
        pAccessor->SetValueByName(pDocObject, pProp->GetPropertyName(), plReflectionUtils::GetMemberPropertyValue(pMember, pNative)).IgnoreResult();
      }
    }
  }
} // namespace

void plJoltRagdollProfileAssetDocument::ApplyPreset(plJoltRagdollPreset::Enum preset)
{
  if (preset == plJoltRagdollPreset::Custom)
    return;

  plJoltRagdollProfileDescriptor desc;
  desc.ApplyPreset(preset);

  plJoltRagdollProfileAssetProperties values;
  values.FromDescriptor(desc);

  const plDocumentObject* pRoot = GetObjectManager()->GetRootObject()->GetChildren()[0];
  plObjectAccessorBase* pAccessor = GetObjectAccessor();

  pAccessor->StartTransaction("Apply Ragdoll Preset");
  WriteMembers(pAccessor, pRoot, plGetStaticRTTI<plJoltRagdollProfileAssetProperties>(), &values);
  pAccessor->FinishTransaction();
}
