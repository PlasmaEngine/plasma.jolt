#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Interfaces/WindWorldModule.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/World/SpatialData.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Time/Stopwatch.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/Animation/Skeletal/AnimatedMeshComponent.h>
#include <GameEngine/Configuration/GameSettings.h>
#include <GameEngine/GameApplication/GameApplication.h>
#include <JoltPlugin/CharacterCloth/JoltCharacterClothComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <Core/Graphics/Camera.h>
#include <RendererCore/AnimationSystem/Declarations.h>
#include <RendererCore/Meshes/ComputeSkinningPool.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Shader/Types.h>

// clang-format off
plCVarBool cvar_JoltClothEnable("Jolt.Cloth.Enable", true, plCVarFlags::Default, "Simulate character cloth. Off draws every garment skinned.");
plCVarBool cvar_JoltClothParallel("Jolt.Cloth.Parallel", true, plCVarFlags::Default, "Step character cloth garments on worker threads.");
plCVarInt cvar_JoltClothMaxGarments("Jolt.Cloth.MaxSimulatedGarments", -1, plCVarFlags::Default, "How many character cloth garments may simulate at once, whatever the quality tier. The nearest ones win; the rest are drawn skinned. Garments with Importance Always simulate on top of this. -1 takes the budget of the active quality tier (Jolt.Cloth.Budget.*).");
plCVarInt cvar_JoltClothBudgetLow("Jolt.Cloth.Budget.Low", 4, plCVarFlags::Save, "Garments that may simulate at once in the Low quality tier. A negative value means no limit. A project sets its budgets in RuntimeConfigs/CVars.cfg.");
plCVarInt cvar_JoltClothBudgetMedium("Jolt.Cloth.Budget.Medium", 8, plCVarFlags::Save, "Garments that may simulate at once in the Medium quality tier, and wherever no tier is known.");
plCVarInt cvar_JoltClothBudgetHigh("Jolt.Cloth.Budget.High", 12, plCVarFlags::Save, "Garments that may simulate at once in the High quality tier.");
plCVarInt cvar_JoltClothBudgetMax("Jolt.Cloth.Budget.Max", 24, plCVarFlags::Save, "Garments that may simulate at once in the Max quality tier.");
plCVarInt cvar_JoltClothBudgetMobile("Jolt.Cloth.Budget.Mobile", 2, plCVarFlags::Save, "Garments that may simulate at once in the Mobile quality tier.");
plCVarInt cvar_JoltClothView("Jolt.Cloth.View", 0, plCVarFlags::Default, "Draws character cloth debug views. 0 off, 1 Sim Mesh, 2 Max Distance, 3 Backstop, 4 Colliders, 5 Strain, 6 State, 7 Cost.");

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothImportance, 1)
  PL_ENUM_CONSTANTS(plJoltClothImportance::Low, plJoltClothImportance::Normal, plJoltClothImportance::High, plJoltClothImportance::Always)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothWorldCollisionMode, 1)
  PL_ENUM_CONSTANTS(plJoltClothWorldCollisionMode::Asset, plJoltClothWorldCollisionMode::Off, plJoltClothWorldCollisionMode::Static, plJoltClothWorldCollisionMode::All)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_COMPONENT_TYPE(plJoltCharacterClothComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_RESOURCE_ACCESSOR_PROPERTY("Cloth", GetCloth, SetCloth)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Jolt_Cloth")),
    PL_ACCESSOR_PROPERTY("Simulate", GetSimulate, SetSimulateProperty)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ENUM_MEMBER_PROPERTY("Importance", plJoltClothImportance, m_Importance),
    PL_ACCESSOR_PROPERTY("MaxDistanceScale", GetMaxDistanceScale, SetMaxDistanceScaleProperty)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 4.0f)),
    PL_ACCESSOR_PROPERTY("WindInfluence", GetWindInfluence, SetWindInfluence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("LinearInertia", m_fLinearInertia)->AddAttributes(new plDefaultValueAttribute(0.7f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AngularInertia", m_fAngularInertia)->AddAttributes(new plDefaultValueAttribute(0.7f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ENUM_MEMBER_PROPERTY("WorldCollision", plJoltClothWorldCollisionMode, m_WorldCollision),
    PL_MEMBER_PROPERTY("CollisionLayer", m_uiCollisionLayer)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
    PL_MEMBER_PROPERTY("TeleportDistance", m_fTeleportDistance)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.1f, 1000.0f)),
    PL_ENUM_MEMBER_PROPERTY("TeleportMode", plJoltClothTeleportMode, m_TeleportMode),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgUpdateLocalBounds, OnMsgUpdateLocalBounds),
    PL_MESSAGE_HANDLER(plMsgAnimationPoseUpdated, OnAnimationPoseUpdated),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Reset),
    PL_SCRIPT_FUNCTION_PROPERTY(SetSimulate, In, "bSimulate", In, "BlendTime"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetMaxDistanceScale, In, "fScale", In, "BlendTime"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetWindInfluence, In, "fInfluence"),
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
  constexpr float s_fLongestTimeStep = 1.0f / 15.0f;
  constexpr float s_fBoundsMargin = 0.1f;
  constexpr float s_fLodBlendTime = 0.3f; // seconds a garment takes to fade between simulated and skinned

  /// Moves fCurrent towards fTarget by fSpeed per second. A speed of zero jumps.
  float Approach(float fCurrent, float fTarget, float fSpeed, float fTimeStep)
  {
    if (fSpeed <= 0.0f)
      return fTarget;

    const float fStep = fSpeed * fTimeStep;
    return fCurrent < fTarget ? plMath::Min(fCurrent + fStep, fTarget) : plMath::Max(fCurrent - fStep, fTarget);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

plJoltCharacterClothComponentManager::plJoltCharacterClothComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

plJoltCharacterClothComponentManager::~plJoltCharacterClothComponentManager() = default;

void plJoltCharacterClothComponentManager::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltCharacterClothComponentManager::Update, this);
  desc.m_Phase = plWorldUpdatePhase::PostAsync;
  desc.m_bOnlyUpdateWhenSimulating = true;
  desc.m_fPriority = -1000.0f; // after physics results are fetched and after animation has set the pose of the frame

  RegisterUpdateFunction(desc);
}

