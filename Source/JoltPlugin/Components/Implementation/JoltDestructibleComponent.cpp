#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Physics/SurfaceResource.h>
#include <Core/ResourceManager/Implementation/ResourceHandleReflection.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <GameEngine/Messages/DamageMessage.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <JoltPlugin/Components/JoltDestructibleComponent.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

plCVarBool cvar_JoltDestructibleVis("Jolt.Destructible.DebugVis", false, plCVarFlags::Default, "Debug draw the bond graphs of destructible meshes.");
plCVarBool cvar_JoltDestructibleStressVis("Jolt.Destructible.StressVis", false, plCVarFlags::Default, "Color destructible bonds by gravity stress load (green -> red).");
plCVarInt cvar_JoltDestructibleMaxBreakEffectsPerFrame("Jolt.Destructible.MaxBreakEffectsPerFrame", 32, plCVarFlags::Default, "World-wide cap on break/chip effect spawns per frame, so cascading collapses cannot spawn hundreds of prefabs at once.");

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltDestructibleAnchorUsage, 2)
  PL_ENUM_CONSTANTS(plJoltDestructibleAnchorUsage::FromAsset, plJoltDestructibleAnchorUsage::FreeStanding, plJoltDestructibleAnchorUsage::Static)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

//////////////////////////////////////////////////////////////////////////

plJoltDestructibleComponentManager::plJoltDestructibleComponentManager(plWorld* pWorld)
  : plComponentManager<plJoltDestructibleComponent, plBlockStorageType::FreeList>(pWorld)
{
}

plJoltDestructibleComponentManager::~plJoltDestructibleComponentManager() = default;

void plJoltDestructibleComponentManager::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltDestructibleComponentManager::PostAsyncUpdate, this);
  desc.m_Phase = plWorldUpdatePhase::PostAsync;
  desc.m_bOnlyUpdateWhenSimulating = true;

  RegisterUpdateFunction(desc);
}

void plJoltDestructibleComponentManager::UpdateMovingDestructibles(plTime deltaTime)
{
  PL_PROFILE_SCOPE("UpdateMovingDestructibles");

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  for (plJoltDestructibleComponent* pComponent : m_MovingDestructibles)
  {
    pComponent->SyncAnchoredBodyTransform(pModule, deltaTime);
  }
}

void plJoltDestructibleComponentManager::PostAsyncUpdate(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("UpdateDestructibles");

  m_uiBreakEffectsThisFrame = 0;

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  // body destruction below can change the active set, so iterate over a copy
  plHybridArray<plJoltDestructibleComponent*, 64> activeComponents;
  activeComponents.Reserve(pModule->GetActiveDestructibles().GetCount());

  for (auto it : pModule->GetActiveDestructibles())
  {
    activeComponents.PushBack(it.Key());
  }

  plSet<plJoltDestructibleComponent*> componentsToUpdate;

  for (plJoltDestructibleComponent* pComponent : activeComponents)
  {
    componentsToUpdate.Insert(pComponent);
  }
  for (plJoltDestructibleComponent* pComponent : pModule->GetDestructiblesPutToSleep())
  {
    componentsToUpdate.Insert(pComponent);
  }
  for (plJoltDestructibleComponent* pComponent : m_FadingDestructibles)
  {
    componentsToUpdate.Insert(pComponent);
  }
  for (plJoltDestructibleComponent* pComponent : m_DebrisLodComponents)
  {
    componentsToUpdate.Insert(pComponent);
  }

  for (plJoltDestructibleComponent* pComponent : componentsToUpdate)
  {
    pComponent->RetrieveIslandTransforms();
  }

  for (auto hComponent : m_RequireDamageProcessing)
  {
    plJoltDestructibleComponent* pComponent;
    if (TryGetComponent(hComponent, pComponent) && pComponent->IsActiveAndSimulating())
    {
      pComponent->ProcessDamage(pModule);
    }
  }

  m_RequireDamageProcessing.Clear();

  if (cvar_JoltDestructibleVis || cvar_JoltDestructibleStressVis)
  {
    for (auto it = GetComponents(); it.IsValid(); it.Next())
    {
      if (it->IsActiveAndSimulating())
      {
        it->DebugDraw();
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltDestructibleComponent, 6, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("DestructibleMesh", GetDestructibleMeshFile, SetDestructibleMeshFile)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_DestructibleMesh")),
    PL_RESOURCE_MEMBER_PROPERTY("Surface", m_hSurface)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Surface")),
    PL_ENUM_MEMBER_PROPERTY("AnchorUsage", plJoltDestructibleAnchorUsage, m_AnchorUsage),
    PL_MEMBER_PROPERTY("Toughness", m_fToughness)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.001f, plVariant())),
    PL_MEMBER_PROPERTY("InteriorDamageScale", m_fInteriorDamageScale)->AddAttributes(new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("StressFactor", m_fStressFactor)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("StressSafetyFactor", m_fStressSafetyFactor)->AddAttributes(new plDefaultValueAttribute(1.5f), new plClampValueAttribute(1.05f, plVariant())),
    PL_ACCESSOR_PROPERTY("OnBreakInteraction", GetOnBreakInteraction, SetOnBreakInteraction)->AddAttributes(new plDynamicStringEnumAttribute("SurfaceInteractionTypeEnum"), new plDefaultValueAttribute(plStringView("Break"))),
    PL_ACCESSOR_PROPERTY("OnChipInteraction", GetOnChipInteraction, SetOnChipInteraction)->AddAttributes(new plDynamicStringEnumAttribute("SurfaceInteractionTypeEnum"), new plDefaultValueAttribute(plStringView("Chip"))),
    PL_ACCESSOR_PROPERTY("OnSettleInteraction", GetOnSettleInteraction, SetOnSettleInteraction)->AddAttributes(new plDynamicStringEnumAttribute("SurfaceInteractionTypeEnum")),
    PL_MEMBER_PROPERTY("MaxBreakEffects", m_uiMaxBreakEffects)->AddAttributes(new plDefaultValueAttribute(4)),
    PL_MEMBER_PROPERTY("DamageVisualIntensity", m_fDamageVisualIntensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("DanglingChance", m_fDanglingChance)->AddAttributes(new plDefaultValueAttribute(0.35f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("DanglingBreakForceFactor", m_fDanglingBreakForceFactor)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, plVariant())),
    PL_MEMBER_PROPERTY("DanglingMaxMass", m_fDanglingMaxMass)->AddAttributes(new plDefaultValueAttribute(50.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" kg")),
    PL_MEMBER_PROPERTY("MaxDangling", m_uiMaxDangling)->AddAttributes(new plDefaultValueAttribute(8)),
    PL_MEMBER_PROPERTY("Density", m_fDensity)->AddAttributes(new plDefaultValueAttribute(500.0f), new plClampValueAttribute(1.0f, plVariant()), new plSuffixAttribute(" kg/m^3")),
    PL_MEMBER_PROPERTY("ContactDamageThreshold", m_fContactDamageThreshold),
    PL_MEMBER_PROPERTY("ContactDamageFactor", m_fContactDamageFactor)->AddAttributes(new plDefaultValueAttribute(0.01f)),
    PL_MEMBER_PROPERTY("DamageMsgRadius", m_fDamageMsgRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("DamageMsgFactor", m_fDamageMsgFactor)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("CollisionLayerStatic", m_uiCollisionLayerStatic)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
    PL_MEMBER_PROPERTY("CollisionLayerDynamic", m_uiCollisionLayerDynamic)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
    PL_MEMBER_PROPERTY("MaxDebris", m_uiMaxDebris)->AddAttributes(new plDefaultValueAttribute(64)),
    PL_MEMBER_PROPERTY("DebrisLifetime", m_DebrisLifetime)->AddAttributes(new plDefaultValueAttribute(plTime::MakeFromSeconds(20.0))),
    PL_MEMBER_PROPERTY("DebrisFadeDuration", m_DebrisFadeDuration)->AddAttributes(new plDefaultValueAttribute(plTime::MakeFromSeconds(0.5))),
    PL_MEMBER_PROPERTY("DebrisMergeDelay", m_DebrisMergeDelay)->AddAttributes(new plDefaultValueAttribute(plTime::MakeFromSeconds(2.0))),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Effects"),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
    PL_MESSAGE_HANDLER(plMsgPhysicsDestructibleDamage, OnMsgPhysicsDestructibleDamage),
    PL_MESSAGE_HANDLER(plMsgPhysicsAddImpulse, OnMsgPhysicsAddImpulse),
    PL_MESSAGE_HANDLER(plMsgPhysicContact, OnMsgPhysicContactMsg),
    PL_MESSAGE_HANDLER(plMsgDamage, OnMsgDamage),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(ApplyDamage, In, "vGlobalPosition", In, "fRadius", In, "fDamage", In, "vImpulse"),
  }
  PL_END_FUNCTIONS;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plJoltDestructibleComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();
  s << m_hDestructible;
  s << m_hSurface;
  s << m_AnchorUsage;
  s << m_fToughness;
  s << m_fDensity;
  s << m_fContactDamageThreshold;
  s << m_fContactDamageFactor;
  s << m_fGravityFactor;
  s << m_uiCollisionLayerStatic;
  s << m_uiCollisionLayerDynamic;
  s << m_uiMaxDebris;
  s << m_DebrisLifetime;

  // version 2
  s << m_fInteriorDamageScale;
  s << m_sOnBreakInteraction;

  // version 3
  s << m_fStressFactor;
  s << m_fStressSafetyFactor;
  s << m_fDamageMsgRadius;
  s << m_fDamageMsgFactor;

  // version 4
  s << m_DebrisFadeDuration;

  // version 5
  s << m_DebrisMergeDelay;

  // version 6
  s << m_sOnChipInteraction;
  s << m_sOnSettleInteraction;
  s << m_uiMaxBreakEffects;
  s << m_fDamageVisualIntensity;
  s << m_fDanglingChance;
  s << m_fDanglingBreakForceFactor;
  s << m_fDanglingMaxMass;
  s << m_uiMaxDangling;

  // version 6: runtime damage state, so destruction survives save games. During prefab/asset
  // serialization the graph is not initialized and only the false flag is written.
  const bool bHasDamageState = m_Graph.IsInitialized();
  s << bHasDamageState;

  if (bHasDamageState)
  {
    plUInt32 uiNumNodes = 0;
    for (const auto& node : m_Graph.m_Nodes)
    {
      if (node.m_uiChunkIndex != plInvalidIndex)
      {
        ++uiNumNodes;
      }
    }

    s << uiNumNodes;

    for (const auto& node : m_Graph.m_Nodes)
    {
      if (node.m_uiChunkIndex == plInvalidIndex)
        continue;

      s << node.m_uiChunkIndex;
      s << node.m_uiState;
      s << node.m_fAccumulatedDamage;
    }

    s << m_Graph.m_Bonds.GetCount();

    for (const auto& bond : m_Graph.m_Bonds)
    {
      const plUInt32 uiChunkA = m_Graph.m_Nodes[bond.m_uiNodeA].m_uiChunkIndex;
      const plUInt32 uiChunkB = bond.IsAnchor() ? plDestructibleBond::WorldAnchor : m_Graph.m_Nodes[bond.m_uiNodeB].m_uiChunkIndex;

      s << uiChunkA;
      s << uiChunkB;
      s << bond.m_fHealth;
    }
  }
}

void plJoltDestructibleComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& s = inout_stream.GetStream();
  s >> m_hDestructible;
  s >> m_hSurface;
  s >> m_AnchorUsage;
  s >> m_fToughness;
  s >> m_fDensity;
  s >> m_fContactDamageThreshold;
  s >> m_fContactDamageFactor;
  s >> m_fGravityFactor;
  s >> m_uiCollisionLayerStatic;
  s >> m_uiCollisionLayerDynamic;
  s >> m_uiMaxDebris;
  s >> m_DebrisLifetime;

  if (uiVersion >= 2)
  {
    s >> m_fInteriorDamageScale;
    s >> m_sOnBreakInteraction;
  }

  if (uiVersion >= 3)
  {
    s >> m_fStressFactor;
    s >> m_fStressSafetyFactor;
    s >> m_fDamageMsgRadius;
    s >> m_fDamageMsgFactor;
  }

  if (uiVersion >= 4)
  {
    s >> m_DebrisFadeDuration;
  }

  if (uiVersion >= 5)
  {
    s >> m_DebrisMergeDelay;
  }

  if (uiVersion >= 6)
  {
    s >> m_sOnChipInteraction;
    s >> m_sOnSettleInteraction;
    s >> m_uiMaxBreakEffects;
    s >> m_fDamageVisualIntensity;
    s >> m_fDanglingChance;
    s >> m_fDanglingBreakForceFactor;
    s >> m_fDanglingMaxMass;
    s >> m_uiMaxDangling;

    bool bHasDamageState = false;
    s >> bHasDamageState;

    if (bHasDamageState)
    {
      plUInt32 uiNumNodes = 0;
      s >> uiNumNodes;

      m_PendingRestoreNodes.SetCount(uiNumNodes);
      for (auto& node : m_PendingRestoreNodes)
      {
        s >> node.m_uiChunkIndex;
        s >> node.m_uiState;
        s >> node.m_fAccumulatedDamage;
      }

      plUInt32 uiNumBonds = 0;
      s >> uiNumBonds;

      m_PendingRestoreBonds.SetCount(uiNumBonds);
      for (auto& bond : m_PendingRestoreBonds)
      {
        s >> bond.m_uiChunkA;
        s >> bond.m_uiChunkB;
        s >> bond.m_fHealth;
      }

      m_bPendingRestore = true;
    }
  }
}

void plJoltDestructibleComponent::SetDestructibleMeshFile(const char* szFile)
{
  plJoltDestructibleMeshResourceHandle hResource;

  if (!plStringUtils::IsNullOrEmpty(szFile))
  {
    hResource = plResourceManager::LoadResource<plJoltDestructibleMeshResource>(szFile);
  }

  m_hDestructible = hResource;

  if (IsActiveAndInitialized())
  {
    TriggerLocalBoundsUpdate();
  }
}

const char* plJoltDestructibleComponent::GetDestructibleMeshFile() const
{
  return m_hDestructible.IsValid() ? m_hDestructible.GetResourceID().GetData() : "";
}

void plJoltDestructibleComponent::OnActivated()
{
  SUPER::OnActivated();

  if (m_hDestructible.IsValid())
  {
    plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::BlockTillLoaded_NeverFail);

    if (pResource.GetAcquireResult() == plResourceAcquireResult::Final)
    {
      m_Bounds = pResource->GetDescriptor().m_Bounds;
      TriggerLocalBoundsUpdate();
    }
  }
}

void plJoltDestructibleComponent::OnDeactivated()
{
  Cleanup();

  SUPER::OnDeactivated();
}

void plJoltDestructibleComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();
  Initialize(pModule);
}

