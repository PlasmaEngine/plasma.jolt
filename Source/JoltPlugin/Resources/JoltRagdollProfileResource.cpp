#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <JoltPlugin/Resources/JoltRagdollProfileResource.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltRagdollRegion, 1)
  PL_ENUM_CONSTANTS(plJoltRagdollRegion::Other, plJoltRagdollRegion::Head, plJoltRagdollRegion::Neck, plJoltRagdollRegion::Chest, plJoltRagdollRegion::Spine, plJoltRagdollRegion::Pelvis)
  PL_ENUM_CONSTANTS(plJoltRagdollRegion::UpperArm, plJoltRagdollRegion::LowerArm, plJoltRagdollRegion::Hand, plJoltRagdollRegion::Thigh, plJoltRagdollRegion::Shin, plJoltRagdollRegion::Foot)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltRagdollPreset, 1)
  PL_ENUM_CONSTANTS(plJoltRagdollPreset::Dead, plJoltRagdollPreset::Stunned, plJoltRagdollPreset::Conscious, plJoltRagdollPreset::Athlete, plJoltRagdollPreset::Slapstick, plJoltRagdollPreset::Custom)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_TYPE(plJoltRagdollMuscleSettings, plNoBase, 1, plRTTIDefaultAllocator<plJoltRagdollMuscleSettings>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Strength", m_fFrequency)->AddAttributes(new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.0f, 30.0f), new plSuffixAttribute(" Hz")),
    PL_MEMBER_PROPERTY("Damping", m_fDampingRatio)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 3.0f), new plSuffixAttribute(" ζ")),
    PL_MEMBER_PROPERTY("TorqueCap", m_fTorqueCap)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.0f, 20.0f), new plSuffixAttribute(" ×g")),
    PL_MEMBER_PROPERTY("NeutralPull", m_fNeutralPull)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("DislocationSpeed", m_fDislocationSpeed)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 5000.0f), new plSuffixAttribute(" °/s")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltRagdollProfileResource, 1, plRTTIDefaultAllocator<plJoltRagdollProfileResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plJoltRagdollProfileResource);

//////////////////////////////////////////////////////////////////////////

plJoltRagdollMuscleGroup::Enum plJoltRagdollMuscleGroup::FromRegion(plJoltRagdollRegion::Enum region)
{
  switch (region)
  {
    case plJoltRagdollRegion::Head:
      return Head;
    case plJoltRagdollRegion::Neck:
      return Neck;
    case plJoltRagdollRegion::Pelvis:
      return Pelvis;
    case plJoltRagdollRegion::UpperArm:
      return UpperArm;
    case plJoltRagdollRegion::LowerArm:
    case plJoltRagdollRegion::Hand:
      return Forearm;
    case plJoltRagdollRegion::Thigh:
      return Thigh;
    case plJoltRagdollRegion::Shin:
    case plJoltRagdollRegion::Foot:
      return Shin;
    default:
      return Spine;
  }
}

const char* plJoltRagdollMuscleGroup::GetName(Enum group)
{
  static const char* s_Names[COUNT] = {"Head", "Neck", "Spine", "Pelvis", "UpperArm", "Forearm", "Thigh", "Shin"};
  return s_Names[group];
}

namespace
{
  // Conscious values per muscle group: Hz, ζ, torque cap, neutral pull
  constexpr float s_Conscious[plJoltRagdollMuscleGroup::COUNT][4] = {
    {4.0f, 0.9f, 1.5f, 0.3f},   // Head
    {5.5f, 1.0f, 2.0f, 0.4f},   // Neck
    {6.0f, 0.8f, 2.5f, 0.35f},  // Spine
    {6.5f, 0.8f, 3.0f, 0.2f},   // Pelvis
    {3.5f, 0.6f, 1.2f, 0.25f},  // UpperArm
    {3.0f, 0.5f, 1.0f, 0.25f},  // Forearm
    {5.0f, 0.8f, 2.5f, 0.3f},   // Thigh
    {4.5f, 0.85f, 2.0f, 0.3f}}; // Shin