void plJoltCharacterClothComponentManager::SetLodReferencePoint(const plVec3& vPosition)
{
  m_vLodReferencePoint = vPosition;
  m_bHasLodReferencePoint = true;
}

void plJoltCharacterClothComponentManager::ClearLodReferencePoint()
{
  m_bHasLodReferencePoint = false;
}

plInt32 plJoltCharacterClothComponentManager::GetBudgetForTier(plStringView sQualityPreset)
{
  if (sQualityPreset.IsEqual_NoCase("low"))
    return cvar_JoltClothBudgetLow;
  if (sQualityPreset.IsEqual_NoCase("high"))
    return cvar_JoltClothBudgetHigh;
  if (sQualityPreset.IsEqual_NoCase("max"))
    return cvar_JoltClothBudgetMax;
  if (sQualityPreset.IsEqual_NoCase("mobile"))
    return cvar_JoltClothBudgetMobile;

  // medium, a preset of the project's own, and an application without game settings
  return cvar_JoltClothBudgetMedium;
}

plInt32 plJoltCharacterClothComponentManager::GetBudget()
{
  if (cvar_JoltClothMaxGarments >= 0)
    return cvar_JoltClothMaxGarments;

  // The quality tier changes when the player changes it, which is rare, and asking for it copies the settings:
  // once a second is plenty.
  const plTime now = plTime::Now();

  if (now - m_LastTierCheck >= plTime::MakeFromSeconds(1.0) || m_LastTierCheck.IsZero())
  {
    m_LastTierCheck = now;
    m_sQualityTier = "medium";

    if (plGameApplication* pApplication = plGameApplication::GetGameApplicationInstance())
    {
      const plVariantDictionary settings = pApplication->GetGameSettings().GetEffectiveSettings();

      if (const plVariant* pPreset = settings.GetValue("Quality.Preset"); pPreset != nullptr && pPreset->IsA<plString>())
        m_sQualityTier = pPreset->Get<plString>();
    }
  }

  return GetBudgetForTier(m_sQualityTier);
}