plResult plJoltDestructibleComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  if (m_Bounds.IsValid())
  {
    ref_bounds = m_Bounds;
    return PL_SUCCESS;
  }

  return PL_FAILURE;
}

void plJoltDestructibleComponent::ApplyDamage(const plVec3& vGlobalPosition, float fRadius, float fDamage, const plVec3& vImpulse)
{
  DamageEvent event;
  event.m_vGlobalPosition = vGlobalPosition;
  event.m_fRadius = fRadius;
  event.m_fDamage = fDamage;
  event.m_vImpulse = vImpulse;

  QueueDamageEvent(event);
}

void plJoltDestructibleComponent::QueueDamageEvent(const DamageEvent& event)
{
  if (!IsActiveAndSimulating() || event.m_fDamage <= 0.0f || event.m_fRadius <= 0.0f)
    return;

  m_DamageEvents.PushBack(event);

  static_cast<plJoltDestructibleComponentManager*>(GetOwningManager())->m_RequireDamageProcessing.Insert(GetHandle());
}

void plJoltDestructibleComponent::OnMsgPhysicsDestructibleDamage(plMsgPhysicsDestructibleDamage& ref_msg)
{
  DamageEvent event;
  event.m_vGlobalPosition = ref_msg.m_vGlobalPosition;
  event.m_fRadius = ref_msg.m_fRadius;
  event.m_fDamage = ref_msg.m_fDamage;
  event.m_vImpulse = ref_msg.m_vImpulse;

  QueueDamageEvent(event);
}

void plJoltDestructibleComponent::OnMsgPhysicsAddImpulse(plMsgPhysicsAddImpulse& ref_msg)
{
  DamageEvent event;
  event.m_vGlobalPosition = ref_msg.m_vGlobalPosition;
  event.m_fRadius = 0.75f;
  event.m_fDamage = ref_msg.m_vImpulse.GetLength() * m_fContactDamageFactor;
  event.m_vImpulse = ref_msg.m_vImpulse;
  event.m_uiChunkIndex = ResolveHitChunk(ref_msg.m_pInternalPhysicsActor, ref_msg.m_pInternalPhysicsShape);

  QueueDamageEvent(event);
}

void plJoltDestructibleComponent::OnMsgPhysicContactMsg(plMsgPhysicContact& ref_msg)
{
  if (m_fContactDamageThreshold <= 0.0f)
    return;

  if (ref_msg.m_fImpactSqr < plMath::Square(m_fContactDamageThreshold))
    return;

  DamageEvent event;
  event.m_vGlobalPosition = ref_msg.m_vGlobalPosition;
  event.m_fRadius = 0.75f;
  event.m_fDamage = plMath::Sqrt(ref_msg.m_fImpactSqr) * m_fContactDamageFactor;
  event.m_vImpulse = plVec3::MakeZero();
  event.m_uiChunkIndex = ResolveHitChunk(ref_msg.m_pInternalPhysicsActor, ref_msg.m_pInternalPhysicsShape);

  QueueDamageEvent(event);
}

plUInt32 plJoltDestructibleComponent::ResolveHitChunk(void* pInternalPhysicsActor, void* pInternalPhysicsShape) const
{
  // The generic physics interface transports the Jolt body and sub-shape IDs as opaque pointers.
  // A null actor means that the sender did not provide backend data (body ID zero is therefore the
  // one case that deliberately falls back to radial damage).
  if (pInternalPhysicsActor == nullptr)
    return plInvalidIndex;

  const plUInt32 uiBodyID = static_cast<plUInt32>(reinterpret_cast<size_t>(pInternalPhysicsActor));
  const plUInt32 uiSubShapeID = static_cast<plUInt32>(reinterpret_cast<size_t>(pInternalPhysicsShape));

  auto resolveCompoundSubShape = [&]() -> plUInt32 {
    const plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
    if (pModule == nullptr)
      return plInvalidIndex;

    JPH::RefConst<JPH::Shape> pShape = pModule->GetJoltSystem()->GetBodyInterface().GetShape(JPH::BodyID(uiBodyID));
    const JPH::Shape* pRootShape = pShape.GetPtr();

    if (pRootShape == nullptr)
      return plInvalidIndex;

    if (pRootShape->GetSubType() == JPH::EShapeSubType::Scaled)
    {
      pRootShape = static_cast<const JPH::ScaledShape*>(pRootShape)->GetInnerShape();
    }

    if (pRootShape->GetType() != JPH::EShapeType::Compound)
      return plInvalidIndex;

    const auto* pCompound = static_cast<const JPH::CompoundShape*>(pRootShape);
    JPH::SubShapeID subShapeId;
    subShapeId.SetValue(uiSubShapeID);
    JPH::SubShapeID remainder;
    const plUInt32 uiSubShapeIndex = pCompound->GetSubShapeIndexFromID(subShapeId, remainder);

    if (uiSubShapeIndex >= pCompound->GetNumSubShapes())
      return plInvalidIndex;

    return pCompound->GetCompoundUserData(uiSubShapeIndex);
  };

  if (uiBodyID == m_uiMergedDebrisBodyID)
  {
    return resolveCompoundSubShape();
  }

  plUInt32 uiGroupId = plInvalidIndex;
  const IslandBody* pIsland = nullptr;

  for (auto it : m_IslandBodies)
  {
    if (it.Value().m_uiBodyID == uiBodyID)
    {
      uiGroupId = it.Key();
      pIsland = &it.Value();
      break;
    }
  }

  if (pIsland == nullptr)
    return plInvalidIndex;

  plUInt32 uiOnlyChunk = plInvalidIndex;
  plUInt32 uiNumGroupNodes = 0;

  for (const auto& node : m_Graph.m_Nodes)
  {
    if (node.m_uiState == plDestructibleGraph::NodeState::Active && node.m_uiGroupId == uiGroupId)
    {
      uiOnlyChunk = node.m_uiChunkIndex;
      ++uiNumGroupNodes;
    }
  }

  if (uiNumGroupNodes == 1)
    return uiOnlyChunk;

  return resolveCompoundSubShape();
}