  void FillMuscles(plJoltRagdollMuscleSettings* pMuscles, const float* pMultipliers, float fDislocation)
  {
    for (plUInt32 g = 0; g < plJoltRagdollMuscleGroup::COUNT; ++g)
    {
      pMuscles[g].m_fFrequency = s_Conscious[g][0] * pMultipliers[0];
      pMuscles[g].m_fDampingRatio = plMath::Min(1.2f, s_Conscious[g][1] * pMultipliers[1]);
      pMuscles[g].m_fTorqueCap = s_Conscious[g][2] * pMultipliers[2];
      pMuscles[g].m_fNeutralPull = s_Conscious[g][3] * pMultipliers[3];

      // elbows, knees and shoulders can dislocate; the head, neck, spine and pelvis never do
      const bool bCanDislocate = g == plJoltRagdollMuscleGroup::UpperArm || g == plJoltRagdollMuscleGroup::Forearm || g == plJoltRagdollMuscleGroup::Shin;
      pMuscles[g].m_fDislocationSpeed = bCanDislocate ? fDislocation : 0.0f;
    }
  }
} // namespace

// Measured in the test room: ordinary 1.5 m drops reach the limits of elbows, knees and shoulders at up to ~2750°/s, so the
// default stays above that. Slapstick dislocates on purpose.
static constexpr float s_fDefaultDislocationSpeed = 3000.0f;

plJoltRagdollProfileDescriptor::plJoltRagdollProfileDescriptor()
{
  const float m[4] = {1, 1, 1, 1};
  FillMuscles(m_Muscles, m, s_fDefaultDislocationSpeed);
}

void plJoltRagdollProfileDescriptor::ApplyPreset(plJoltRagdollPreset::Enum preset)
{
  // multipliers on the conscious values: Hz, ζ, cap, pull
  float m[4] = {1, 1, 1, 1};
  float fDislocation = s_fDefaultDislocationSpeed;

  // start from the defaults, so values a preset does not mention are reset too
  *this = plJoltRagdollProfileDescriptor();

  switch (preset)
  {
    case plJoltRagdollPreset::Dead:
      m[0] = 0.0f, m[1] = 0.5f, m[2] = 0.15f, m[3] = 0.5f;
      m_fImpactSpin = 1.0f;
      m_fFallPoseStrength = 0.0f;
      m_fBraceStrength = 0.0f;
      break;

    case plJoltRagdollPreset::Stunned:
      m[0] = 0.5f, m[1] = 1.1f, m[2] = 0.55f, m[3] = 0.9f;
      m_fFallPoseStrength = 0.25f;
      m_fBraceStrength = 0.4f;
      m_fReactionDelay = 0.15f;
      break;

    case plJoltRagdollPreset::Athlete:
      m[0] = 1.3f, m[1] = 1.1f, m[2] = 1.4f, m[3] = 1.1f;
      m_fImpactSpin = 0.8f;
      m_fReactionDelay = 0.06f;
      m_fMaxBalanceAssist = 0.2f;
      break;

    case plJoltRagdollPreset::Slapstick:
      m[0] = 0.6f, m[1] = 0.35f, m[2] = 0.8f, m[3] = 0.6f;
      m_fLimpDampingFloor = 0.05f;
      m_fImpactSpin = 2.2f;
      m_fImpactSpinRandomness = 0.6f;
      m_fFallPoseStrength = 0.8f;
      m_fFallPoseMaxRate = 3.0f;
      m_bInjuryEnabled = true;
      fDislocation = 380.0f;
      break;

    default:
      break;
  }

  FillMuscles(m_Muscles, m, fDislocation);
  m_Preset = preset;
}