void plJoltCharacterClothComponentManager::UpdateLod()
{
  // where the garments are looked at from
  plHybridArray<plVec3, 4> cameras;
  bool bCheckVisibility = false;

  if (m_bHasLodReferencePoint)
  {
    cameras.PushBack(m_vLodReferencePoint);
  }
  else
  {
    for (const plViewHandle& hView : plRenderWorld::GetMainViews())
    {
      plView* pView = nullptr;
      if (plRenderWorld::TryGetView(hView, pView) && pView->GetWorld() == GetWorld() && pView->GetLodCamera() != nullptr)
        cameras.PushBack(pView->GetLodCamera()->GetCenterPosition());
    }

    bCheckVisibility = !cameras.IsEmpty();
  }

  m_LodCandidates.Clear();

  for (auto it = GetComponents(); it.IsValid(); ++it)
  {
    if (!it->IsActiveAndSimulating())
      continue;

    plJoltCharacterClothComponent* pCloth = it;
    pCloth->m_bLodGranted = false;
    pCloth->m_bLodReduced = false;

    // without any view (a headless run) every garment counts as right in front of the camera
    float fDistance = 0.0f;
    if (!cameras.IsEmpty())
    {
      const plVec3 vPosition = pCloth->GetOwner()->GetGlobalPosition();

      fDistance = plMath::MaxValue<float>();
      for (const plVec3& vCamera : cameras)
      {
        fDistance = plMath::Min(fDistance, (vCamera - vPosition).GetLength());
      }
    }
    pCloth->m_fLodDistance = fDistance;

    if (!pCloth->WantsSimulation())
      continue;

    // a garment that left the screen keeps simulating for about a second, then stops
    if (bCheckVisibility && pCloth->GetOwner()->GetVisibilityState(60) == plVisibilityState::Invisible)
      continue;

    const bool bAlways = pCloth->m_Importance == plJoltClothImportance::Always;

    const plJoltClothSimulationSettings* pSettings = pCloth->m_pData != nullptr ? &pCloth->m_pData->GetDescriptor().m_Simulation : nullptr;
    const float fSimulateDistance = pSettings != nullptr ? pSettings->m_fSimulateDistance : 30.0f;

    if (!bAlways && fDistance > fSimulateDistance)
      continue;

    m_LodCandidates.PushBack(pCloth);
  }

  // the important and the near garments get the budget
  m_LodCandidates.Sort([](const plJoltCharacterClothComponent* pLhs, const plJoltCharacterClothComponent* pRhs)
    {
      if (pLhs->m_Importance != pRhs->m_Importance)
        return pLhs->m_Importance.GetValue() > pRhs->m_Importance.GetValue();

      return pLhs->m_fLodDistance < pRhs->m_fLodDistance; });

  const plInt32 iBudget = GetBudget();
  plInt32 iGranted = 0;

  for (plJoltCharacterClothComponent* pCloth : m_LodCandidates)
  {
    const bool bAlways = pCloth->m_Importance == plJoltClothImportance::Always;

    if (!bAlways)
    {
      if (iBudget >= 0 && iGranted >= iBudget)
        continue;

      ++iGranted;
    }

    const plJoltClothSimulationSettings* pSettings = pCloth->m_pData != nullptr ? &pCloth->m_pData->GetDescriptor().m_Simulation : nullptr;
    const float fFullRateDistance = pSettings != nullptr ? pSettings->m_fFullRateDistance : 14.0f;

    pCloth->m_bLodGranted = true;
    pCloth->m_bLodReduced = !bAlways && pCloth->m_fLodDistance > fFullRateDistance;
  }
}

void plJoltCharacterClothComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  const plTime timeStep = GetWorld()->GetClock().GetTimeDiff();

  UpdateLod();

  m_ToStep.Clear();

  for (auto it = GetComponents(); it.IsValid(); ++it)
  {
    if (it->IsActiveAndSimulating() && it->PrepareStep(timeStep))
      m_ToStep.PushBack(it);
  }

  plStopwatch timer;

  // A step touches only its own soft body and reads the physics system, which is at rest in this phase.
  if (cvar_JoltClothParallel && m_ToStep.GetCount() > 1)
  {
    plParallelForParams params;
    params.m_uiBinSize = 1;

    plTaskSystem::ParallelForIndexed(
      static_cast<plUInt32>(0), m_ToStep.GetCount(),
      [this](plUInt32 uiStart, plUInt32 uiEnd)
      {
        for (plUInt32 i = uiStart; i < uiEnd; ++i)
        {
          m_ToStep[i]->m_Instance.Step(m_ToStep[i]->m_StepInput);
        }
      },
      "CharacterCloth", plTaskNesting::Maybe, params);
  }
  else
  {
    for (plJoltCharacterClothComponent* pCloth : m_ToStep)
    {
      pCloth->m_Instance.Step(pCloth->m_StepInput);
    }
  }

  m_LastSolveTime = timer.GetRunningTotal();
  m_uiNumSimulated = m_ToStep.GetCount();

  plUInt32 uiParticles = 0;
  for (plJoltCharacterClothComponent* pCloth : m_ToStep)
  {
    pCloth->FinishStep();
    uiParticles += pCloth->m_Instance.GetParticleCount();
  }

  // the view of the viewport's menu, unless the garment was given its own
  const plInt32 iGlobalView = cvar_JoltClothView;

  for (auto it = GetComponents(); it.IsValid(); ++it)
  {
    const plInt32 iView = it->m_iDebugView != 0 ? it->m_iDebugView : iGlobalView;

    if (iView != 0 && it->IsActiveAndSimulating())
      it->DrawView(iView);
  }

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
  plStats::SetStat("Physics/Cloth/Garments", m_uiNumSimulated);
  plStats::SetStat("Physics/Cloth/Particles", uiParticles);
  plStats::SetStat("Physics/Cloth/SolveMs", static_cast<float>(m_LastSolveTime.GetMilliseconds()));
#endif
}

//////////////////////////////////////////////////////////////////////////

plJoltCharacterClothComponent::plJoltCharacterClothComponent() = default;
plJoltCharacterClothComponent::~plJoltCharacterClothComponent() = default;

void plJoltCharacterClothComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_hCloth;
  s << m_bSimulate;
  s << m_Importance;
  s << m_fMaxDistanceScaleTarget;
  s << m_fWindInfluence;
  s << m_fLinearInertia;
  s << m_fAngularInertia;
  s << m_fGravityFactor;
  s << m_WorldCollision;
  s << m_uiCollisionLayer;
  s << m_fTeleportDistance;
  s << m_TeleportMode;
}

void plJoltCharacterClothComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_hCloth;
  s >> m_bSimulate;
  s >> m_Importance;
  s >> m_fMaxDistanceScaleTarget;
  s >> m_fWindInfluence;
  s >> m_fLinearInertia;
  s >> m_fAngularInertia;
  s >> m_fGravityFactor;
  s >> m_WorldCollision;
  s >> m_uiCollisionLayer;
  s >> m_fTeleportDistance;
  s >> m_TeleportMode;

  m_fMaxDistanceScale = m_fMaxDistanceScaleTarget;
  m_fSimulateBlend = m_bSimulate ? 1.0f : 0.0f;
}

void plJoltCharacterClothComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();
  m_uiObjectFilterID = pModule->CreateObjectFilterID();

  m_fMaxDistanceScale = m_fMaxDistanceScaleTarget;
  m_fSimulateBlend = m_bSimulate ? 1.0f : 0.0f;
  m_bResetPending = true;
}

void plJoltCharacterClothComponent::OnDeactivated()
{
  ClearClothOnMesh();
  ReleaseInstance();
  m_pData = nullptr;

  if (m_uiObjectFilterID != plInvalidIndex)
  {
    if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
      pModule->DeleteObjectFilterID(m_uiObjectFilterID);

    m_uiObjectFilterID = plInvalidIndex;
  }

  m_State = plJoltClothState::Inactive;

  SUPER::OnDeactivated();
}

void plJoltCharacterClothComponent::SetCloth(const plJoltClothResourceHandle& hCloth)
{
  if (m_hCloth == hCloth)
    return;

  m_hCloth = hCloth;

  // the next step builds from the new asset
  ClearClothOnMesh();
  ReleaseInstance();
  m_pData = nullptr;
}

void plJoltCharacterClothComponent::SetSimulate(bool bSimulate, plTime blendTime)
{
  m_bSimulate = bSimulate;
  m_fSimulateBlendSpeed = blendTime.IsPositive() ? 1.0f / blendTime.AsFloatInSeconds() : 0.0f;
}