void plJoltDestructibleComponent::OnMsgDamage(plMsgDamage& ref_msg)
{
  // generic gameplay damage (bullets, melee, explosions routed through plMsgDamage) -> local
  // destruction at the hit point, scaled from gameplay damage numbers to bond-health units
  if (m_fDamageMsgFactor <= 0.0f || m_fDamageMsgRadius <= 0.0f)
    return;

  plVec3 vDir = ref_msg.m_vImpactDirection;
  if (vDir.NormalizeIfNotZero(plVec3::MakeZero()).Failed())
    vDir = plVec3::MakeZero();

  DamageEvent event;
  event.m_vGlobalPosition = ref_msg.m_vGlobalPosition;
  event.m_fRadius = m_fDamageMsgRadius;
  event.m_fDamage = (float)ref_msg.m_fDamage * m_fDamageMsgFactor;
  event.m_vImpulse = vDir * ref_msg.m_fImpulse;

  QueueDamageEvent(event);
}

const plJoltMaterial* plJoltDestructibleComponent::GetPhysicsMaterial() const
{
  if (m_hSurface.IsValid())
  {
    plResourceLock<plSurfaceResource> pSurf(m_hSurface, plResourceAcquireMode::BlockTillLoaded);

    if (pSurf->m_pPhysicsMaterialJolt != nullptr)
    {
      return static_cast<const plJoltMaterial*>(pSurf->m_pPhysicsMaterialJolt);
    }
  }

  return plJoltCore::GetDefaultMaterial();
}

void plJoltDestructibleComponent::SpawnBreakEffects(plArrayPtr<const plDestructibleGraph::BrokenBond> brokenBonds, const plHashedString& sInteraction)
{
  if (brokenBonds.IsEmpty() || sInteraction.IsEmpty() || !m_hSurface.IsValid())
    return;

  // world-wide frame budget: a cascading collapse must not spawn hundreds of prefabs at once
  auto* pManager = static_cast<plJoltDestructibleComponentManager*>(GetOwningManager());
  const plUInt32 uiFrameBudget = (plUInt32)plMath::Max(0, (plInt32)cvar_JoltDestructibleMaxBreakEffectsPerFrame);

  if (pManager->m_uiBreakEffectsThisFrame >= uiFrameBudget)
    return;

  plResourceLock<plSurfaceResource> pSurface(m_hSurface, plResourceAcquireMode::AllowLoadingFallback);

  if (pSurface.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  // largest broken bonds get the effects - big structural breaks warrant the big dust
  plHybridArray<plUInt32, 32> order;
  order.SetCountUninitialized(brokenBonds.GetCount());
  for (plUInt32 i = 0; i < brokenBonds.GetCount(); ++i)
  {
    order[i] = i;
  }

  order.Sort([&](plUInt32 uiLhs, plUInt32 uiRhs) { return brokenBonds[uiLhs].m_fArea > brokenBonds[uiRhs].m_fArea; });

  const plUInt32 uiMaxEffects = plMath::Min<plUInt32>(m_uiMaxBreakEffects, uiFrameBudget - pManager->m_uiBreakEffectsThisFrame);
  plUInt32 uiSpawned = 0;

  for (plUInt32 i = 0; i < order.GetCount() && uiSpawned < uiMaxEffects; ++i)
  {
    const auto& bond = brokenBonds[order[i]];

    plVec3 vNormal = bond.m_vNormal;
    vNormal.NormalizeIfNotZero(plVec3::MakeAxisZ()).IgnoreResult();

    if (pSurface->InteractWithSurface(GetWorld(), GetOwner()->GetHandle(), bond.m_vCentroid, vNormal, -vNormal, sInteraction, &GetOwner()->GetTeamID()))
    {
      ++uiSpawned;
      ++pManager->m_uiBreakEffectsThisFrame;
    }
  }
}

void plJoltDestructibleComponent::SpawnSurfaceInteraction(const plVec3& vGlobalPosition, const plVec3& vNormal, const plHashedString& sInteraction)
{
  if (sInteraction.IsEmpty() || !m_hSurface.IsValid())
    return;

  plResourceLock<plSurfaceResource> pSurface(m_hSurface, plResourceAcquireMode::AllowLoadingFallback);

  if (pSurface.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  pSurface->InteractWithSurface(GetWorld(), GetOwner()->GetHandle(), vGlobalPosition, vNormal, -vNormal, sInteraction, &GetOwner()->GetTeamID());
}

void plJoltDestructibleComponent::Initialize(plJoltWorldModule* pModule)
{
  if (!m_hDestructible.IsValid())
    return;

  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::BlockTillLoaded_NeverFail);

  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
  {
    plLog::Warning("Destructible mesh resource '{}' is not available.", m_hDestructible.GetResourceID());
    return;
  }

  const auto& desc = pResource->GetDescriptor();

  if (m_uiObjectFilterID == plInvalidIndex)
  {
    m_uiObjectFilterID = pModule->CreateObjectFilterID();
  }

  if (m_uiUserDataIndexStatic == plInvalidIndex)
  {
    plJoltUserData* pUserData = nullptr;
    m_uiUserDataIndexStatic = pModule->AllocateUserData(pUserData);
    pUserData->Init(this, plOnJoltContact::SendContactMsg);
  }

  if (m_uiUserDataIndexDynamic == plInvalidIndex)
  {
    plJoltUserData* pUserData = nullptr;
    m_uiUserDataIndexDynamic = pModule->AllocateUserData(pUserData);
    pUserData->Init(this, plOnJoltContact::ImpactReactions);
  }

  // the baked geometry is unscaled; the owner's scale is applied via Jolt ScaledShapes and the
  // per-chunk render transforms (scale changes during play are not supported)
  m_vScale = GetOwner()->GetGlobalScaling();

  plDestructibleGraph::SupportMode::Enum supportMode = plDestructibleGraph::SupportMode::FromAsset;

  switch (m_AnchorUsage)
  {
    case plJoltDestructibleAnchorUsage::FreeStanding:
      supportMode = plDestructibleGraph::SupportMode::FreeStanding;
      break;

    case plJoltDestructibleAnchorUsage::Static:
      // does not need baked anchor bonds - every chunk is world-supported
      supportMode = plDestructibleGraph::SupportMode::AllAnchored;
      break;

    case plJoltDestructibleAnchorUsage::FromAsset:
    default:
    {
      // an asset without any anchor bonds cannot be anchored - treat it as free-standing
      bool bHasAnchors = false;
      for (const auto& bond : desc.m_Bonds)
      {
        if (bond.m_uiChunkB == plDestructibleBond::WorldAnchor)
        {
          bHasAnchors = true;
          break;
        }
      }

      if (!bHasAnchors)
      {
        plLog::Warning("Destructible mesh '{}' has no anchor bonds (check the asset's AnchorMode and the mesh orientation) - treating it as a free-standing dynamic object.", m_hDestructible.GetResourceID());
        supportMode = plDestructibleGraph::SupportMode::FreeStanding;
      }
      break;
    }
  }

  m_Graph.Initialize(desc, m_fDensity, m_fToughness, supportMode, m_fInteriorDamageScale);

  // calibrate the gravity stress solver so a pristine structure is stable but shooting out its
  // support makes the unsupported weight tear away (only meaningful for asset-anchored structures)
  if (m_fStressFactor > 0.0f && supportMode == plDestructibleGraph::SupportMode::FromAsset)
  {
    plDestructibleGraph::StressSettings stress;
    stress.m_vGravity = GetOwner()->GetGlobalRotation().GetInverse() * (pModule->GetObjectGravity() * m_fGravityFactor);
    stress.m_fStressFactor = m_fStressFactor;
    stress.m_fSafetyFactor = m_fStressSafetyFactor;
    m_Graph.CalibrateStress(stress);
  }

  // instance-data slots are keyed by CHUNK index (stable across chunk splits; slot 0 = pristine mesh)
  m_ChunkInstanceOffsets.Clear();
  m_ChunkInstanceOffsets.SetCount(desc.m_Chunks.GetCount());

  m_Bounds = desc.m_Bounds;

  if (m_bPendingRestore)
  {
    ApplyRestoredDamageState(pModule, desc);
  }
  else
  {
    CreateInitialBodies(pModule);
  }

  if (GetOwner()->IsDynamic() && supportMode != plDestructibleGraph::SupportMode::FreeStanding)
  {
    static_cast<plJoltDestructibleComponentManager*>(GetOwningManager())->m_MovingDestructibles.Insert(this);
  }

  TriggerLocalBoundsUpdate();
}

void plJoltDestructibleComponent::Cleanup()
{
  auto* pManager = static_cast<plJoltDestructibleComponentManager*>(GetOwningManager());
  pManager->m_MovingDestructibles.Remove(this);
  pManager->m_FadingDestructibles.Remove(this);
  pManager->m_DebrisLodComponents.Remove(this);

  DestroyAllBodies();

  m_Graph.Clear();
  m_IslandBodies.Clear();
  m_DebrisSpawnOrder.Clear();
  m_DamageEvents.Clear();

  if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    if (m_uiObjectFilterID != plInvalidIndex)
    {
      pModule->DeleteObjectFilterID(m_uiObjectFilterID);
    }

    pModule->DeallocateUserData(m_uiUserDataIndexStatic);
    pModule->DeallocateUserData(m_uiUserDataIndexDynamic);
  }
}

float plJoltDestructibleComponent::GetIslandMass(plArrayPtr<const plUInt32> nodes) const
{
  float fMass = 0.0f;

  for (plUInt32 uiNode : nodes)
  {
    fMass += m_Graph.m_Nodes[uiNode].m_fMass;
  }

  return fMass;
}