plResult plJoltRagdollProfileDescriptor::Serialize(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_Preset;

  for (const auto& mus : m_Muscles)
  {
    inout_stream << mus.m_fFrequency << mus.m_fDampingRatio << mus.m_fTorqueCap << mus.m_fNeutralPull << mus.m_fDislocationSpeed;
  }

  inout_stream << m_fLimpDampingFloor << m_fSoftZoneDeg << m_fSoftZoneStiffness;
  inout_stream << m_fNeutralVariationDeg;
  inout_stream << m_fImpactSpin << m_fImpactSpinRandomness << m_bLegsFollowGroundedFeet;
  inout_stream << m_fFallPoseStrength << m_fFallPoseMinRate << m_fFallPoseMaxRate;
  inout_stream << m_fBraceStrength << m_fBraceHeight << m_fBraceFallSpeed << m_fReactionDelay << m_fChinTuckDeg << m_fFaceTurnDeg << m_fKnockOutHeadImpulse;
  inout_stream << m_fRollRateThreshold << m_fRollDetectTime << m_fRollSpineDamping << m_fRollFrictionBoost;
  inout_stream << m_bInjuryEnabled << m_fInjuryJolt;
  inout_stream << m_fMaxBalanceAssist << m_uiMaxRecoverySteps << m_fMinSwingTime << m_fMaxFootSpeed << m_fTakeoverUpperTime << m_fTakeoverLegsTime;

  return PL_SUCCESS;
}

plResult plJoltRagdollProfileDescriptor::Deserialize(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion != 1)
    return PL_FAILURE;

  inout_stream >> m_Preset;

  for (auto& mus : m_Muscles)
  {
    inout_stream >> mus.m_fFrequency >> mus.m_fDampingRatio >> mus.m_fTorqueCap >> mus.m_fNeutralPull >> mus.m_fDislocationSpeed;
  }

  inout_stream >> m_fLimpDampingFloor >> m_fSoftZoneDeg >> m_fSoftZoneStiffness;
  inout_stream >> m_fNeutralVariationDeg;
  inout_stream >> m_fImpactSpin >> m_fImpactSpinRandomness >> m_bLegsFollowGroundedFeet;
  inout_stream >> m_fFallPoseStrength >> m_fFallPoseMinRate >> m_fFallPoseMaxRate;
  inout_stream >> m_fBraceStrength >> m_fBraceHeight >> m_fBraceFallSpeed >> m_fReactionDelay >> m_fChinTuckDeg >> m_fFaceTurnDeg >> m_fKnockOutHeadImpulse;
  inout_stream >> m_fRollRateThreshold >> m_fRollDetectTime >> m_fRollSpineDamping >> m_fRollFrictionBoost;
  inout_stream >> m_bInjuryEnabled >> m_fInjuryJolt;
  inout_stream >> m_fMaxBalanceAssist >> m_uiMaxRecoverySteps >> m_fMinSwingTime >> m_fMaxFootSpeed >> m_fTakeoverUpperTime >> m_fTakeoverLegsTime;

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////

plJoltRagdollProfileResource::plJoltRagdollProfileResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

plJoltRagdollProfileResource::~plJoltRagdollProfileResource() = default;

plResourceLoadDesc plJoltRagdollProfileResource::UnloadData(Unload WhatToUnload)
{
  m_Descriptor = plJoltRagdollProfileDescriptor();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;
  return res;
}

plResourceLoadDesc plJoltRagdollProfileResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plJoltRagdollProfileResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (Stream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  // skip the absolute file path data that the standard file reader writes into the stream
  plStringBuilder sAbsFilePath;
  (*Stream) >> sAbsFilePath;

  // skip the asset file header at the start of the file
  plAssetFileHeader AssetHash;
  AssetHash.Read(*Stream).IgnoreResult();

  plJoltRagdollProfileDescriptor desc;
  if (desc.Deserialize(*Stream).Failed())
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  CreateResource(std::move(desc));

  res.m_State = plResourceState::Loaded;
  return res;
}

void plJoltRagdollProfileResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plJoltRagdollProfileResource);
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plJoltRagdollProfileResource, plJoltRagdollProfileDescriptor)
{
  m_Descriptor = std::move(descriptor);
  ++m_uiContentVersion;

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;
  return res;
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Resources_JoltRagdollProfileResource);