void plJoltCharacterClothComponent::SetMaxDistanceScale(float fScale, plTime blendTime)
{
  m_fMaxDistanceScaleTarget = plMath::Max(0.0f, fScale);

  const float fChange = plMath::Abs(m_fMaxDistanceScaleTarget - m_fMaxDistanceScale);
  m_fMaxDistanceScaleSpeed = blendTime.IsPositive() ? fChange / blendTime.AsFloatInSeconds() : 0.0f;
}

void plJoltCharacterClothComponent::Reset()
{
  m_bResetPending = true;
}

void plJoltCharacterClothComponent::SetPose(plArrayPtr<const plMat4> skinTransforms, const plMat4& mSkinSpaceToOwner)
{
  m_SkinTransforms = skinTransforms;
  m_mExternalSkinSpaceToOwner = mSkinSpaceToOwner;
  m_bHasExternalPose = true;
}

plJoltClothWorldCollision::Enum plJoltCharacterClothComponent::GetEffectiveWorldCollision() const
{
  switch (m_WorldCollision)
  {
    case plJoltClothWorldCollisionMode::Off:
      return plJoltClothWorldCollision::None;

    case plJoltClothWorldCollisionMode::Static:
      return plJoltClothWorldCollision::Static;

    case plJoltClothWorldCollisionMode::All:
      return plJoltClothWorldCollision::All;

    default:
      break;
  }

  if (m_pData == nullptr)
    return plJoltClothWorldCollision::None;

  return static_cast<plJoltClothWorldCollision::Enum>(m_pData->GetDescriptor().m_Simulation.m_WorldCollision.GetValue());
}

bool plJoltCharacterClothComponent::AcquireData()
{
  plResourceLock<plJoltClothResource> pCloth(m_hCloth, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pCloth.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  // a reloaded asset replaces its data: rebuild from the new one
  if (m_pData == nullptr || m_uiResourceChangeCounter != pCloth->GetCurrentResourceChangeCounter())
  {
    ReleaseInstance();

    m_pData = pCloth->GetData();
    m_uiResourceChangeCounter = pCloth->GetCurrentResourceChangeCounter();
    m_bResetPending = true;

    // the new data may have other colliders: look their joints up again
    m_pColliderSkeleton = nullptr;
    m_bHasColliderJointPoses = false;
  }

  return m_pData != nullptr;
}

void plJoltCharacterClothComponent::OnAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg)
{
  // Only the joints of the body colliders are taken from the pose. The garment itself follows the skinning matrices
  // of the mesh, which the mesh component builds from the same pose.
  if (m_pData == nullptr || ref_msg.m_pSkeleton == nullptr)
    return;

  const plDynamicArray<plJoltClothCollider>& colliders = m_pData->GetDescriptor().m_Colliders;
  if (colliders.IsEmpty())
    return;

  if (m_pColliderSkeleton != ref_msg.m_pSkeleton || m_ColliderJoints.GetCount() != colliders.GetCount())
  {
    m_pColliderSkeleton = ref_msg.m_pSkeleton;
    m_ColliderJoints.SetCountUninitialized(colliders.GetCount());

    for (plUInt32 c = 0; c < colliders.GetCount(); ++c)
    {
      m_ColliderJoints[c] = ref_msg.m_pSkeleton->FindJointByName(colliders[c].m_sJoint);

      if (m_ColliderJoints[c] == plInvalidJointIndex)
        plLog::Warning("Cloth collider {} follows joint '{}', which the skeleton does not have.", c, colliders[c].m_sJoint.GetView());
    }
  }

  m_ColliderJointPoses.SetCountUninitialized(colliders.GetCount());

  for (plUInt32 c = 0; c < colliders.GetCount(); ++c)
  {
    const plUInt16 uiJoint = m_ColliderJoints[c];
    m_ColliderJointPoses[c] = uiJoint < ref_msg.m_ModelTransforms.GetCount() ? ref_msg.m_ModelTransforms[uiJoint] : plMat4::MakeIdentity();
  }

  m_bHasColliderJointPoses = true;
}