JPH::Shape* plJoltDestructibleComponent::BuildIslandShape(plArrayPtr<const plUInt32> nodes, bool bStatic)
{
  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::BlockTillLoaded);

  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  const plJoltUserData* pUserData = &pModule->GetUserData(bStatic ? m_uiUserDataIndexStatic : m_uiUserDataIndexDynamic);
  const plJoltMaterial* pMaterial = GetPhysicsMaterial();

  // the hulls are baked unscaled; apply the owner's scale to the finished shape
  auto applyScale = [&](JPH::Shape* pShape) -> JPH::Shape* {
    if (pShape == nullptr || m_vScale.IsEqual(plVec3(1.0f), 0.0001f))
      return pShape;

    JPH::ScaledShape* pScaled = new JPH::ScaledShape(pShape, plJoltConversionUtils::ToVec3(m_vScale));
    pScaled->AddRef();
    pShape->Release(); // the scaled shape holds its own reference
    return pScaled;
  };

  if (nodes.GetCount() == 1)
  {
    // single chunk: plain convex hull body, no compound overhead
    return applyScale(pResource->InstantiateChunkHull(m_Graph.m_Nodes[nodes[0]].m_uiChunkIndex, reinterpret_cast<plUInt64>(pUserData), pMaterial, m_fDensity));
  }

  // the hulls are cooked in component space, so all compound children sit at the component origin
  JPH::MutableCompoundShapeSettings compound;
  plUInt32 uiNumChildren = 0;

  for (plUInt32 uiNode : nodes)
  {
    JPH::Shape* pHull = pResource->InstantiateChunkHull(m_Graph.m_Nodes[uiNode].m_uiChunkIndex, reinterpret_cast<plUInt64>(pUserData), pMaterial, m_fDensity);

    if (pHull == nullptr)
      continue;

    compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), pHull, m_Graph.m_Nodes[uiNode].m_uiChunkIndex);
    pHull->Release(); // AddShape took a reference
    ++uiNumChildren;
  }

  if (uiNumChildren == 0)
  {
    // an empty compound would enter the broadphase with inside-out bounds
    plLog::Error("Destructible island has no valid collision hulls ('{}').", m_hDestructible.GetResourceID());
    return nullptr;
  }

  auto result = compound.Create();

  if (result.HasError())
  {
    plLog::Error("Failed to create destructible island shape: {}", result.GetError().c_str());
    return nullptr;
  }

  JPH::Shape* pShape = result.Get();

  const JPH::AABox bounds = pShape->GetLocalBounds();
  if (!bounds.IsValid() || bounds.mMin.IsNaN() || bounds.mMax.IsNaN())
  {
    plLog::Error("Destructible island shape has invalid bounds ('{}').", m_hDestructible.GetResourceID());
    return nullptr;
  }

  pShape->AddRef();
  return applyScale(pShape);
}

void plJoltDestructibleComponent::DestroyMergedDebrisProxy(plJoltWorldModule* pModule)
{
  if (m_uiMergedDebrisBodyID == plInvalidIndex)
    return;

  auto& jphBodies = pModule->GetBodyInterface();
  const JPH::BodyID bodyId(m_uiMergedDebrisBodyID);

  if (jphBodies.IsAdded(bodyId))
  {
    jphBodies.RemoveBody(bodyId);
  }
  else
  {
    pModule->RemoveBodyFromQueue(bodyId);
  }

  jphBodies.DestroyBody(bodyId);
  m_uiMergedDebrisBodyID = plInvalidIndex;
}

void plJoltDestructibleComponent::RebuildMergedDebrisProxy(plJoltWorldModule* pModule)
{
  DestroyMergedDebrisProxy(pModule);

  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::BlockTillLoaded);
  const plJoltUserData* pUserData = &pModule->GetUserData(m_uiUserDataIndexStatic);
  const plJoltMaterial* pMaterial = GetPhysicsMaterial();
  const plVec3 vProxyOrigin = GetOwner()->GetGlobalPosition();

  JPH::StaticCompoundShapeSettings compound;
  plUInt32 uiNumChildren = 0;

  for (auto it : m_IslandBodies)
  {
    const IslandBody& island = it.Value();
    if (!island.m_bMerged || island.m_bFading)
      continue;

    plUInt32 uiChunk = plInvalidIndex;
    for (const auto& node : m_Graph.m_Nodes)
    {
      if (node.m_uiState == plDestructibleGraph::NodeState::Active && node.m_uiGroupId == it.Key())
      {
        uiChunk = node.m_uiChunkIndex;
        break;
      }
    }

    if (uiChunk == plInvalidIndex)
      continue;

    JPH::Shape* pShape = pResource->InstantiateChunkHull(uiChunk, reinterpret_cast<plUInt64>(pUserData), pMaterial, m_fDensity);
    if (pShape == nullptr)
      continue;

    if (!m_vScale.IsEqual(plVec3(1.0f), 0.0001f))
    {
      JPH::ScaledShape* pScaled = new JPH::ScaledShape(pShape, plJoltConversionUtils::ToVec3(m_vScale));
      pScaled->AddRef();
      pShape->Release();
      pShape = pScaled;
    }

    compound.AddShape(plJoltConversionUtils::ToVec3(island.m_Transform.m_vPosition - vProxyOrigin), plJoltConversionUtils::ToQuat(island.m_Transform.m_qRotation).Normalized(), pShape, uiChunk);
    pShape->Release(); // AddShape took a reference
    ++uiNumChildren;
  }

  if (uiNumChildren == 0)
    return;

  auto result = compound.Create();
  if (result.HasError())
  {
    plLog::Error("Failed to create consolidated destructible debris shape: {}", result.GetError().c_str());
    return;
  }

  JPH::Shape* pShape = result.Get();
  pShape->AddRef();

  JPH::BodyCreationSettings bodyCfg;
  bodyCfg.SetShape(pShape);
  pShape->Release(); // SetShape took a reference
  bodyCfg.mRestitution = pMaterial->m_fRestitution;
  bodyCfg.mFriction = pMaterial->m_fFriction;
  bodyCfg.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
  bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
  bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserData);
  bodyCfg.mMotionType = JPH::EMotionType::Static;
  bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerStatic, plJoltBroadphaseLayer::Static);
  bodyCfg.mPosition = plJoltConversionUtils::ToVec3(vProxyOrigin);
  bodyCfg.mRotation = JPH::Quat::sIdentity();

  JPH::Body* pBody = pModule->GetBodyInterface().CreateBody(bodyCfg);
  pModule->QueueBodyToAdd(pBody, false);
  m_uiMergedDebrisBodyID = pBody->GetID().GetIndexAndSequenceNumber();
}

plUInt32 plJoltDestructibleComponent::CreateIslandBody(plJoltWorldModule* pModule, JPH::Shape* pShape, const plTransform& transform, const plVec3& vLinearVelocity, const plVec3& vAngularVelocity, float fMass)
{
  auto& jphBodies = pModule->GetBodyInterface();

  const plJoltUserData* pUserDataDynamic = &pModule->GetUserData(m_uiUserDataIndexDynamic);
  const plJoltMaterial* pMaterial = GetPhysicsMaterial();

  JPH::BodyCreationSettings bodyCfg;
  bodyCfg.SetShape(pShape);
  bodyCfg.mRestitution = pMaterial->m_fRestitution;
  bodyCfg.mFriction = pMaterial->m_fFriction;
  bodyCfg.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
  bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
  bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserDataDynamic);
  bodyCfg.mMotionType = JPH::EMotionType::Dynamic;
  bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerDynamic, plJoltBroadphaseLayer::Debris);
  bodyCfg.mGravityFactor = m_fGravityFactor;
  // derive the mass from the baked chunk volumes instead of the shape's mass properties -
  // near-degenerate hulls can otherwise produce singular inertia tensors (-> NaN transforms
  // that corrupt the broadphase); only the inertia is computed from the shape
  bodyCfg.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bodyCfg.mMassPropertiesOverride.mMass = plMath::Max(0.1f, fMass * plMath::Abs(m_vScale.x * m_vScale.y * m_vScale.z));
  bodyCfg.mPosition = plJoltConversionUtils::ToVec3(transform.m_vPosition);
  bodyCfg.mRotation = plJoltConversionUtils::ToQuat(transform.m_qRotation).Normalized();
  bodyCfg.mLinearVelocity = plJoltConversionUtils::ToVec3(vLinearVelocity);
  bodyCfg.mAngularVelocity = plJoltConversionUtils::ToVec3(vAngularVelocity);

  JPH::Body* pBody = jphBodies.CreateBody(bodyCfg);
  pModule->QueueBodyToAdd(pBody, true);

  return pBody->GetID().GetIndexAndSequenceNumber();
}

void plJoltDestructibleComponent::RebuildStaticBody(plJoltWorldModule* pModule)
{
  auto& jphBodies = pModule->GetBodyInterface();

  DestroyIslandBody(jphBodies, plDestructibleGraph::AnchoredGroup);

  plHybridArray<plUInt32, 64> attachedNodes;
  for (plUInt32 n = 0; n < m_Graph.m_Nodes.GetCount(); ++n)
  {
    const auto& node = m_Graph.m_Nodes[n];
    if (node.m_uiState == plDestructibleGraph::NodeState::Active && node.m_uiGroupId == plDestructibleGraph::AnchoredGroup)
    {
      attachedNodes.PushBack(n);
    }
  }

  if (attachedNodes.IsEmpty())
    return;

  JPH::Shape* pShape = BuildIslandShape(attachedNodes, true);

  if (pShape == nullptr)
    return;

  const plJoltUserData* pUserDataStatic = &pModule->GetUserData(m_uiUserDataIndexStatic);
  const plJoltMaterial* pMaterial = GetPhysicsMaterial();
  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();

  JPH::BodyCreationSettings bodyCfg;
  bodyCfg.SetShape(pShape);
  pShape->Release(); // SetShape took a reference
  bodyCfg.mRestitution = pMaterial->m_fRestitution;
  bodyCfg.mFriction = pMaterial->m_fFriction;
  bodyCfg.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
  bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
  bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserDataStatic);
  const bool bMoving = GetOwner()->IsDynamic();
  bodyCfg.mMotionType = bMoving ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static;
  bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerStatic, bMoving ? plJoltBroadphaseLayer::Dynamic : plJoltBroadphaseLayer::Static);
  bodyCfg.mPosition = plJoltConversionUtils::ToVec3(ownerTransform.m_vPosition);
  bodyCfg.mRotation = plJoltConversionUtils::ToQuat(ownerTransform.m_qRotation).Normalized();

  JPH::Body* pBody = jphBodies.CreateBody(bodyCfg);
  pModule->QueueBodyToAdd(pBody, bMoving);

  IslandBody& island = m_IslandBodies[plDestructibleGraph::AnchoredGroup];
  island.m_uiBodyID = pBody->GetID().GetIndexAndSequenceNumber();
  island.m_Transform = ownerTransform;
  island.m_LastRenderTransform = ownerTransform;
  island.m_bSingleChunk = false;
}

void plJoltDestructibleComponent::SyncAnchoredBodyTransform(plJoltWorldModule* pModule, plTime deltaTime)
{
  IslandBody* pStatic = m_IslandBodies.GetValue(plDestructibleGraph::AnchoredGroup);
  if (pStatic == nullptr || pStatic->m_uiBodyID == plInvalidIndex || !deltaTime.IsPositive())
    return;

  GetOwner()->UpdateGlobalTransform();
  plTransform target = GetOwner()->GetGlobalTransform();
  target.m_vScale = m_vScale;

  pModule->GetBodyInterface().MoveKinematic(JPH::BodyID(pStatic->m_uiBodyID), plJoltConversionUtils::ToVec3(target.m_vPosition), plJoltConversionUtils::ToQuat(target.m_qRotation).Normalized(), deltaTime.AsFloatInSeconds());

  pStatic->m_LastRenderTransform = pStatic->m_Transform;
  pStatic->m_Transform = target;
}

void plJoltDestructibleComponent::CreateInitialBodies(plJoltWorldModule* pModule)
{
  if (m_Graph.m_Nodes.IsEmpty())
    return;

  // free-standing objects start with all nodes in group 1
  if (m_Graph.m_Nodes[0].m_uiGroupId != plDestructibleGraph::AnchoredGroup)
  {
    plHybridArray<plUInt32, 64> allNodes;
    for (plUInt32 n = 0; n < m_Graph.m_Nodes.GetCount(); ++n)
    {
      allNodes.PushBack(n);
    }

    JPH::Shape* pShape = BuildIslandShape(allNodes, false);

    if (pShape == nullptr)
      return;

    const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
    const plUInt32 uiBodyID = CreateIslandBody(pModule, pShape, ownerTransform, plVec3::MakeZero(), plVec3::MakeZero(), GetIslandMass(allNodes.GetArrayPtr()));
    pShape->Release();

    IslandBody& island = m_IslandBodies[m_Graph.m_Nodes[0].m_uiGroupId];
    island.m_uiBodyID = uiBodyID;
    island.m_Transform = ownerTransform;
    island.m_LastRenderTransform = ownerTransform;
    island.m_SpawnTime = GetWorld()->GetClock().GetAccumulatedTime();
    island.m_bSingleChunk = false;
  }
  else
  {
    RebuildStaticBody(pModule);
  }
}

void plJoltDestructibleComponent::ApplyRestoredDamageState(plJoltWorldModule* pModule, const plDestructibleMeshDescriptor& desc)
{
  m_bPendingRestore = false;

  // the restore path only supports world-anchored structures; free-standing objects start pristine
  if (m_Graph.m_Nodes.IsEmpty() || m_Graph.m_Nodes[0].m_uiGroupId != plDestructibleGraph::AnchoredGroup)
  {
    if (!m_PendingRestoreNodes.IsEmpty())
    {
      plLog::Warning("Destructible '{}': saved damage state is only restored for anchored structures - starting pristine.", m_hDestructible.GetResourceID());
    }

    m_PendingRestoreNodes.Clear();
    m_PendingRestoreBonds.Clear();
    CreateInitialBodies(pModule);
    return;
  }

  // 1) replay chunk splits so the live node set matches the saved leaf partition
  for (const auto& saved : m_PendingRestoreNodes)
  {
    if (saved.m_uiChunkIndex >= desc.m_Chunks.GetCount())
      continue;

    plUInt32 uiGuard = 0;
    plUInt32 uiNode = m_Graph.ResolveNodeForChunk(desc, saved.m_uiChunkIndex);

    while (uiNode != plInvalidIndex && m_Graph.m_Nodes[uiNode].m_uiChunkIndex != saved.m_uiChunkIndex && uiGuard++ < 64)
    {
      if (!m_Graph.SplitChunkIntoChildren(desc, uiNode, 1.0f))
        break;

      uiNode = m_Graph.ResolveNodeForChunk(desc, saved.m_uiChunkIndex);
    }
  }

  // 2) restore per-node damage and destroyed (despawned debris) state
  for (const auto& saved : m_PendingRestoreNodes)
  {
    const plUInt32 uiNode = m_Graph.GetNodeForChunk(saved.m_uiChunkIndex);

    if (uiNode == plInvalidIndex)
      continue;

    m_Graph.m_Nodes[uiNode].m_fAccumulatedDamage = saved.m_fAccumulatedDamage;

    if (saved.m_uiState == plDestructibleGraph::NodeState::Destroyed)
    {
      m_Graph.DestroyNode(uiNode);
    }
  }

  // 3) restore bond healths, matched by leaf chunk pair
  auto makeChunkPairKey = [](plUInt32 uiChunkA, plUInt32 uiChunkB) -> plUInt64 {
    if (uiChunkA > uiChunkB)
    {
      plMath::Swap(uiChunkA, uiChunkB);
    }
    return ((plUInt64)uiChunkA << 32) | uiChunkB;
  };

  plMap<plUInt64, float> savedBondHealth;

  for (const auto& saved : m_PendingRestoreBonds)
  {
    savedBondHealth[makeChunkPairKey(saved.m_uiChunkA, saved.m_uiChunkB)] = saved.m_fHealth;
  }

  for (auto& bond : m_Graph.m_Bonds)
  {
    const plUInt32 uiChunkA = m_Graph.m_Nodes[bond.m_uiNodeA].m_uiChunkIndex;
    const plUInt32 uiChunkB = bond.IsAnchor() ? plDestructibleBond::WorldAnchor : m_Graph.m_Nodes[bond.m_uiNodeB].m_uiChunkIndex;

    if (const float* pHealth = savedBondHealth.GetValue(makeChunkPairKey(uiChunkA, uiChunkB)))
    {
      bond.m_fHealth = plMath::Min(*pHealth, bond.m_fMaxHealth);
    }
  }

  m_PendingRestoreNodes.Clear();
  m_PendingRestoreBonds.Clear();

  // 4) rebuild bodies: the anchored part from group 0; unsupported regions fall anew from their
  // original spot (dynamic debris transforms are deliberately not persisted)
  plDestructibleGraph::IslandChanges changes;
  m_Graph.MarkAllGroupsDirty();
  m_Graph.RecalculateIslands(changes);

  RebuildStaticBody(pModule);

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const plTime tNow = GetWorld()->GetClock().GetAccumulatedTime();

  for (const auto& newIsland : changes.m_NewIslands)
  {
    JPH::Shape* pShape = BuildIslandShape(newIsland.m_Nodes.GetArrayPtr(), false);

    if (pShape == nullptr)
      continue;

    const plUInt32 uiBodyID = CreateIslandBody(pModule, pShape, ownerTransform, plVec3::MakeZero(), plVec3::MakeZero(), GetIslandMass(newIsland.m_Nodes.GetArrayPtr()));
    pShape->Release();

    IslandBody& island = m_IslandBodies[newIsland.m_uiGroupId];
    island.m_uiBodyID = uiBodyID;
    island.m_Transform = ownerTransform;
    island.m_LastRenderTransform = ownerTransform;
    island.m_SpawnTime = tNow;
    island.m_bSingleChunk = (newIsland.m_Nodes.GetCount() == 1);

    if (island.m_bSingleChunk)
    {
      m_DebrisSpawnOrder.PushBack(newIsland.m_uiGroupId);
    }
  }
}