bool plJoltCharacterClothComponent::GatherPose(plMat4& out_mSkinSpaceToOwner)
{
  plAnimatedMeshComponent* pMesh = nullptr;
  if (GetOwner()->TryGetComponentOfBaseType(pMesh))
  {
    const plArrayPtr<const plShaderTransform> skinning = pMesh->GetSkinningTransforms();
    if (skinning.IsEmpty())
      return false;

    m_SkinTransforms.SetCountUninitialized(skinning.GetCount());
    for (plUInt32 b = 0; b < skinning.GetCount(); ++b)
    {
      m_SkinTransforms[b] = skinning[b].GetAsMat4();
    }

    out_mSkinSpaceToOwner = pMesh->GetRootTransform().GetAsMat4();
    m_hMeshComponent = pMesh->GetHandle();
    return true;
  }

  if (m_bHasExternalPose && !m_SkinTransforms.IsEmpty())
  {
    out_mSkinSpaceToOwner = m_mExternalSkinSpaceToOwner;
    return true;
  }

  return false;
}

void plJoltCharacterClothComponent::SetColliderJointTransforms(plArrayPtr<const plTransform> jointTransforms)
{
  m_ColliderTransforms = jointTransforms;
  m_bHasExternalColliderPose = true;
}

void plJoltCharacterClothComponent::ResolveColliderBones()
{
  const plDynamicArray<plJoltClothCollider>& colliders = m_pData->GetDescriptor().m_Colliders;

  m_ColliderBones.Clear();
  m_ColliderBones.SetCount(colliders.GetCount());

  plAnimatedMeshComponent* pMesh = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType(pMesh) || !pMesh->GetMesh().IsValid())
    return;

  // The render vertices are bound by index, so the cloth only fits the mesh it was made for.
  const plString& sClothMesh = m_pData->GetDescriptor().m_sMesh;
  if (!sClothMesh.IsEmpty() && sClothMesh != pMesh->GetMesh().GetResourceID())
    plLog::Warning("The cloth on '{}' was made for the mesh '{}', but the object draws '{}'. The garments stay skinned unless both meshes have the same vertices.", GetOwner()->GetName(), sClothMesh, pMesh->GetMesh().GetResourceID());

  if (colliders.IsEmpty())
    return;

  plResourceLock<plMeshResource> pMeshResource(pMesh->GetMesh(), plResourceAcquireMode::BlockTillLoaded);
  if (pMeshResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  for (plUInt32 c = 0; c < colliders.GetCount(); ++c)
  {
    plMeshResourceDescriptor::BoneData bone;
    // a joint the mesh is not skinned to is still followed when the frame's skeleton pose has it
    if (!pMeshResource->m_Bones.TryGetValue(colliders[c].m_sJoint, bone))
      continue;

    m_ColliderBones[c].m_uiBone = bone.m_uiBoneIndex;
    m_ColliderBones[c].m_mBindPose = bone.m_GlobalInverseRestPoseMatrix.GetInverse();
  }
}

void plJoltCharacterClothComponent::UpdateColliders(const plMat4& mSkinSpaceToWorld)
{
  if (m_Instance.GetNumColliders() == 0)
    return;

  if (!m_bHasExternalColliderPose)
  {
    m_ColliderTransforms.SetCount(m_ColliderBones.GetCount());

    const bool bUseJointPoses = m_bHasColliderJointPoses && m_ColliderJointPoses.GetCount() == m_ColliderBones.GetCount();

    for (plUInt32 c = 0; c < m_ColliderBones.GetCount(); ++c)
    {
      const ColliderBone& bone = m_ColliderBones[c];

      plMat4 mJoint;

      if (bUseJointPoses && m_ColliderJoints[c] != plInvalidJointIndex)
      {
        mJoint = m_ColliderJointPoses[c];
      }
      else if (bone.m_uiBone < m_SkinTransforms.GetCount())
      {
        // skinning matrix = joint pose * inverse bind pose, so the joint pose is the skinning matrix * bind pose
        mJoint = m_SkinTransforms[bone.m_uiBone] * bone.m_mBindPose;
      }
      else
      {
        // a collider without a joint stays where it was created, out of the way
        m_ColliderTransforms[c] = plTransform(plVec3(0, 0, -100000.0f));
        continue;
      }

      plMat4 mFull;
      plQuat qRotation;
      plMsgAnimationPoseUpdated::ComputeFullBoneTransform(mSkinSpaceToWorld, mJoint, mFull, qRotation);

      m_ColliderTransforms[c].m_vPosition = mFull.GetTranslationVector();
      m_ColliderTransforms[c].m_qRotation = qRotation;
      m_ColliderTransforms[c].m_vScale.Set(1.0f);
    }
  }

  m_Instance.SetColliderJointTransforms(m_ColliderTransforms);
}