void plJoltDestructibleComponent::ProcessDamage(plJoltWorldModule* pModule)
{
  if (!m_Graph.IsInitialized() || m_DamageEvents.IsEmpty())
  {
    m_DamageEvents.Clear();
    return;
  }

  PL_PROFILE_SCOPE("DestructibleProcessDamage");

  // keep the anchored group in sync with the owner transform
  if (auto* pStatic = m_IslandBodies.GetValue(plDestructibleGraph::AnchoredGroup))
  {
    pStatic->m_Transform = GetOwner()->GetGlobalTransform();
  }

  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::BlockTillLoaded);
  const auto& desc = pResource->GetDescriptor();

  // apply damage per group, in each group's local (unscaled component) space; the island transforms
  // carry the owner scale, so their inverse maps world positions into the baked geometry's space -
  // the radius has to be scaled down accordingly (uniform scale assumed for radii)
  const float fRadiusScale = plMath::Max(plMath::Max(plMath::Abs(m_vScale.x), plMath::Abs(m_vScale.y)), plMath::Max(plMath::Abs(m_vScale.z), 0.0001f));

  plHybridArray<plDestructibleGraph::BrokenBond, 16> brokenBonds;
  plHybridArray<plDestructibleGraph::BrokenBond, 16> brokenBondsWorld;

  // all bonds broken this frame (world space); effects spawn once at the end, when it is known
  // whether anything actually detached (Break interaction) or the surface merely cracked (Chip)
  plHybridArray<plDestructibleGraph::BrokenBond, 32> frameBrokenBonds;

  for (const auto& event : m_DamageEvents)
  {
    brokenBondsWorld.Clear();

    if (event.m_uiChunkIndex != plInvalidIndex)
    {
      const plUInt32 uiNode = m_Graph.ResolveNodeForChunk(desc, event.m_uiChunkIndex);

      if (uiNode != plInvalidIndex)
      {
        const plUInt32 uiGroupId = m_Graph.m_Nodes[uiNode].m_uiGroupId;
        const IslandBody* pIsland = m_IslandBodies.GetValue(uiGroupId);

        if (pIsland == nullptr || !pIsland->m_bFading)
        {
          const plTransform islandTransform = pIsland != nullptr ? pIsland->m_Transform : GetOwner()->GetGlobalTransform();

          brokenBonds.Clear();
          m_Graph.ApplyChunkDamage(uiNode, event.m_fDamage, &brokenBonds);

          for (const auto& bb : brokenBonds)
          {
            auto& world = brokenBondsWorld.ExpandAndGetRef();
            world.m_vCentroid = islandTransform.TransformPosition(bb.m_vCentroid);
            world.m_vNormal = islandTransform.m_qRotation * bb.m_vNormal;
            world.m_fArea = bb.m_fArea;
          }
        }
      }
    }
    else
    {
      for (auto it : m_IslandBodies)
      {
        if (it.Value().m_bFading)
          continue;

        const plTransform& islandTransform = it.Value().m_Transform;
        const plVec3 vLocalPos = islandTransform.GetInverse().TransformPosition(event.m_vGlobalPosition);

        brokenBonds.Clear();
        m_Graph.ApplyRadialDamage(vLocalPos, event.m_fRadius / fRadiusScale, event.m_fDamage, it.Key(), &brokenBonds);

        for (const auto& bb : brokenBonds)
        {
          auto& world = brokenBondsWorld.ExpandAndGetRef();
          world.m_vCentroid = islandTransform.TransformPosition(bb.m_vCentroid);
          world.m_vNormal = islandTransform.m_qRotation * bb.m_vNormal;
          world.m_fArea = bb.m_fArea;
        }
      }
    }

    frameBrokenBonds.PushBackRange(brokenBondsWorld.GetArrayPtr());
  }

  // chunks that absorbed enough damage split into their children (deeper fracture level)
  if (desc.m_fSplitDamageThreshold > 0.0f)
  {
    const float fSplitThreshold = desc.m_fSplitDamageThreshold * m_fToughness;

    // splits append new nodes; only the pre-existing nodes are candidates this frame
    const plUInt32 uiNumNodes = m_Graph.m_Nodes.GetCount();

    for (plUInt32 n = 0; n < uiNumNodes; ++n)
    {
      const auto& node = m_Graph.m_Nodes[n];

      if (node.m_uiState != plDestructibleGraph::NodeState::Active || node.m_fAccumulatedDamage < fSplitThreshold)
        continue;

      // the more excess damage, the weaker the children start out
      const float fChildHealthScale = plMath::Clamp(1.0f - (node.m_fAccumulatedDamage - fSplitThreshold) / fSplitThreshold, 0.25f, 0.75f);

      m_Graph.SplitChunkIntoChildren(desc, n, fChildHealthScale);
    }
  }

  // gravity stress: unsupported weight (e.g. a pillar whose base was just shot out) tears the
  // surviving bonds; feeds the same island detection so the fallout is emitted below
  if (m_Graph.IsStressActive())
  {
    brokenBonds.Clear();
    if (m_Graph.UpdateStress(&brokenBonds) > 0)
    {
      const plTransform anchoredTransform = GetOwner()->GetGlobalTransform();

      for (const auto& bb : brokenBonds)
      {
        auto& world = frameBrokenBonds.ExpandAndGetRef();
        world.m_vCentroid = anchoredTransform.TransformPosition(bb.m_vCentroid);
        world.m_vNormal = anchoredTransform.m_qRotation * bb.m_vNormal;
        world.m_fArea = bb.m_fArea;
      }
    }
  }

  plDestructibleGraph::IslandChanges changes;
  m_Graph.RecalculateIslands(changes);

  if (changes.IsEmpty())
  {
    // bonds broke but nothing came loose: the surface cracked/chipped where the bonds gave way
    SpawnBreakEffects(frameBrokenBonds, m_sOnChipInteraction);
    m_DamageEvents.Clear();
    return;
  }

  const bool bAnythingDetached = !changes.m_DetachedFromAnchor.IsEmpty() || !changes.m_NewIslands.IsEmpty();

  auto& jphBodies = pModule->GetBodyInterface();

  // capture the state of retired islands before their bodies are destroyed
  struct SourceState
  {
    plTransform m_Transform;
    plUInt32 m_uiBodyID = plInvalidIndex;
  };

  plMap<plUInt32, SourceState> sourceStates;

  {
    SourceState ownerState;
    ownerState.m_Transform = GetOwner()->GetGlobalTransform();

    if (const IslandBody* pAnchoredBody = m_IslandBodies.GetValue(plDestructibleGraph::AnchoredGroup))
    {
      ownerState.m_uiBodyID = pAnchoredBody->m_uiBodyID;
    }

    sourceStates[plDestructibleGraph::AnchoredGroup] = ownerState;
  }

  for (plUInt32 uiRetired : changes.m_RetiredGroups)
  {
    if (const auto* pIsland = m_IslandBodies.GetValue(uiRetired))
    {
      SourceState state;
      state.m_Transform = pIsland->m_Transform;
      state.m_uiBodyID = pIsland->m_uiBodyID;
      sourceStates[uiRetired] = state;
    }
  }

  // rebuild the static body when its chunk composition changed (detachments or splits)
  plBoundingBox affectedBox = plBoundingBox::MakeInvalid();

  if (changes.m_bAnchoredCompositionChanged && changes.m_DetachedFromAnchor.IsEmpty())
  {
    RebuildStaticBody(pModule);
  }

  if (!changes.m_DetachedFromAnchor.IsEmpty())
  {
    RebuildStaticBody(pModule);

    const plTransform ownerTransform = GetOwner()->GetGlobalTransform();

    for (plUInt32 uiNode : changes.m_DetachedFromAnchor)
    {
      const auto& chunkBounds = desc.m_Chunks[m_Graph.m_Nodes[uiNode].m_uiChunkIndex].m_Bounds;
      affectedBox.ExpandToInclude(ownerTransform.TransformPosition(chunkBounds.m_vCenter));
    }

    if (affectedBox.IsValid())
    {
      affectedBox.Grow(plVec3(1.0f));

      plMsgDestructibleChanged msg;
      msg.m_vAffectedMin = affectedBox.m_vMin;
      msg.m_vAffectedMax = affectedBox.m_vMax;
      GetOwner()->PostEventMessage(msg, this, plTime::MakeZero(), plObjectMsgQueueType::PostTransform);
    }
  }

  // create bodies for the new islands
  const plTime tNow = GetWorld()->GetClock().GetAccumulatedTime();

  for (const auto& newIsland : changes.m_NewIslands)
  {
    const SourceState* pSource = sourceStates.GetValue(newIsland.m_uiSourceGroupId);

    plTransform sourceTransform = GetOwner()->GetGlobalTransform();
    plUInt32 uiSourceBodyID = plInvalidIndex;

    if (pSource != nullptr)
    {
      sourceTransform = pSource->m_Transform;
      uiSourceBodyID = pSource->m_uiBodyID;
    }

    JPH::Shape* pShape = BuildIslandShape(newIsland.m_Nodes.GetArrayPtr(), false);

    if (pShape == nullptr)
      continue;

    // transfer the source body's velocity at the new center of mass (v + w x r)
    plVec3 vLinearVelocity = plVec3::MakeZero();
    plVec3 vAngularVelocity = plVec3::MakeZero();

    if (uiSourceBodyID != plInvalidIndex)
    {
      const JPH::BodyID srcBodyId(uiSourceBodyID);

      if (jphBodies.IsAdded(srcBodyId))
      {
        // the shape is already scaled (ScaledShape), so map its COM to world WITHOUT applying
        // the transform's render scale a second time
        const plVec3 vComLocal = plJoltConversionUtils::ToVec3(pShape->GetCenterOfMass());
        const plVec3 vComWorld = sourceTransform.m_vPosition + sourceTransform.m_qRotation * vComLocal;

        vLinearVelocity = plJoltConversionUtils::ToVec3(jphBodies.GetPointVelocity(srcBodyId, plJoltConversionUtils::ToVec3(vComWorld)));
        vAngularVelocity = plJoltConversionUtils::ToVec3(jphBodies.GetAngularVelocity(srcBodyId));
      }
    }

    const float fIslandMass = GetIslandMass(newIsland.m_Nodes.GetArrayPtr());
    const plUInt32 uiBodyID = CreateIslandBody(pModule, pShape, sourceTransform, vLinearVelocity, vAngularVelocity, fIslandMass);
    pShape->Release();

    IslandBody& island = m_IslandBodies[newIsland.m_uiGroupId];
    island.m_uiBodyID = uiBodyID;
    island.m_Transform = sourceTransform;
    island.m_LastRenderTransform = sourceTransform; // no motion on the spawn frame; physics fills in real motion next tick
    island.m_SpawnTime = tNow;
    island.m_bSingleChunk = (newIsland.m_Nodes.GetCount() == 1);

    if (island.m_bSingleChunk)
    {
      m_DebrisSpawnOrder.PushBack(newIsland.m_uiGroupId);
    }

    // dangling debris: pieces breaking off the STATIC structure may keep hanging from their last
    // bond site (ceiling tiles at an angle, fixtures swinging from one corner) until torn off
    if (newIsland.m_uiSourceGroupId == plDestructibleGraph::AnchoredGroup && !GetOwner()->IsDynamic())
    {
      TryCreateTether(pModule, island, newIsland.m_Nodes.GetArrayPtr(), fIslandMass);
    }

    // push new pieces away from the damage source
    for (const auto& event : m_DamageEvents)
    {
      if (!event.m_vImpulse.IsZero(0.001f))
      {
        pModule->AddImpulse(uiBodyID, event.m_vImpulse);
      }
    }
  }

  // destroy the bodies of retired groups (their chunks now live in the new islands)
  bool bRebuildMergedDebris = false;
  for (plUInt32 uiRetired : changes.m_RetiredGroups)
  {
    if (const IslandBody* pIsland = m_IslandBodies.GetValue(uiRetired))
    {
      bRebuildMergedDebris |= pIsland->m_bMerged;
    }
    DestroyIslandBody(jphBodies, uiRetired);
  }

  if (bRebuildMergedDebris)
  {
    RebuildMergedDebrisProxy(pModule);
  }

  // enforce the debris budget: oldest single-chunk debris loses collision immediately and
  // shrinks out visually instead of popping
  while (m_DebrisSpawnOrder.GetCount() > m_uiMaxDebris)
  {
    const plUInt32 uiOldest = m_DebrisSpawnOrder[0];
    BeginDebrisFade(jphBodies, uiOldest, tNow);
  }

  // wake up anything that was resting on geometry that is now gone
  if (affectedBox.IsValid())
  {
    plJoltObjectLayerFilter objectFilter(m_uiCollisionLayerStatic);

    plUInt32 broadphase = plPhysicsShapeType::Default;
    broadphase &= ~plPhysicsShapeType::Static;
    broadphase &= ~plPhysicsShapeType::Character;
    broadphase &= ~plPhysicsShapeType::Query;
    broadphase &= ~plPhysicsShapeType::Trigger;
    plJoltBroadPhaseLayerFilter broadphaseFilter(static_cast<plPhysicsShapeType::Enum>(broadphase));

    const JPH::AABox box = JPH::AABox::sFromTwoPoints(plJoltConversionUtils::ToVec3(affectedBox.m_vMin), plJoltConversionUtils::ToVec3(affectedBox.m_vMax));
    jphBodies.ActivateBodiesInAABox(box, broadphaseFilter, objectFilter);
  }

  SpawnBreakEffects(frameBrokenBonds, bAnythingDetached ? m_sOnBreakInteraction : m_sOnChipInteraction);

  m_DamageEvents.Clear();

  InvalidateCachedRenderData();
  TriggerLocalBoundsUpdate();
}

void plJoltDestructibleComponent::RetrieveIslandTransforms()
{
  bool bHasChangingRenderTransforms = false;
  for (auto it : m_IslandBodies)
  {
    if (it.Key() != plDestructibleGraph::AnchoredGroup && (it.Value().m_bFading || (!it.Value().m_bMerged && it.Value().m_uiBodyID != plInvalidIndex)))
    {
      bHasChangingRenderTransforms = true;
      break;
    }
  }

  if (bHasChangingRenderTransforms && m_uiSleepCounter >= 200)
  {
    // the render data cache is set to 'static' at this point, force a refresh
    InvalidateCachedRenderData();
  }

  if (bHasChangingRenderTransforms)
  {
    m_uiSleepCounter = 0;
  }

  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  auto& jphBodies = pModule->GetBodyInterface();

  const plVec3 vOwnPos = GetOwner()->GetGlobalPosition();
  const plTime tNow = GetWorld()->GetClock().GetAccumulatedTime();

  plBoundingBox bbox = plBoundingBox::MakeInvalid();
  plHybridArray<plUInt32, 8> islandsToFade;
  plHybridArray<plUInt32, 8> islandsToDestroy;
  bool bMergedCompositionChanged = false;
  bool bNeedsDebrisLodUpdate = false;

  for (auto it : m_IslandBodies)
  {
    if (it.Key() == plDestructibleGraph::AnchoredGroup)
      continue;

    IslandBody& island = it.Value();

    if (island.m_bFading)
    {
      const double fDuration = m_DebrisFadeDuration.GetSeconds();
      const float fFade = fDuration > 0.0 ? plMath::Saturate(static_cast<float>((tNow - island.m_FadeStartTime).GetSeconds() / fDuration)) : 1.0f;

      if (fFade >= 1.0f)
      {
        islandsToDestroy.PushBack(it.Key());
        continue;
      }

      // Keep a small non-zero scale because zero-scale transforms are invalid for render instances.
      island.m_LastRenderTransform = island.m_Transform;
      island.m_Transform.m_vScale = m_vScale * plMath::Lerp(1.0f, 0.02f, fFade);
      bbox.ExpandToInclude(island.m_Transform.m_vPosition);
      continue;
    }

    if (island.m_bMerged)
    {
      island.m_LastRenderTransform = island.m_Transform; // frozen: report zero motion

      const bool bTooFar = (island.m_Transform.m_vPosition - vOwnPos).GetLengthSquared() > plMath::Square(60.0f);
      const bool bExpired = m_DebrisLifetime.IsPositive() && (tNow - island.m_SpawnTime) > m_DebrisLifetime;

      if (bTooFar || bExpired)
      {
        islandsToFade.PushBack(it.Key());
      }
      else
      {
        bbox.ExpandToInclude(island.m_Transform.m_vPosition);
        bNeedsDebrisLodUpdate = true;
      }
      continue;
    }

    const JPH::BodyID bodyId(island.m_uiBodyID);

    if (bodyId.IsInvalid())
      continue;

    JPH::RVec3 pos;
    JPH::Quat rot;
    jphBodies.GetPositionAndRotation(bodyId, pos, rot);

    island.m_LastRenderTransform = island.m_Transform;
    island.m_Transform = plJoltConversionUtils::ToTransform(pos, rot);
    island.m_Transform.m_vScale = m_vScale; // chunks render the unscaled baked geometry

    // dangling debris: tear the tether once the constraint impulse exceeds its break threshold
    // (further damage, debris landing on it, or the player pushing it)
    if (island.m_pTether != nullptr)
    {
      const auto* pPoint = static_cast<const JPH::PointConstraint*>(island.m_pTether);

      if (pPoint->GetTotalLambdaPosition().ReduceMax() >= island.m_fTetherBreakThreshold)
      {
        RemoveTether(island, true);
      }
    }

    if (!jphBodies.IsActive(bodyId))
    {
      // Settled bodies must report exactly zero motion, even if this is the last update this island ever
      // receives (once asleep the component drops out of the per-frame update sets and the last/current
      // pair would otherwise stay one tick apart forever -> permanent TAA history rejection).
      island.m_LastRenderTransform = island.m_Transform;
    }

    const plVec3 vPos = plJoltConversionUtils::ToVec3(pos);

    // debris that tunneled through the world and keeps falling is removed
    const bool bTooFar = (vPos - vOwnPos).GetLengthSquared() > plMath::Square(60.0f);
    const bool bExpired = island.m_bSingleChunk && m_DebrisLifetime.IsPositive() && (tNow - island.m_SpawnTime) > m_DebrisLifetime;

    if (bTooFar || bExpired)
    {
      islandsToFade.PushBack(it.Key());
      continue;
    }

    if (island.m_bSingleChunk && m_DebrisMergeDelay.IsPositive() && !jphBodies.IsActive(bodyId))
    {
      if ((tNow - island.m_SpawnTime) >= m_DebrisMergeDelay)
      {
        DestroyIslandPhysicsBody(jphBodies, island);
        island.m_bMerged = true;
        bMergedCompositionChanged = true;

        // settled dust / rubble decal under the frozen debris
        SpawnSurfaceInteraction(island.m_Transform.m_vPosition, plVec3::MakeAxisZ(), m_sOnSettleInteraction);
      }
      else
      {
        // Keep receiving lightweight lifecycle ticks until the merge delay has elapsed.
        bNeedsDebrisLodUpdate = true;
      }
    }

    bbox.ExpandToInclude(vPos);
  }

  for (plUInt32 uiGroupId : islandsToFade)
  {
    BeginDebrisFade(jphBodies, uiGroupId, tNow);
  }

  for (plUInt32 uiGroupId : islandsToDestroy)
  {
    for (plUInt32 n = 0; n < m_Graph.m_Nodes.GetCount(); ++n)
    {
      if (m_Graph.m_Nodes[n].m_uiGroupId == uiGroupId)
      {
        m_Graph.DestroyNode(n);
      }
    }

    m_DebrisSpawnOrder.RemoveAndCopy(uiGroupId);
    DestroyIslandBody(jphBodies, uiGroupId);
  }

  if (bMergedCompositionChanged)
  {
    RebuildMergedDebrisProxy(pModule);
    bNeedsDebrisLodUpdate = true;
  }

  if (bbox.IsValid())
  {
    const plTransform tInv = GetOwner()->GetGlobalTransform().GetInverse();
    bbox.TransformFromOrigin(tInv.GetAsMat4());
    bbox.Grow(plVec3(2.0f));

    plBoundingBoxSphere newBounds = plBoundingBoxSphere::MakeFromBox(bbox);

    if (m_Bounds.IsValid())
    {
      newBounds.ExpandToInclude(m_Bounds);
    }

    m_Bounds = newBounds;
    TriggerLocalBoundsUpdate();
  }

  bool bHasFadingDebris = false;
  for (auto it : m_IslandBodies)
  {
    if (it.Value().m_bFading)
    {
      bHasFadingDebris = true;
      break;
    }
  }

  if (!bHasFadingDebris)
  {
    static_cast<plJoltDestructibleComponentManager*>(GetOwningManager())->m_FadingDestructibles.Remove(this);
  }

  auto* pManager = static_cast<plJoltDestructibleComponentManager*>(GetOwningManager());
  if (bNeedsDebrisLodUpdate)
  {
    pManager->m_DebrisLodComponents.Insert(this);
  }
  else
  {
    pManager->m_DebrisLodComponents.Remove(this);
  }
}

void plJoltDestructibleComponent::BeginDebrisFade(JPH::BodyInterface& jphBodies, plUInt32 uiGroupId, plTime now)
{
  auto it = m_IslandBodies.Find(uiGroupId);

  if (!it.IsValid() || it.Value().m_bFading)
    return;

  m_DebrisSpawnOrder.RemoveAndCopy(uiGroupId);

  if (it.Value().m_bMerged)
  {
    it.Value().m_bMerged = false;
    RebuildMergedDebrisProxy(GetWorld()->GetModule<plJoltWorldModule>());
  }

  if (!m_DebrisFadeDuration.IsPositive())
  {
    for (plUInt32 n = 0; n < m_Graph.m_Nodes.GetCount(); ++n)
    {
      if (m_Graph.m_Nodes[n].m_uiGroupId == uiGroupId)
      {
        m_Graph.DestroyNode(n);
      }
    }

    DestroyIslandBody(jphBodies, uiGroupId);
    return;
  }

  DestroyIslandPhysicsBody(jphBodies, it.Value());
  it.Value().m_bFading = true;
  it.Value().m_FadeStartTime = now;
  m_uiSleepCounter = 0;

  static_cast<plJoltDestructibleComponentManager*>(GetOwningManager())->m_FadingDestructibles.Insert(this);

  InvalidateCachedRenderData();
}

plUInt32 plJoltDestructibleComponent::CountTetheredIslands() const
{
  plUInt32 uiCount = 0;

  for (auto it : m_IslandBodies)
  {
    if (it.Value().m_pTether != nullptr)
    {
      ++uiCount;
    }
  }

  return uiCount;
}

void plJoltDestructibleComponent::TryCreateTether(plJoltWorldModule* pModule, IslandBody& ref_island, plArrayPtr<const plUInt32> nodes, float fIslandMass)
{
  if (m_fDanglingChance <= 0.0f || nodes.IsEmpty() || ref_island.m_uiBodyID == plInvalidIndex)
    return;

  if (fIslandMass > m_fDanglingMaxMass || CountTetheredIslands() >= m_uiMaxDangling)
    return;

  if (GetWorld()->GetRandomNumberGenerator().FloatZeroToOneExclusive() >= m_fDanglingChance)
    return;

  // hang from the LARGEST broken bond that used to connect this island to the still-anchored part
  const plUInt32 uiIslandGroup = m_Graph.m_Nodes[nodes[0]].m_uiGroupId;
  const plDestructibleGraph::Bond* pBestBond = nullptr;

  for (const auto& bond : m_Graph.m_Bonds)
  {
    if (!bond.IsBroken() || bond.IsAnchor())
      continue;

    const auto& nodeA = m_Graph.m_Nodes[bond.m_uiNodeA];
    const auto& nodeB = m_Graph.m_Nodes[bond.m_uiNodeB];

    if (nodeA.m_uiState != plDestructibleGraph::NodeState::Active || nodeB.m_uiState != plDestructibleGraph::NodeState::Active)
      continue;

    const bool bAInIsland = (nodeA.m_uiGroupId == uiIslandGroup);
    const bool bBInIsland = (nodeB.m_uiGroupId == uiIslandGroup);

    if (bAInIsland == bBInIsland)
      continue;

    const auto& anchoredNode = bAInIsland ? nodeB : nodeA;

    if (anchoredNode.m_uiGroupId != plDestructibleGraph::AnchoredGroup)
      continue;

    if (pBestBond == nullptr || bond.m_fArea > pBestBond->m_fArea)
    {
      pBestBond = &bond;
    }
  }

  if (pBestBond == nullptr)
    return;

  const plVec3 vTetherWorld = GetOwner()->GetGlobalTransform().TransformPosition(pBestBond->m_vCentroid);

  JPH::BodyLockWrite bodyLock(pModule->GetJoltSystem()->GetBodyLockInterface(), JPH::BodyID(ref_island.m_uiBodyID));

  if (!bodyLock.Succeeded())
    return;

  // pin the island back to the world at the bond site; sFixedToWorld's COM transform is identity,
  // so LocalToBodyCOM takes the world-space point directly for body 1
  JPH::PointConstraintSettings opt;
  opt.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
  opt.mPoint1 = plJoltConversionUtils::ToVec3(vTetherWorld);
  opt.mPoint2 = bodyLock.GetBody().GetInverseCenterOfMassTransform() * plJoltConversionUtils::ToVec3(vTetherWorld);

  JPH::Constraint* pConstraint = opt.Create(JPH::Body::sFixedToWorld, bodyLock.GetBody());

  if (pConstraint == nullptr)
    return;

  pConstraint->AddRef();
  pModule->GetJoltSystem()->AddConstraint(pConstraint);

  ref_island.m_pTether = pConstraint;
  ref_island.m_fTetherBreakThreshold = plMath::Max(0.01f, fIslandMass * m_fDanglingBreakForceFactor);
}