bool plJoltCharacterClothComponent::WantsSimulation() const
{
  // a garment that is still fading out keeps its place until the fade is over
  return cvar_JoltClothEnable && m_hCloth.IsValid() && (m_bSimulate || m_fSimulateBlend > 0.0f);
}

void plJoltCharacterClothComponent::ReleaseInstance()
{
  m_Instance.Deinitialize();
  m_pSetup = nullptr;
}

void plJoltCharacterClothComponent::ClearClothOnMesh()
{
  if (m_hMeshComponent.IsInvalidated())
    return;

  plAnimatedMeshComponent* pMesh = nullptr;
  if (GetWorld()->TryGetComponent(m_hMeshComponent, pMesh))
    plComputeSkinningPool::SetClothForFrame(pMesh, nullptr, {}, 0.0f);

  m_hMeshComponent.Invalidate();
}

bool plJoltCharacterClothComponent::PrepareStep(plTime timeStep)
{
  const plJoltClothState stateBefore = m_State;
  m_State = plJoltClothState::Inactive;

  if (!cvar_JoltClothEnable || !m_hCloth.IsValid())
  {
    ClearClothOnMesh();
    return false;
  }

  // A device that cannot deform meshes in compute cannot draw the result. Without any device (a server, a test) the
  // cloth still simulates, for whoever reads the particles.
  if (plGALDevice::HasDefaultDevice() && !plComputeSkinningPool::IsDeformationSupported())
  {
    static bool s_bWarned = false;
    if (!s_bWarned)
    {
      s_bWarned = true;
      plLog::Warning("Character cloth needs compute mesh deformation, which this device does not support. Garments are drawn skinned.");
    }
    return false;
  }

  if (!AcquireData())
    return false;

  // A paused world keeps its cloth as it is, and its debug views go on showing it. A garment that has not started
  // yet (new, or rebuilt after its asset changed) takes one step from the pose, so that it shows as cloth.
  float fTimeStep = plMath::Min(timeStep.AsFloatInSeconds(), s_fLongestTimeStep);
  if (fTimeStep <= 0.0f)
  {
    if (m_Instance.IsInitialized() && !m_bResetPending)
    {
      m_State = stateBefore;
      return false;
    }

    fTimeStep = 1.0f / 60.0f;
  }

  m_fSimulateBlend = Approach(m_fSimulateBlend, m_bSimulate ? 1.0f : 0.0f, m_fSimulateBlendSpeed, fTimeStep);
  m_fLodBlend = Approach(m_fLodBlend, m_bLodGranted ? 1.0f : 0.0f, 1.0f / s_fLodBlendTime, fTimeStep);
  m_fMaxDistanceScale = Approach(m_fMaxDistanceScale, m_fMaxDistanceScaleTarget, m_fMaxDistanceScaleSpeed, fTimeStep);

  const float fSwitchedOn = m_fSimulateBlend * m_fLodBlend;
  if (fSwitchedOn <= 0.0f)
  {
    // switched off or out of budget: plain skinning, and a fresh start from the pose when it comes back
    m_State = plJoltClothState::Skinned;
    ClearClothOnMesh();
    m_bResetPending = true;
    m_fSkippedTime = 0.0f;
    return false;
  }

  // At reduced rate a garment steps every other frame, over the time of both. Garments alternate, so half of them
  // step in any one frame.
  const bool bBlending = fSwitchedOn < 1.0f || m_fMaxDistanceScale != m_fMaxDistanceScaleTarget;
  m_fSkippedTime += fTimeStep;

  if (m_bLodReduced && !bBlending && !m_bResetPending && m_Instance.IsInitialized())
  {
    const plUInt32 uiPhase = GetWorld()->GetUpdateCounter() + GetHandle().GetInternalID().m_InstanceIndex;
    if ((uiPhase & 1) != 0)
    {
      m_State = plJoltClothState::Reduced;
      return false;
    }
  }

  const float fStepTime = plMath::Min(m_fSkippedTime, s_fLongestTimeStep);
  m_fSkippedTime = 0.0f;

  plMat4 mSkinSpaceToOwner;
  if (!GatherPose(mSkinSpaceToOwner))
    return false;

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const plMat4 mBindToSim = plMat4::MakeScaling(ownerTransform.m_vScale) * mSkinSpaceToOwner;

  const plJoltClothSolverSetup* pSetup = m_pData->GetSolverSetup(mBindToSim);
  if (pSetup == nullptr)
    return false;

  const plUInt16 uiObjectLayer = plJoltCollisionFiltering::ConstructCharacterClothLayer(m_uiCollisionLayer, GetEffectiveWorldCollision());

  if (!m_Instance.IsInitialized() || pSetup != m_pSetup || uiObjectLayer != m_uiObjectLayer)
  {
    plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

    // collider shapes are in the units of the skeleton and grow with the character
    ReleaseInstance();
    if (m_Instance.Initialize(*pModule, m_pData->GetDescriptor(), *pSetup, uiObjectLayer, m_uiObjectFilterID, pSetup->m_fUniformScale).Failed())
      return false;

    m_pSetup = pSetup;
    m_uiObjectLayer = uiObjectLayer;
    m_bResetPending = true;

    ResolveColliderBones();
  }

  UpdateColliders(ownerTransform.GetAsMat4() * mSkinSpaceToOwner);

  plVec3 vWind = plVec3::MakeZero();
  if (m_fWindInfluence > 0.0f)
  {
    if (const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
      vWind = pWind->GetWindAt(ownerTransform.m_vPosition + plVec3(0, 0, 1)) * m_fWindInfluence;
  }

  m_StepInput.m_vOwnerPosition = ownerTransform.m_vPosition;
  m_StepInput.m_qOwnerRotation = ownerTransform.m_qRotation;
  m_StepInput.m_SkinTransforms = m_SkinTransforms;
  m_StepInput.m_fTimeStep = fStepTime;
  m_StepInput.m_vWind = vWind;
  m_StepInput.m_fMaxDistanceScale = m_fMaxDistanceScale * fSwitchedOn;
  m_StepInput.m_fLinearInertia = m_fLinearInertia;
  m_StepInput.m_fAngularInertia = m_fAngularInertia;
  m_StepInput.m_fGravityFactor = m_fGravityFactor;
  m_StepInput.m_fTeleportDistance = m_fTeleportDistance;
  m_StepInput.m_TeleportMode = m_TeleportMode;
  m_StepInput.m_bReset = m_bResetPending;

  m_bResetPending = false;

  if (bBlending)
    m_State = plJoltClothState::Blending;
  else
    m_State = m_bLodReduced ? plJoltClothState::Reduced : plJoltClothState::Full;

  return true;
}

void plJoltCharacterClothComponent::FinishStep()
{
  // hand the particles to the mesh that draws the garment
  plAnimatedMeshComponent* pMesh = nullptr;
  if (!m_hMeshComponent.IsInvalidated() && GetWorld()->TryGetComponent(m_hMeshComponent, pMesh))
  {
    if (plSharedPtr<plClothDeformation> pDeformation = m_pData->GetDeformation(m_hCloth.GetResourceID()))
    {
      m_Instance.GetRenderParticles(m_Particles);
      plComputeSkinningPool::SetClothForFrame(pMesh, pDeformation, m_Particles, 1.0f);
    }
  }

  // The cloth can leave the bounds of the skinned mesh. Simulation space has the owner's position as origin and the
  // world's axes, so only the owner's rotation and scale lie between it and the owner's space.
  const plBoundingBox simBounds = m_Instance.GetSimBounds();
  if (!simBounds.IsValid())
    return;

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const plMat4 mSimToOwner = (ownerTransform.m_qRotation.GetAsMat4() * plMat4::MakeScaling(ownerTransform.m_vScale)).GetInverse();

  plBoundingBox localBounds = simBounds;
  localBounds.TransformFromOrigin(mSimToOwner);

  if (!m_LocalBounds.IsValid() || !m_LocalBounds.Contains(localBounds))
  {
    localBounds.Grow(plVec3(s_fBoundsMargin));
    m_LocalBounds = localBounds;

    GetOwner()->UpdateLocalBounds();
  }
}

void plJoltCharacterClothComponent::OnMsgUpdateLocalBounds(plMsgUpdateLocalBounds& ref_msg)
{
  if (m_LocalBounds.IsValid())
    ref_msg.AddBounds(plBoundingBoxSphere::MakeFromBox(m_LocalBounds), plDefaultSpatialDataCategories::RenderDynamic);
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltCharacterClothComponent);