void plJoltDestructibleComponent::RemoveTether(IslandBody& ref_island, bool bActivateBody)
{
  if (ref_island.m_pTether == nullptr)
    return;

  if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    pModule->GetJoltSystem()->RemoveConstraint(ref_island.m_pTether);

    // wake the body so a torn-off dangler starts falling instead of hanging asleep in the air
    if (bActivateBody && ref_island.m_uiBodyID != plInvalidIndex)
    {
      auto& jphBodies = pModule->GetBodyInterface();
      const JPH::BodyID bodyId(ref_island.m_uiBodyID);

      if (jphBodies.IsAdded(bodyId))
      {
        jphBodies.ActivateBody(bodyId);
      }
    }
  }

  ref_island.m_pTether->Release();
  ref_island.m_pTether = nullptr;
  ref_island.m_fTetherBreakThreshold = 0.0f;
}

void plJoltDestructibleComponent::DestroyIslandPhysicsBody(JPH::BodyInterface& jphBodies, IslandBody& island)
{
  // the constraint must be gone before its body is destroyed
  RemoveTether(island, false);

  const JPH::BodyID bodyId(island.m_uiBodyID);

  if (!bodyId.IsInvalid())
  {
    if (jphBodies.IsAdded(bodyId))
    {
      jphBodies.RemoveBody(bodyId);
    }
    else
    {
      // might still be in the add-queue
      plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
      pModule->RemoveBodyFromQueue(bodyId);
    }

    jphBodies.DestroyBody(bodyId);
    island.m_uiBodyID = plInvalidIndex;
  }
}

void plJoltDestructibleComponent::DestroyIslandBody(JPH::BodyInterface& jphBodies, plUInt32 uiGroupId)
{
  auto it = m_IslandBodies.Find(uiGroupId);

  if (!it.IsValid())
    return;

  DestroyIslandPhysicsBody(jphBodies, it.Value());

  m_IslandBodies.Remove(it);
}

void plJoltDestructibleComponent::DestroyAllBodies()
{
  if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    DestroyMergedDebrisProxy(pModule);

    auto& jphBodies = pModule->GetBodyInterface();

    while (!m_IslandBodies.IsEmpty())
    {
      DestroyIslandBody(jphBodies, m_IslandBodies.GetIterator().Key());
    }
  }

  m_IslandBodies.Clear();
}

void plJoltDestructibleComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!m_hDestructible.IsValid())
    return;

  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::AllowLoadingFallback);

  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  const plMeshResourceHandle& hMesh = pResource->GetRenderMesh();

  if (!hMesh.IsValid())
    return;

  plResourceLock<plMeshResource> pMesh(hMesh, plResourceAcquireMode::AllowLoadingFallback);
  const auto& desc = pResource->GetDescriptor();
  const auto subMeshes = pMesh->GetSubMeshes();
  const auto materials = pMesh->GetMaterials();

  if (m_ChunkInstanceOffsets.GetCount() < desc.m_Chunks.GetCount())
  {
    m_ChunkInstanceOffsets.SetCount(desc.m_Chunks.GetCount());
  }

  const bool bDynamic = GetOwner()->IsDynamic();

  auto emitChunk = [&](plUInt32 uiChunk, const plTransform& transform, const plTransform* pLastTransform, plUInt32 uiInstanceSlot, float fDamage01) {
    const auto& chunk = desc.m_Chunks[uiChunk];

    if (chunk.m_uiNumRenderSubMeshes == 0)
      return;

    // accumulated damage darkens the chunk (dust/soot build-up) so surfaces scar before they break;
    // materials can additionally react to the instance color for crack overlays
    plColor instanceColor = plColor::White;

    if (m_fDamageVisualIntensity > 0.0f && fDamage01 > 0.0f)
    {
      const float fDarken = 1.0f - 0.45f * plMath::Min(1.0f, fDamage01 * m_fDamageVisualIntensity);
      instanceColor = plColor(fDarken, fDarken, fDarken, 1.0f);
    }

    auto hInstanceDataBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, transform, m_ChunkInstanceOffsets[uiInstanceSlot], GetUniqueIdForRendering(), instanceColor, plVec4(0, 1, 0, 1), pLastTransform);

    for (plUInt32 s = 0; s < chunk.m_uiNumRenderSubMeshes; ++s)
    {
      const plUInt32 uiSubMesh = chunk.m_uiFirstRenderSubMesh + s;

      if (uiSubMesh >= subMeshes.GetCount())
        continue;

      const plUInt32 uiMaterialIndex = subMeshes[uiSubMesh].m_uiMaterialIndex;

      plMaterialResourceHandle hMaterial = (uiMaterialIndex < materials.GetCount()) ? materials[uiMaterialIndex] : plMaterialResourceHandle();

      if (!hMaterial.IsValid())
      {
        // meshes with an invalid material are skipped by the renderer entirely
        hMaterial = plResourceManager::LoadResource<plMaterialResource>(plMaterialResource::GetDefaultMaterialFileName(plMaterialResource::DefaultMaterialType::OpenPBR));
      }

      plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
      pRenderData->m_GlobalTransform = transform;
      pRenderData->m_vGlobalPosition = transform.m_vPosition;
      pRenderData->SetFallbackGlobalBounds(GetOwner()->GetGlobalBounds());
      pRenderData->Fill(m_ChunkInstanceOffsets[uiInstanceSlot], hInstanceDataBuffer, hMaterial, hMesh, uiMaterialIndex, uiSubMesh);

      plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial);

      if (m_uiSleepCounter >= 200)
      {
        msg.AddRenderData(pRenderData, category, plRenderData::Caching::IfStatic);
      }
      else
      {
        msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
      }
    }
  };

  if (!m_Graph.IsInitialized())
  {
    // not simulating (e.g. editor edit mode): render the pristine mesh
    emitChunk(0, GetOwner()->GetGlobalTransform(), nullptr, 0, 0.0f);
    return;
  }

  if (m_uiSleepCounter < 200)
  {
    ++m_uiSleepCounter;
  }

  // reference for the damage visualization: the point at which a chunk would split into children,
  // or (without subdivision levels) roughly two fully broken average bonds
  const float fDamageVisualReference = plMath::Max(0.05f, (desc.m_fSplitDamageThreshold > 0.0f ? desc.m_fSplitDamageThreshold : 2.0f) * m_fToughness);

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();

  for (plUInt32 n = 0; n < m_Graph.m_Nodes.GetCount(); ++n)
  {
    const auto& node = m_Graph.m_Nodes[n];

    if (node.m_uiState != plDestructibleGraph::NodeState::Active)
      continue;

    plTransform transform = ownerTransform;
    const plTransform* pLastTransform = nullptr;

    if (const auto* pIsland = m_IslandBodies.GetValue(node.m_uiGroupId))
    {
      transform = pIsland->m_Transform;
      pLastTransform = &pIsland->m_LastRenderTransform;
    }

    const float fDamage01 = plMath::Min(1.0f, node.m_fAccumulatedDamage / fDamageVisualReference);

    emitChunk(node.m_uiChunkIndex, transform, pLastTransform, node.m_uiChunkIndex, fDamage01);
  }
}

void plJoltDestructibleComponent::DebugDraw()
{
  if (!m_Graph.IsInitialized())
    return;

  plDynamicArray<plDebugRenderer::Line> lines;
  lines.Reserve(m_Graph.m_Bonds.GetCount());

  const bool bStressVis = cvar_JoltDestructibleStressVis;

  for (plUInt32 uiBond = 0; uiBond < m_Graph.m_Bonds.GetCount(); ++uiBond)
  {
    const auto& bond = m_Graph.m_Bonds[uiBond];
    const auto& nodeA = m_Graph.m_Nodes[bond.m_uiNodeA];

    if (nodeA.m_uiState != plDestructibleGraph::NodeState::Active)
      continue;

    plTransform transform = GetOwner()->GetGlobalTransform();
    if (const auto* pIsland = m_IslandBodies.GetValue(nodeA.m_uiGroupId))
    {
      transform = pIsland->m_Transform;
    }

    plColor color;

    if (bond.IsBroken())
    {
      continue;
    }

    if (bStressVis)
    {
      // green (unloaded) -> yellow -> red (at/over the failure ratio of 1)
      const float fStress = plMath::Clamp(m_Graph.GetBondStressRatio(uiBond), 0.0f, 1.0f);
      color = plMath::Lerp(plColor::Lime, plColor::Red, fStress);
    }
    else
    {
      const float fHealthRatio = plMath::Clamp(bond.m_fHealth / plMath::Max(0.0001f, bond.m_fMaxHealth), 0.0f, 1.0f);
      color = plMath::Lerp(plColor::Red, bond.IsAnchor() ? plColor::DeepSkyBlue : plColor::Lime, fHealthRatio);
    }

    const plVec3 vCenter = transform.TransformPosition(bond.m_vCentroid);

    if (bond.IsAnchor())
    {
      auto& line = lines.ExpandAndGetRef();
      line.m_start = vCenter;
      line.m_end = vCenter - plVec3(0, 0, 0.25f);
      line.m_startColor = line.m_endColor = color;
    }
    else
    {
      const auto& nodeB = m_Graph.m_Nodes[bond.m_uiNodeB];

      auto& line = lines.ExpandAndGetRef();
      line.m_start = transform.TransformPosition(nodeA.m_vCentroid);
      line.m_end = transform.TransformPosition(nodeB.m_vCentroid);
      line.m_startColor = line.m_endColor = color;
    }
  }

  plDebugRenderer::DrawLines(GetWorld(), lines, plColor::White);
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltDestructibleComponent);
