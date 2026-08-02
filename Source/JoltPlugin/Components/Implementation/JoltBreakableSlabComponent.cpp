#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Physics/SurfaceResource.h>
#include <Core/ResourceManager/Implementation/ResourceHandleReflection.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <GameEngine/Messages/DamageMessage.h>
#include <Foundation/Configuration/CVar.h>
#include <JoltPlugin/Components/JoltBreakableSlabComponent.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <Physics/Collision/Shape/ConvexShape.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

//////////////////////////////////////////////////////////////////////////

PL_BEGIN_STATIC_REFLECTED_BITFLAGS(plJoltBreakableSlabFlags, 1)
  PL_ENUM_CONSTANT(plJoltBreakableSlabFlags::FixedEdgeTop),
    PL_ENUM_CONSTANT(plJoltBreakableSlabFlags::FixedEdgeRight),
    PL_ENUM_CONSTANT(plJoltBreakableSlabFlags::FixedEdgeBottom),
    PL_ENUM_CONSTANT(plJoltBreakableSlabFlags::FixedEdgeLeft),
PL_END_STATIC_REFLECTED_BITFLAGS;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltBreakableShape, 1)
  PL_ENUM_CONSTANT(plJoltBreakableShape::Rectangle),
    PL_ENUM_CONSTANT(plJoltBreakableShape::Triangle),
    PL_ENUM_CONSTANT(plJoltBreakableShape::Circle),
PL_END_STATIC_REFLECTED_ENUM

plAtomicInteger32 plJoltBreakableSlabComponent::s_iShardMeshCounter;

plCVarBool cvar_BreakableSlabVis("Jolt.BreakableSlab.DebugVis", false, plCVarFlags::Default, "Debug draw the state of breakable slabs.");

plJoltBreakableSlabComponentManager::plJoltBreakableSlabComponentManager(plWorld* pWorld)
  : plComponentManager<plJoltBreakableSlabComponent, plBlockStorageType::FreeList>(pWorld)
{
}

plJoltBreakableSlabComponentManager::~plJoltBreakableSlabComponentManager() = default;

void plJoltBreakableSlabComponentManager::Initialize()
{
  SUPER::Initialize();

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltBreakableSlabComponentManager::PreAsyncUpdate, this);
    desc.m_Phase = plWorldUpdatePhase::PreAsync;
    desc.m_bOnlyUpdateWhenSimulating = false;

    RegisterUpdateFunction(desc);
  }

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltBreakableSlabComponentManager::ReinitSlabs, this);
    desc.m_Phase = plWorldUpdatePhase::Async;
    desc.m_bOnlyUpdateWhenSimulating = false;
    desc.m_uiGranularity = 16;

    RegisterUpdateFunction(desc);
  }

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltBreakableSlabComponentManager::PostAsyncUpdate, this);
    desc.m_Phase = plWorldUpdatePhase::PostAsync;
    desc.m_bOnlyUpdateWhenSimulating = false;

    RegisterUpdateFunction(desc);
  }
}

void plJoltBreakableSlabComponentManager::PreAsyncUpdate(const plWorldModule::UpdateContext& context)
{
  m_iTriggerBoundsUpdateSlot = 0;
  m_TriggerBoundsUpdate.SetCount(m_ComponentStorage.GetCount());
}

void plJoltBreakableSlabComponentManager::ReinitSlabs(const plWorldModule::UpdateContext& context)
{
  for (auto it = m_ComponentStorage.GetIterator(context.m_uiFirstComponentIndex, context.m_uiComponentCount); it.IsValid(); ++it)
  {
    plJoltBreakableSlabComponent* pComponent = it;
    if (pComponent->IsActive() && pComponent->m_bReinitMeshes)
    {
      pComponent->ReinitMeshes();

      // we need to call TriggerUpdateBounds() on this component, but we can't do this on just any thread, so queue this for the PostAsync update
      const plInt32 iSlot = m_iTriggerBoundsUpdateSlot.PostIncrement();
      m_TriggerBoundsUpdate[iSlot] = pComponent;
    }
  }
}

void plJoltBreakableSlabComponentManager::PostAsyncUpdate(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("UpdateBreakableSlabs");

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  // when updating slabs, we activate bodies, which would immediately modify the active slabs map
  // and then create a crash during iteration
  // therefore we have to make a copy of the active slabs first
  plHybridArray<plJoltBreakableSlabComponent*, 64> activeSlabs;
  activeSlabs.Reserve(pModule->GetActiveSlabs().GetCount());

  for (auto it : pModule->GetActiveSlabs())
  {
    plJoltBreakableSlabComponent* pComponent = it.Key();
    activeSlabs.PushBack(pComponent);
  }

  for (auto pComponent : activeSlabs)
  {
    pComponent->RetrieveShardTransforms();
  }

  for (plJoltBreakableSlabComponent* pComponent : pModule->GetSlabsPutToSleep())
  {
    pComponent->RetrieveShardTransforms();
  }

  for (auto hComponent : m_RequireBreakage)
  {
    plJoltBreakableSlabComponent* pComponent;
    if (TryGetComponent(hComponent, pComponent))
    {
      if (pComponent->IsActiveAndSimulating() && pComponent->m_pShatterTask == nullptr)
      {
        pComponent->m_pShatterTask = PL_DEFAULT_NEW(plShatterTask);
        pComponent->m_pShatterTask->ConfigureTask("ShatterGlass", plTaskNesting::Never);
        pComponent->m_pShatterTask->m_pComponent = pComponent;
        pComponent->m_pShatterTask->m_ShatterPoints = pComponent->m_ShatterPoints;

        plTaskSystem::StartSingleTask(pComponent->m_pShatterTask, plTaskPriority::In3Frames); // allow some delay, this can take longer than a single frame

        m_RequireBreakUpdate.Insert(hComponent);
      }

      // make sure the shatter points are cleared, even if nothing was done
      // otherwise the component won't be put into m_RequireBreakage again
      pComponent->m_ShatterPoints.Clear();
    }
  }

  m_RequireBreakage.Clear();

  for (auto hComponent : m_RequireBreakUpdate)
  {
    plJoltBreakableSlabComponent* pComponent;
    if (TryGetComponent(hComponent, pComponent) && pComponent->IsActiveAndSimulating() && pComponent->m_pShatterTask != nullptr)
    {
      if (pComponent->m_pShatterTask->IsTaskFinished())
      {
        pComponent->ApplyBreak(pComponent->m_pShatterTask->m_Shapes, pComponent->m_pShatterTask->m_hShardsMesh, pComponent->m_pShatterTask->m_vFinalImpulse);
        pComponent->m_pShatterTask = nullptr;

        m_RequireBreakUpdate.Remove(hComponent);
        break; // sufficient to update one slab per frame
      }
    }
    else
    {
      m_RequireBreakUpdate.Remove(hComponent);
    }
  }

  for (plInt32 i = 0; i < m_iTriggerBoundsUpdateSlot; ++i)
  {
    m_TriggerBoundsUpdate[i]->TriggerLocalBoundsUpdate();
  }

  if (cvar_BreakableSlabVis)
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
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltBreakableSlabComponent, 2, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Width", GetWidth, SetWidth)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 8.0f), new plSuffixAttribute(" m")),
    PL_ACCESSOR_PROPERTY("Height", GetHeight, SetHeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 8.0f), new plSuffixAttribute(" m")),
    PL_ACCESSOR_PROPERTY("Thickness", GetThickness, SetThickness)->AddAttributes(new plDefaultValueAttribute(0.02f), new plClampValueAttribute(0.005f, 1.0f), new plSuffixAttribute(" m")),
    PL_RESOURCE_MEMBER_PROPERTY("Material", m_hMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_RESOURCE_MEMBER_PROPERTY("Surface", m_hSurface)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Surface")),
    PL_ACCESSOR_PROPERTY("UVScale", GetUvScale, SetUvScale)->AddAttributes(new plDefaultValueAttribute(plVec2(1.0f))),
    PL_MEMBER_PROPERTY("CollisionLayerStatic", m_uiCollisionLayerStatic)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
    PL_MEMBER_PROPERTY("CollisionLayerDynamic", m_uiCollisionLayerDynamic)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
    PL_ENUM_ACCESSOR_PROPERTY("Shape", plJoltBreakableShape, GetShape, SetShape),
    PL_BITFLAGS_ACCESSOR_PROPERTY("Flags", plJoltBreakableSlabFlags, GetFlags, SetFlags),
    PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("ContactReportForceThreshold", m_fContactReportForceThreshold),
    PL_MEMBER_PROPERTY("Health", m_fHealth)->AddAttributes(new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("InstantShatterDamage", m_fInstantShatterDamage)->AddAttributes(new plClampValueAttribute(0.0f, plVariant())),
    PL_ACCESSOR_PROPERTY("OnCrackInteraction", GetOnCrackInteraction, SetOnCrackInteraction)->AddAttributes(new plDynamicStringEnumAttribute("SurfaceInteractionTypeEnum"), new plDefaultValueAttribute(plStringView("Crack"))),
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
    PL_MESSAGE_HANDLER(plMsgDamage, OnMsgDamage),
    PL_MESSAGE_HANDLER(plMsgPhysicsAddImpulse, OnMsgPhysicsAddImpulse),
    PL_MESSAGE_HANDLER(plMsgPhysicContact, OnMsgPhysicContactMsg),
    PL_MESSAGE_HANDLER(plMsgPhysicCharacterContact, OnMsgPhysicCharacterContact),
    PL_MESSAGE_HANDLER(plMsgCustomInstanceDataOffsetChanged, OnMsgCustomInstanceDataOffsetChanged),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Restore),
    PL_SCRIPT_FUNCTION_PROPERTY(ShatterCellular, In, "vGlobalPosition", In, "fCellSize", In, "vImpulse", In, "fMakeDynamicRadius"),
    PL_SCRIPT_FUNCTION_PROPERTY(ShatterRadial, In, "vGlobalPosition", In, "fImpactRadius", In, "vImpulse", In, "fMakeDynamicRadius"),
    PL_SCRIPT_FUNCTION_PROPERTY(ShatterAll, In, "fShardSize", In, "vImpulse"),
  }
  PL_END_FUNCTIONS;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plJoltBreakableSlabComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();
  s << m_fWidth;
  s << m_fHeight;
  s << m_fThickness;
  s << m_hMaterial;
  s << m_vUvScale;
  s << m_uiCollisionLayerStatic;
  s << m_uiCollisionLayerDynamic;
  s << m_Flags;
  s << m_Shape;
  s << m_fGravityFactor;
  s << m_fContactReportForceThreshold;

  // version 2
  s << m_hSurface;
  s << m_fHealth;
  s << m_fInstantShatterDamage;
  s << m_sOnCrackInteraction;
}

void plJoltBreakableSlabComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& s = inout_stream.GetStream();
  s >> m_fWidth;
  s >> m_fHeight;
  s >> m_fThickness;
  s >> m_hMaterial;
  s >> m_vUvScale;
  s >> m_uiCollisionLayerStatic;
  s >> m_uiCollisionLayerDynamic;
  s >> m_Flags;
  s >> m_Shape;
  s >> m_fGravityFactor;
  s >> m_fContactReportForceThreshold;

  if (uiVersion >= 2)
  {
    s >> m_hSurface;
    s >> m_fHealth;
    s >> m_fInstantShatterDamage;
    s >> m_sOnCrackInteraction;
  }
}

void plJoltBreakableSlabComponent::OnActivated()
{
  SUPER::OnActivated();

  m_bReinitMeshes = true;
}

void plJoltBreakableSlabComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  if (m_uiObjectFilterID == plInvalidIndex)
  {
    // only create a new filter ID, if none has been passed in manually
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

  plHybridArray<JPH::Ref<JPH::ConvexShape>, 2> shapes;

  if (m_Shape == plJoltBreakableShape::Rectangle)
  {
    // this code path is an optimization for PrepareShardColliders() because box colliders are much more efficient to set up (and simulate)
    shapes.SetCount(1);

    const float fShardThickness = plMath::Max(m_fThickness, JPH::cDefaultConvexRadius * 2.0f);
    const float fThick1 = -(fShardThickness - m_fThickness) * 0.5f;
    const float fThick2 = fShardThickness + fThick1;

    JPH::BoxShapeSettings shapeSettings;
    shapeSettings.mHalfExtent.SetX(m_fWidth * 0.5f);
    shapeSettings.mHalfExtent.SetY(m_fHeight * 0.5f);
    shapeSettings.mHalfExtent.SetZ(fShardThickness * 0.5f);
    shapeSettings.mDensity = 0.1f;
    shapeSettings.mUserData = 0;

    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (!shapeResult.HasError())
    {
      shapes[0] = (JPH::ConvexShape*)(shapeResult.Get().GetPtr());
    }
  }
  else
  {
    PrepareShardColliders(0, shapes);
  }

  CreateShardColliders(0, shapes);
  m_uiShardsSleeping = 190;

  InvalidateCachedRenderData();

  if (m_bReinitMeshes)
  {
    ReinitMeshes();
    TriggerLocalBoundsUpdate();
  }
}

void plJoltBreakableSlabComponent::OnDeactivated()
{
  Cleanup();

  plRenderDataManager* pRenderDataManager = GetWorld()->GetModule<plRenderDataManager>();
  pRenderDataManager->DeleteInstanceData(m_InstanceDataOffset);

  if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    pModule->DeallocateUserData(m_uiUserDataIndexStatic);
    pModule->DeallocateUserData(m_uiUserDataIndexDynamic);
    pModule->DeleteObjectFilterID(m_uiObjectFilterID);
  }

  SUPER::OnDeactivated();
}

plResult plJoltBreakableSlabComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  ref_bounds = m_Bounds;
  return PL_SUCCESS;
}

void plJoltBreakableSlabComponent::SetWidth(float fWidth)
{
  if (fWidth <= 0.0f)
    return;

  m_fWidth = fWidth;
  m_bReinitMeshes = true;
}

float plJoltBreakableSlabComponent::GetWidth() const
{
  return m_fWidth;
}

void plJoltBreakableSlabComponent::SetHeight(float fHeight)
{
  if (fHeight <= 0.0f)
    return;

  m_fHeight = fHeight;
  m_bReinitMeshes = true;
}

float plJoltBreakableSlabComponent::GetHeight() const
{
  return m_fHeight;
}

void plJoltBreakableSlabComponent::SetThickness(float fThickness)
{
  if (fThickness <= 0.0f)
    return;

  m_fThickness = fThickness;
  m_bReinitMeshes = true;
}

float plJoltBreakableSlabComponent::GetThickness() const
{
  return m_fThickness;
}

void plJoltBreakableSlabComponent::SetUvScale(plVec2 vScale)
{
  if (m_vUvScale == vScale)
    return;

  m_vUvScale = vScale;
  m_bReinitMeshes = true;
}


plVec2 plJoltBreakableSlabComponent::GetUvScale() const
{
  return m_vUvScale;
}

void plJoltBreakableSlabComponent::SetFlags(plBitflags<plJoltBreakableSlabFlags> flags)
{
  m_Flags = flags;
  m_bReinitMeshes = true;
}

void plJoltBreakableSlabComponent::SetShape(plEnum<plJoltBreakableShape> shape)
{
  m_Shape = shape;
  m_bReinitMeshes = true;
}

void plJoltBreakableSlabComponent::Restore()
{
  m_fDamageTaken = 0.0f;

  Cleanup();
  ReinitMeshes();
  TriggerLocalBoundsUpdate();

  plHybridArray<JPH::Ref<JPH::ConvexShape>, 2> shapes;
  PrepareShardColliders(0, shapes);

  CreateShardColliders(0, shapes);
  m_uiShardsSleeping = 190;

  InvalidateCachedRenderData();
}

bool plJoltBreakableSlabComponent::IsPointOnSlab(const plVec3& vGlobalPosition) const
{
  const plPlane mainPlane = plPlane::MakeFromNormalAndPoint(GetOwner()->GetGlobalDirUp(), GetOwner()->GetGlobalPosition());
  const float fDist = plMath::Abs(mainPlane.GetDistanceTo(vGlobalPosition));
  if (fDist > plMath::Max(JPH::cDefaultConvexRadius * 2.0f, m_fThickness))
    return false;

  return true;
}

plUInt32 plJoltBreakableSlabComponent::FindClosestShard(const plVec3& vGlobalPosition) const
{
  const plVec2 vLocalPos = (GetOwner()->GetGlobalTransform().GetInverse() * vGlobalPosition).GetAsVec2();
  float fBestDistance = plMath::HighValue<float>();

  plUInt32 uiShardIdx = plInvalidIndex;

  const plUInt32 uiShards = m_Breakable.m_Shards.GetCount();
  for (plUInt32 i = 0; i < uiShards; ++i)
  {
    const auto& shard = m_Breakable.m_Shards[i];
    if (shard.m_bShattered || shard.m_bDynamic)
      continue;

    const float fDistSqr = (shard.m_vCenterPosition - vLocalPos).GetLengthSquared();

    if (fDistSqr > fBestDistance)
      continue;

    // if the shard is smaller than we are close to it, ignore it
    if (fDistSqr > plMath::Square(shard.m_fBoundingRadius))
      continue;

    fBestDistance = fDistSqr;
    uiShardIdx = i;
  }

  return uiShardIdx;
}

void plJoltBreakableSlabComponent::ShatterCellular(const plVec3& vGlobalPosition, float fCellSize, const plVec3& vImpulse, float fMakeDynamicRadius)
{
  if (!IsPointOnSlab(vGlobalPosition))
    return;

  const plUInt32 uiShardIdx = FindClosestShard(vGlobalPosition);
  if (uiShardIdx == plInvalidIndex)
    return;

  if (m_ShatterPoints.IsEmpty())
  {
    ((plJoltBreakableSlabComponentManager*)GetOwningManager())->m_RequireBreakage.Insert(GetHandle());
  }

  auto& pt = m_ShatterPoints.ExpandAndGetRef();
  pt.m_vGlobalPosition = vGlobalPosition;
  pt.m_vImpulse = vImpulse;
  pt.m_fCellSize = fCellSize;
  pt.m_uiShardIdx = uiShardIdx;
  pt.m_fMakeDynamicRadius = fMakeDynamicRadius;
  pt.m_uiAllowedBreakPatterns = (plUInt8)plBreakablePattern::Cellular;
}

void plJoltBreakableSlabComponent::ShatterRadial(const plVec3& vGlobalPosition, float fImpactRadius, const plVec3& vImpulse, float fMakeDynamicRadius)
{
  if (!IsPointOnSlab(vGlobalPosition))
    return;

  const plUInt32 uiShardIdx = FindClosestShard(vGlobalPosition);
  if (uiShardIdx == plInvalidIndex)
    return;

  if (m_ShatterPoints.IsEmpty())
  {
    ((plJoltBreakableSlabComponentManager*)GetOwningManager())->m_RequireBreakage.Insert(GetHandle());
  }

  auto& pt = m_ShatterPoints.ExpandAndGetRef();
  pt.m_vGlobalPosition = vGlobalPosition;
  pt.m_vImpulse = vImpulse;
  pt.m_fImpactRadius = fImpactRadius;
  pt.m_fCellSize = 0.4f;
  pt.m_uiShardIdx = uiShardIdx;
  pt.m_fMakeDynamicRadius = fMakeDynamicRadius;
  pt.m_uiAllowedBreakPatterns = (plUInt8)plBreakablePattern::Radial | (plUInt8)plBreakablePattern::Cellular;
}

void plJoltBreakableSlabComponent::ShatterAll(float fShardSize, const plVec3& vImpulse)
{
  m_fContactReportForceThreshold = 0.0f;

  if (m_ShatterPoints.IsEmpty())
  {
    ((plJoltBreakableSlabComponentManager*)GetOwningManager())->m_RequireBreakage.Insert(GetHandle());
  }

  auto& pt = m_ShatterPoints.ExpandAndGetRef();
  pt.m_vGlobalPosition.Set(1024, 1024, 1024);
  pt.m_vImpulse = vImpulse;
  pt.m_fCellSize = fShardSize;
  pt.m_uiShardIdx = plInvalidIndex;
}

void plJoltBreakableSlabComponent::PrepareBreakAsync(plDynamicArray<JPH::Ref<JPH::ConvexShape>>& out_Shapes, plArrayPtr<const plShatterPoint> points)
{
  PL_PROFILE_SCOPE("PrepareBreakAsync");


  for (plUInt32 i = 0; i < points.GetCount(); ++i)
  {
    const auto& point = points[i];

    if (point.m_vGlobalPosition == plVec3(1024, 1024, 1024))
    {
      // shatter all pieces
      m_Breakable.ShatterAll(point.m_fCellSize, GetWorld()->GetRandomNumberGenerator(), true);
      break;
    }
    else
    {

      if (point.m_uiShardIdx != plInvalidIndex && !m_Breakable.m_Shards[point.m_uiShardIdx].m_bShattered)
      {
        const plUInt32 uiShardIdxOffset = m_Breakable.m_Shards.GetCount();
        const plVec2 vLocalPos = (GetOwner()->GetGlobalTransform().GetInverse() * point.m_vGlobalPosition).GetAsVec2();

        m_Breakable.ShatterShard(point.m_uiShardIdx, vLocalPos, GetWorld()->GetRandomNumberGenerator(), plMath::Clamp(point.m_fImpactRadius, 0.05f, 0.4f), plMath::Clamp(point.m_fCellSize, 0.3f, 0.5f), point.m_uiAllowedBreakPatterns);

        const float fDistSqr = plMath::Square(point.m_fMakeDynamicRadius);

        for (plUInt32 idx = uiShardIdxOffset; idx < m_Breakable.m_Shards.GetCount(); ++idx)
        {
          // make everything dynamic that can't be broken further
          if (m_Breakable.m_Shards[idx].m_uiBreakablePatterns == (plUInt8)plBreakablePattern::None)
          {
            m_Breakable.m_Shards[idx].m_bDynamic = true;
          }
          else if ((m_Breakable.m_Shards[idx].m_vCenterPosition - vLocalPos).GetLengthSquared() <= fDistSqr)
          {
            // and everything that's close enough to the shatter point
            m_Breakable.m_Shards[idx].m_bDynamic = true;
          }
        }
      }
    }
  }


  m_Breakable.RecalculateDymamic();

  PrepareShardColliders(m_ShardBodyIDs.GetCount(), out_Shapes);
}

void plJoltBreakableSlabComponent::ApplyBreak(plArrayPtr<JPH::Ref<JPH::ConvexShape>> shapes, const plMeshResourceHandle& hMesh, const plVec3& vImpulse)
{
  PL_PROFILE_SCOPE("ApplyBreak");

  UpdateShardColliders();

  const plUInt32 uiFirstShard = m_ShardBodyIDs.GetCount();

  m_hMesh = hMesh;
  CreateShardColliders(uiFirstShard, shapes);

  ApplyImpulse(vImpulse, uiFirstShard);

  // make sure everyone around is awake
  WakeUpBodies();
}

void plJoltBreakableSlabComponent::Cleanup()
{
  if (m_pShatterTask)
  {
    plTaskSystem::CancelTask(m_pShatterTask, plOnTaskRunning::WaitTillFinished).IgnoreResult();
    m_pShatterTask = nullptr;
  }

  DestroyAllShardColliders();

  m_Breakable.Clear();
  m_ShatterPoints.Clear();

  m_hMesh.Invalidate();

  m_SkinningState.Clear();
}

void plJoltBreakableSlabComponent::ReinitMeshes()
{
  PL_ASSERT_DEBUG(IsActive(), "Should only be called on active components.");

  m_bReinitMeshes = false;

  Cleanup();

  m_Breakable.Initialize();

  auto& shard = m_Breakable.m_Shards[0];

  if (m_Shape == plJoltBreakableShape::Rectangle)
  {
    shard.m_vCenterPosition.Set(m_fWidth * 0.5f, m_fHeight * 0.5f);
    shard.m_fBoundingRadius = plMath::Max(m_fWidth, m_fHeight) * 0.75f;
    shard.m_Edges.SetCount(4);

    shard.m_Edges[0].m_vStartPosition.Set(0, 0);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeLeft))
      shard.m_Edges[0].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;

    shard.m_Edges[1].m_vStartPosition.Set(0, m_fHeight);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeTop))
      shard.m_Edges[1].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;

    shard.m_Edges[2].m_vStartPosition.Set(m_fWidth, m_fHeight);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeRight))
      shard.m_Edges[2].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;

    shard.m_Edges[3].m_vStartPosition.Set(m_fWidth, 0);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeBottom))
      shard.m_Edges[3].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;
  }
  else if (m_Shape == plJoltBreakableShape::Triangle)
  {
    shard.m_vCenterPosition.Set(m_fWidth * 0.5f, m_fHeight * 0.5f);
    shard.m_fBoundingRadius = plMath::Max(m_fWidth, m_fHeight) * 0.75f;
    shard.m_Edges.SetCount(3);

    shard.m_Edges[0].m_vStartPosition.Set(0, 0);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeLeft))
      shard.m_Edges[0].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;

    shard.m_Edges[1].m_vStartPosition.Set(0, m_fHeight);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeTop) || m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeRight))
      shard.m_Edges[1].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;

    shard.m_Edges[2].m_vStartPosition.Set(m_fWidth, 0);
    if (m_Flags.IsSet(plJoltBreakableSlabFlags::FixedEdgeBottom))
      shard.m_Edges[2].m_uiOutsideShardIdx = plBreakableShard2D::FixedEdge;
  }
  else if (m_Shape == plJoltBreakableShape::Circle)
  {
    const plUInt32 num = 16;

    shard.m_vCenterPosition.Set(m_fWidth * 0.5f, m_fHeight * 0.5f);
    shard.m_fBoundingRadius = plMath::Max(m_fWidth, m_fHeight) * 0.75f;
    shard.m_Edges.SetCount(num);

    const float hw = m_fWidth * 0.5f;
    const float hh = m_fHeight * 0.5f;

    const plUInt32 uiOutsideIdx = m_Flags.IsAnySet(plJoltBreakableSlabFlags::Default) ? plBreakableShard2D::FixedEdge : plBreakableShard2D::LooseEdge;

    for (plUInt32 i = 0; i < num; ++i)
    {
      const plAngle a = plAngle::MakeFromDegree((360.0f / num) * i);

      shard.m_Edges[i].m_vStartPosition.Set(hw + plMath::Sin(a) * hw, hh + plMath::Cos(a) * hh);
      shard.m_Edges[i].m_uiOutsideShardIdx = uiOutsideIdx;
    }
  }

  m_hMesh = CreateShardsMesh();

  InvalidateCachedRenderData();

  m_Bounds = plBoundingBox::MakeFromMinMax(plVec3::MakeZero(), plVec3(m_fWidth, m_fHeight, m_fThickness));
}

void plJoltBreakableSlabComponent::BuildMeshResourceFromGeometry(plGeometry& Geometry, plMeshResourceDescriptor& MeshDesc, bool bWithSkinningData) const
{
  auto& MeshBufferDesc = MeshDesc.MeshBufferDesc();

  MeshBufferDesc.AddCommonStreams();

  if (bWithSkinningData)
  {
    MeshBufferDesc.AddStream(plMeshVertexStreamType::SkinningData);
  }
  MeshBufferDesc.AllocateStreamsFromGeometry(Geometry, plGALPrimitiveTopology::Triangles);

  MeshDesc.AddSubMesh(MeshBufferDesc.GetPrimitiveCount(), 0, 0);

  MeshDesc.ComputeBounds();
}

const plJoltMaterial* plJoltBreakableSlabComponent::GetPhysicsMaterial()
{
  if (m_hMaterial.IsValid())
  {
    if (!m_hSurface.IsValid())
    {
      plResourceLock<plMaterialResource> pMat(m_hMaterial, plResourceAcquireMode::BlockTillLoaded);

      if (!pMat->GetSurface().IsEmpty())
      {
        m_hSurface = plResourceManager::LoadResource<plSurfaceResource>(pMat->GetSurface());
      }
    }

    if (m_hSurface.IsValid())
    {
      plResourceLock<plSurfaceResource> pSurf(m_hSurface, plResourceAcquireMode::BlockTillLoaded);

      if (pSurf->m_pPhysicsMaterialJolt != nullptr)
      {
        return static_cast<plJoltMaterial*>(pSurf->m_pPhysicsMaterialJolt);
      }
    }
  }

  return plJoltCore::GetDefaultMaterial();
}

void AddSkirtPolygons(const plVec2& vPoint0, const plVec2& vPoint1, float fThickness, plGeometry& ref_geometry, const plGeometry::GeoOptions& opt)
{
  const float fSpanX = plMath::Abs(vPoint0.x - vPoint1.x);
  const float fSpanY = plMath::Abs(vPoint0.y - vPoint1.y);
  const float fSpan = plMath::Max(fSpanX, fSpanY);

  plVec3 vPoint0Front(vPoint0.x, vPoint0.y, fThickness);
  plVec2 vPoint0FrontUV(fThickness, 0);
  plVec3 vPoint0Back(vPoint0.x, vPoint0.y, 0);
  plVec2 vPoint0BackUV(0, 0);
  plVec3 vPoint1Front(vPoint1.x, vPoint1.y, fThickness);
  plVec2 vPoint1FrontUV(fThickness, fSpan);
  plVec3 vPoint1Back(vPoint1.x, vPoint1.y, 0);
  plVec2 vPoint1BackUV(0, fSpan);

  plVec3 FaceNormal;
  if (FaceNormal.CalculateNormal(vPoint0Front, vPoint1Front, vPoint0Back).Failed())
  {
    // ignore degenerate triangles
    return;
  }

  const plUInt32 uiIdx0 = ref_geometry.AddVertex(opt, vPoint0Front, FaceNormal, vPoint0FrontUV);
  const plUInt32 uiIdx1 = ref_geometry.AddVertex(opt, vPoint0Back, FaceNormal, vPoint0BackUV);
  const plUInt32 uiIdx2 = ref_geometry.AddVertex(opt, vPoint1Front, FaceNormal, vPoint1FrontUV);
  const plUInt32 uiIdx3 = ref_geometry.AddVertex(opt, vPoint1Back, FaceNormal, vPoint1BackUV);

  {
    plUInt32 idx[3] = {uiIdx0, uiIdx2, uiIdx1};
    ref_geometry.AddPolygon(idx, false);
  }

  {
    plUInt32 idx[3] = {uiIdx1, uiIdx2, uiIdx3};
    ref_geometry.AddPolygon(idx, false);
  }
}

plMeshResourceHandle plJoltBreakableSlabComponent::CreateShardsMesh() const
{
  PL_PROFILE_SCOPE("CreateShardsMesh");

  plStringBuilder meshName;
  meshName.SetFormat("JoltSlab-{}-{}", plArgP(this), s_iShardMeshCounter.Increment());

  plGeometry geo;
  plHybridArray<plUInt32, 16> vtxIdx1;
  plHybridArray<plUInt32, 16> vtxIdx2;

  const plVec3 vNormal(0, 0, 1);

  const plVec3 vOffset = vNormal * m_fThickness;

  for (plUInt32 uiShardIdx = 0; uiShardIdx < m_Breakable.m_Shards.GetCount(); ++uiShardIdx)
  {
    const auto& shard = m_Breakable.m_Shards[uiShardIdx];
    vtxIdx1.Clear();
    vtxIdx2.Clear();

    plGeometry::GeoOptions opt;
    opt.m_uiBoneIndex = uiShardIdx;

    for (plUInt32 i = 0; i < shard.m_Edges.GetCount(); ++i)
    {
      plVec3 v = shard.m_Edges[i].m_vStartPosition.GetAsVec3(0);

      plVec2 vTexCoord;
      vTexCoord.x = v.x / m_fWidth;
      vTexCoord.y = 1.0f - (v.y / m_fHeight);
      vTexCoord = vTexCoord.CompMul(m_vUvScale);

      v -= shard.m_vCenterPosition.GetAsVec3(0);

      vtxIdx1.PushBack(geo.AddVertex(opt, v, -vNormal, vTexCoord));
      vtxIdx2.PushBack(geo.AddVertex(opt, v + vOffset, vNormal, vTexCoord));
    }

    geo.AddPolygon(vtxIdx1, false);
    geo.AddPolygon(vtxIdx2, true);

    plUInt32 uiPrev = shard.m_Edges.GetCount() - 1;
    for (plUInt32 i = 0; i < shard.m_Edges.GetCount(); ++i)
    {
      AddSkirtPolygons(shard.m_Edges[uiPrev].m_vStartPosition - shard.m_vCenterPosition, shard.m_Edges[i].m_vStartPosition - shard.m_vCenterPosition, m_fThickness, geo, opt);
      uiPrev = i;
    }
  }

  geo.TriangulatePolygons();
  geo.ComputeTangents();

  plMeshResourceDescriptor desc;
  BuildMeshResourceFromGeometry(geo, desc, true /* include skinning data */);

  return plResourceManager::CreateResource<plMeshResource>(meshName, std::move(desc));
}

void plJoltBreakableSlabComponent::PrepareShardColliders(plUInt32 uiFirstShard, plDynamicArray<JPH::Ref<JPH::ConvexShape>>& out_Shapes) const
{
  PL_PROFILE_SCOPE("PrepareShardColliders");

  out_Shapes.SetCount(m_Breakable.m_Shards.GetCount() - uiFirstShard);

  const float fShardThickness = plMath::Max(m_fThickness, JPH::cDefaultConvexRadius * 2.0f);
  const float fThick1 = -(fShardThickness - m_fThickness) * 0.5f;
  const float fThick2 = fShardThickness + fThick1;

  for (plUInt32 uiShardIdx = 0; uiShardIdx < out_Shapes.GetCount(); ++uiShardIdx)
  {
    const auto& shard = m_Breakable.m_Shards[uiFirstShard + uiShardIdx];

    if (shard.m_bShattered)
      continue;

    JPH::Array<JPH::Vec3> points;
    points.reserve(shard.m_Edges.GetCount() * 2);

    for (plUInt32 i = 0; i < shard.m_Edges.GetCount(); ++i)
    {
      const plVec2 v = shard.m_Edges[i].m_vStartPosition - shard.m_vCenterPosition;
      points.push_back(JPH::Vec3(v.x, v.y, fThick1));
      points.push_back(JPH::Vec3(v.x, v.y, fThick2));
    }

    JPH::ConvexHullShapeSettings shapeSettings(points);
    shapeSettings.mDensity = 0.1f;
    shapeSettings.mUserData = 0;

    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError())
    {
      // plLog::Warning("Invalid shard shape: {} - {} points", shapeResult.GetError().c_str(), points.size());
      continue;
    }

    out_Shapes[uiShardIdx] = (JPH::ConvexShape*)(shapeResult.Get().GetPtr());
  }
}

void plJoltBreakableSlabComponent::UpdateShardColliders()
{
  PL_PROFILE_SCOPE("UpdateShardColliders");

  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  auto* pSystem = pModule->GetJoltSystem();
  auto& jphBodies = pModule->GetBodyInterface();

  for (plUInt32 uiShardIdx = 0; uiShardIdx < m_ShardBodyIDs.GetCount(); ++uiShardIdx)
  {
    const auto& shard = m_Breakable.m_Shards[uiShardIdx];

    if (m_ShardBodyIDs[uiShardIdx] == plInvalidIndex)
      continue;

    if (shard.m_bShattered)
    {
      // remove shard colliders that are now destroyed
      DestroyShardCollider(uiShardIdx, jphBodies, true);
      continue;
    }

    if (!shard.m_bDynamic)
      continue;

    JPH::BodyID bodyId(m_ShardBodyIDs[uiShardIdx]);
    bool bRemoveBody = false;

    {
      JPH::BodyLockWrite bodyLock(pSystem->GetBodyLockInterface(), bodyId);

      if (bodyLock.Succeeded())
      {
        JPH::Body& body = bodyLock.GetBody();
        if (body.GetMotionType() == JPH::EMotionType::Static)
        {
          bRemoveBody = true;

          JPH::BodyCreationSettings bodyCfg;
          bodyCfg.SetShape(body.GetShape());
          bodyCfg.mRestitution = body.GetRestitution();
          bodyCfg.mFriction = body.GetFriction();
          bodyCfg.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
          bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
          bodyCfg.mUserData = body.GetUserData();
          bodyCfg.mMassPropertiesOverride.mMass = 1.0f;
          bodyCfg.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
          bodyCfg.mMotionType = JPH::EMotionType::Dynamic;
          bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerDynamic, plJoltBroadphaseLayer::Debris);
          bodyCfg.mPosition = body.GetPosition();
          bodyCfg.mRotation = body.GetRotation();
          bodyCfg.mGravityFactor = m_fGravityFactor;

          // Create and add body
          JPH::Body* pBody = jphBodies.CreateBody(bodyCfg);
          pModule->QueueBodyToAdd(pBody, true);

          m_ShardBodyIDs[uiShardIdx] = pBody->GetID().GetIndexAndSequenceNumber();
        }
      }
    }

    if (bRemoveBody)
    {
      // can only do this outside the lock
      if (jphBodies.IsAdded(bodyId))
      {
        jphBodies.RemoveBody(bodyId);
      }
      else
      {
        // still pending in the module's add queue; it must not reference a destroyed body
        pModule->RemoveBodyFromQueue(bodyId);
      }

      jphBodies.DestroyBody(bodyId);
    }
  }
}

void plJoltBreakableSlabComponent::WakeUpBodies()
{
  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  auto& jphBodies = pModule->GetBodyInterface();

  plJoltObjectLayerFilter objectFilter(m_uiCollisionLayerStatic);

  plUInt32 broadphase = plPhysicsShapeType::Default;
  broadphase &= ~plPhysicsShapeType::Static;
  broadphase &= ~plPhysicsShapeType::Character;
  broadphase &= ~plPhysicsShapeType::Query;
  broadphase &= ~plPhysicsShapeType::Trigger;
  plJoltBroadPhaseLayerFilter broadphaseFilter(static_cast<plPhysicsShapeType::Enum>(broadphase));

  plBoundingBox bbox;
  bbox.m_vMin.SetZero();
  bbox.m_vMax = plVec3(m_fWidth, m_fHeight, m_fThickness);
  bbox.Grow(plVec3(0.3f));

  bbox.TransformFromOrigin(GetOwner()->GetGlobalTransform().GetAsMat4());

  const JPH::AABox box = JPH::AABox::sFromTwoPoints(plJoltConversionUtils::ToVec3(bbox.m_vMin), plJoltConversionUtils::ToVec3(bbox.m_vMax));
  jphBodies.ActivateBodiesInAABox(box, broadphaseFilter, objectFilter);
}

void plJoltBreakableSlabComponent::CreateShardColliders(plUInt32 uiFirstShard, plArrayPtr<JPH::Ref<JPH::ConvexShape>> shapes)
{
  PL_PROFILE_SCOPE("CreateShardColliders");

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();
  auto& jphBodies = pModule->GetBodyInterface();

  const plJoltUserData* pUserDataStatic = &pModule->GetUserData(m_uiUserDataIndexStatic);
  const plJoltUserData* pUserDataDynamic = &pModule->GetUserData(m_uiUserDataIndexDynamic);
  const plJoltMaterial* pMaterial = GetPhysicsMaterial();

  // GetOrCreateBoneTransformsForWriting will resize as necessary but will lose existing data so we need to make a copy here
  plDynamicArray<plShaderTransform> oldTransforms(plFrameAllocator::GetCurrentAllocator());
  oldTransforms = m_SkinningState.GetBoneTransformsForReading();

  auto transforms = m_SkinningState.GetOrCreateBoneTransformsForWriting(*this, m_Breakable.m_Shards.GetCount());
  plMemoryUtils::Copy(transforms.GetPtr(), oldTransforms.GetData(), oldTransforms.GetCount());

  m_ShardBodyIDs.SetCount(m_Breakable.m_Shards.GetCount(), plInvalidIndex);

  for (plUInt32 uiShardIdx = uiFirstShard; uiShardIdx < m_Breakable.m_Shards.GetCount(); ++uiShardIdx)
  {
    transforms[uiShardIdx] = plMat4::MakeScaling(plVec3(0));

    const auto& shard = m_Breakable.m_Shards[uiShardIdx];

    if (shard.m_bShattered)
      continue;

    JPH::ConvexShape* pShape = shapes[uiShardIdx - uiFirstShard];
    if (pShape == nullptr)
      continue;

    pShape->SetMaterial(pMaterial);

    // Body settings
    JPH::BodyCreationSettings bodyCfg;
    bodyCfg.SetShape(pShape);
    // bodyCfg.mMotionQuality = JPH::EMotionQuality::LinearCast; // don't use CCD, shards are allowed to tunnel through geometry, they will be destroyed, if necessary
    bodyCfg.mRestitution = pMaterial->m_fRestitution;
    bodyCfg.mFriction = pMaterial->m_fFriction;
    bodyCfg.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
    bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
    bodyCfg.mMassPropertiesOverride.mMass = 1.0f;
    bodyCfg.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodyCfg.mGravityFactor = m_fGravityFactor;

    if (shard.m_bDynamic == false)
    {
      bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserDataStatic);
      bodyCfg.mMotionType = JPH::EMotionType::Static;
      bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerStatic, plJoltBroadphaseLayer::Static);
    }
    else
    {
      bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserDataDynamic);
      bodyCfg.mMotionType = JPH::EMotionType::Dynamic;
      bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayerDynamic, plJoltBroadphaseLayer::Debris);
    }

    // Set transform to owner
    plTransform trans = GetOwner()->GetGlobalTransform();
    trans.m_vPosition += trans.m_qRotation * shard.m_vCenterPosition.GetAsVec3(0);

    bodyCfg.mPosition = plJoltConversionUtils::ToVec3(trans.m_vPosition);
    bodyCfg.mRotation = plJoltConversionUtils::ToQuat(trans.m_qRotation).Normalized();

    transforms[uiShardIdx] = trans;

    // Create and add body
    JPH::Body* pBody = jphBodies.CreateBody(bodyCfg);
    pModule->QueueBodyToAdd(pBody, shard.m_bDynamic);

    m_ShardBodyIDs[uiShardIdx] = pBody->GetID().GetIndexAndSequenceNumber();
  }
}

void plJoltBreakableSlabComponent::DestroyAllShardColliders()
{
  if (m_ShardBodyIDs.IsEmpty())
    return;

  if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    auto& jphBodies = pModule->GetBodyInterface();

    for (plUInt32 uiShardIdx = 0; uiShardIdx < m_ShardBodyIDs.GetCount(); ++uiShardIdx)
    {
      DestroyShardCollider(uiShardIdx, jphBodies, false);
    }
  }

  m_ShardBodyIDs.Clear();
}

void plJoltBreakableSlabComponent::DestroyShardCollider(plUInt32 uiShardIdx, JPH::BodyInterface& jphBodies, bool bUpdateVis)
{
  JPH::BodyID bodyId(m_ShardBodyIDs[uiShardIdx]);
  m_ShardBodyIDs[uiShardIdx] = plInvalidIndex;

  if (bUpdateVis)
  {
    auto transforms = m_SkinningState.GetOrCreateBoneTransformsForWriting(*this, m_SkinningState.m_uiNumBones);
    transforms[uiShardIdx] = plMat4::MakeScaling(plVec3(0)); // make it disappear by scaling to zero
  }

  if (bodyId.IsInvalid())
    return;

  if (jphBodies.IsAdded(bodyId))
  {
    jphBodies.RemoveBody(bodyId);
  }
  else if (plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
  {
    // still pending in the module's add queue; it must not reference a destroyed body
    pModule->RemoveBodyFromQueue(bodyId);
  }

  jphBodies.DestroyBody(bodyId);
}

void plJoltBreakableSlabComponent::RetrieveShardTransforms()
{
  if (m_uiShardsSleeping >= 200)
  {
    // invalidate the cache, because at this point it is set to 'static'
    InvalidateCachedRenderData();
  }

  m_uiShardsSleeping = 0;

  plBoundingBox bbox = plBoundingBox::MakeInvalid();

  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  auto& jphBodies = pModule->GetBodyInterface();

  const plVec3 vOwnPos = GetOwner()->GetGlobalPosition();

  auto transforms = m_SkinningState.GetOrCreateBoneTransformsForWriting(*this, m_SkinningState.m_uiNumBones);

  for (plUInt32 idx = 0; idx < m_ShardBodyIDs.GetCount(); ++idx)
  {
    JPH::BodyID bodyId(m_ShardBodyIDs[idx]);

    if (bodyId.IsInvalid())
      continue;

    JPH::RVec3 pos;
    JPH::Quat rot;
    jphBodies.GetPositionAndRotation(bodyId, pos, rot);

    transforms[idx] = plJoltConversionUtils::ToTransform(pos, rot);

    const plVec3 vPos = plJoltConversionUtils::ToVec3(pos);

    if ((vPos - vOwnPos).GetLengthSquared() > plMath::Square(30.0f))
    {
      // if the shard moved pretty far away from the center, it probably tunneled through geometry and just keeps falling
      // we don't want to use continuous collision detection (CCD), because this is just low-priority eye-candy
      // so simply remove shards that appear problematic
      DestroyShardCollider(idx, jphBodies, true);
      continue;
    }

    bbox.ExpandToInclude(vPos);
  }

  if (bbox.IsValid())
  {
    const plTransform tInv = GetOwner()->GetGlobalTransform().GetInverse();

    bbox.TransformFromOrigin(tInv.GetAsMat4());
    bbox.Grow(plVec3(m_Breakable.m_fMaxRadius));

    m_Bounds = bbox;
    TriggerLocalBoundsUpdate();
  }
}

void plJoltBreakableSlabComponent::ApplyImpulse(const plVec3& vImpulse, plUInt32 uiFirstShard)
{
  const float fTorque = vImpulse.GetLength();
  if (fTorque <= 0.01f)
    return;

  plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();

  const plVec3 vTorques[] = {
    plVec3::MakeAxisX(),
    -plVec3::MakeAxisX(),
    plVec3::MakeAxisY(),
    -plVec3::MakeAxisY(),
    plVec3::MakeAxisZ(),
    -plVec3::MakeAxisZ(),
    plVec3(0.7f, 0.7f, 0.0f),
    plVec3(0.7f, -0.7f, 0.0f),
    plVec3(-0.7f, 0.7f, 0.0f),
    plVec3(-0.7f, -0.7f, 0.0f),
    plVec3(0.7f, 0, 0.7f),
    plVec3(0.7f, 0, -0.7f),
    plVec3(-0.7f, 0, -0.7f),
    plVec3(-0.7f, 0, 0.7f),
    plVec3(0, 0.7f, 0.7f),
    plVec3(0, -0.7f, 0.7f),
  };

  const plUInt32 uiNumTorques = PL_ARRAY_SIZE(vTorques);

  for (plUInt32 idx = uiFirstShard; idx < m_Breakable.m_Shards.GetCount(); ++idx)
  {
    const auto& shard = m_Breakable.m_Shards[idx];

    if (shard.m_bDynamic)
    {
      const plUInt32 uiBodyID = m_ShardBodyIDs[idx];

      if (uiBodyID != plInvalidIndex)
      {
        pModule->AddImpulse(uiBodyID, vImpulse);
        pModule->AddTorque(uiBodyID, vTorques[idx % uiNumTorques] * fTorque);
      }
    }
  }
}

void plJoltBreakableSlabComponent::OnMsgDamage(plMsgDamage& ref_msg)
{
  // two-stage glass: bullets first crack the pane (decal + sound via the Crack surface
  // interaction), then shatter it radially at the final hit once the health pool is spent
  if (m_fHealth <= 0.0f)
    return;

  plVec3 vDir = ref_msg.m_vImpactDirection;
  vDir.NormalizeIfNotZero(GetOwner()->GetGlobalDirUp()).IgnoreResult();

  const float fDamage = (float)ref_msg.m_fDamage;
  m_fDamageTaken += fDamage;

  const bool bInstant = m_fInstantShatterDamage > 0.0f && fDamage >= m_fInstantShatterDamage;

  if (!bInstant && m_fDamageTaken < m_fHealth)
  {
    if (!m_sOnCrackInteraction.IsEmpty() && m_hSurface.IsValid())
    {
      // the slab plane normal is the owner's up axis; use the side that faces the shooter
      plVec3 vNormal = GetOwner()->GetGlobalDirUp();
      if (vNormal.Dot(vDir) > 0.0f)
      {
        vNormal = -vNormal;
      }

      plResourceLock<plSurfaceResource> pSurface(m_hSurface, plResourceAcquireMode::AllowLoadingFallback);

      if (pSurface.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        pSurface->InteractWithSurface(GetWorld(), GetOwner()->GetHandle(), ref_msg.m_vGlobalPosition, vNormal, vDir, m_sOnCrackInteraction, &GetOwner()->GetTeamID());
      }
    }

    return;
  }

  ShatterRadial(ref_msg.m_vGlobalPosition, 0.1f, vDir * ref_msg.m_fImpulse, 0.4f);
}

void plJoltBreakableSlabComponent::OnMsgPhysicsAddImpulse(plMsgPhysicsAddImpulse& ref_msg)
{
  // not implemented atm
}

void plJoltBreakableSlabComponent::OnMsgPhysicContactMsg(plMsgPhysicContact& ref_msg)
{
  if (m_fContactReportForceThreshold <= 0.0f)
    return;

  if (ref_msg.m_fImpactSqr < plMath::Square(m_fContactReportForceThreshold))
    return;

  // let the script decide what to do
  GetOwner()->PostEventMessage(ref_msg, this, plTime::MakeZero(), plObjectMsgQueueType::PostTransform);
}

void plJoltBreakableSlabComponent::OnMsgPhysicCharacterContact(plMsgPhysicCharacterContact& ref_msg)
{
  if (m_fContactReportForceThreshold <= 0.0f)
    return;

  if (ref_msg.m_fImpact < m_fContactReportForceThreshold)
    return;

  // let the script decide what to do
  GetOwner()->PostEventMessage(ref_msg, this, plTime::MakeZero(), plObjectMsgQueueType::PostTransform);
}

void plJoltBreakableSlabComponent::OnMsgCustomInstanceDataOffsetChanged(plMsgCustomInstanceDataOffsetChanged& ref_msg)
{
  m_SkinningState.m_DataOffset = ref_msg.m_NewOffset;
}

void plJoltBreakableSlabComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (m_hMesh.IsValid())
  {
    plMeshRenderData* pRenderData = nullptr;
    plTransform globalTransform;

    const bool bHasSkinning = m_SkinningState.HasBoneTransforms();
    if (bHasSkinning)
    {
      auto pSkinnedRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plSkinnedMeshRenderData>(GetOwner());
      pSkinnedRenderData->m_DataOffsets.m_uiSkinning = m_SkinningState.m_DataOffset.m_uiOffset;
      pSkinnedRenderData->m_hSkinningBuffer = msg.m_pRenderDataManager->GetSkinningDataBuffer();
      pRenderData = pSkinnedRenderData;

      globalTransform = plTransform::MakeIdentity();
    }
    else
    {
      pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());

      globalTransform = GetOwner()->GetGlobalTransform();
      globalTransform.m_vPosition += globalTransform.m_qRotation * plVec3(m_fWidth * 0.5f, m_fHeight * 0.5f, 0);
    }

    const bool bDynamic = GetOwner()->IsDynamic();
    auto hInstanceBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, globalTransform, m_InstanceDataOffset, GetUniqueIdForRendering());

    pRenderData->SetFallbackGlobalBounds(GetOwner()->GetGlobalBounds().GetBox());
    pRenderData->Fill(m_InstanceDataOffset, hInstanceBuffer, m_hMaterial, m_hMesh);

    plRenderData::Category category = plMaterialResource::GetRenderDataCategory(m_hMaterial);

    if (bHasSkinning)
    {
      if (m_uiShardsSleeping >= 200)
      {
        msg.AddRenderData(pRenderData, category, plRenderData::Caching::IfStatic);
      }
      else
      {
        ++m_uiShardsSleeping;
        msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
      }
    }
    else
    {
      msg.AddRenderData(pRenderData, category, plRenderData::Caching::IfStatic);
    }
  }
}

void plJoltBreakableSlabComponent::DebugDraw()
{
  const plTransform trans = GetOwner()->GetGlobalTransform();

  plDynamicArray<plDebugRenderer::Line> lines;

  const plColor edgeColor = plColor::White;
  const plColor centerColor = plColor::Yellow;

  for (const auto& shard : m_Breakable.m_Shards)
  {
    if (shard.m_bShattered)
      continue;

    auto& center = lines.ExpandAndGetRef();
    center.m_start = shard.m_vCenterPosition.GetAsVec3(0);
    center.m_end = center.m_start + plVec3(0, 0, 0.1f);
    center.m_startColor = centerColor;
    center.m_endColor = centerColor;

    const plColor c = shard.m_bDynamic ? plColor::GreenYellow : plColor::OrangeRed;


    plUInt32 uiPrevEdge = shard.m_Edges.GetCount() - 1;
    for (plUInt32 i = 0; i < shard.m_Edges.GetCount(); ++i)
    {

      auto& l = lines.ExpandAndGetRef();
      l.m_start = shard.m_Edges[uiPrevEdge].m_vStartPosition.GetAsVec3(0);
      l.m_end = shard.m_Edges[i].m_vStartPosition.GetAsVec3(0);

      const plUInt32 uiOut = shard.m_Edges[uiPrevEdge].m_uiOutsideShardIdx;
      if (uiOut > plInvalidIndex - 32 && uiOut < plInvalidIndex)
      {
        l.m_startColor = edgeColor;
        l.m_endColor = edgeColor;
      }
      else
      {
        l.m_startColor = c;
        l.m_endColor = c;
      }

      uiPrevEdge = i;
    }
  }

  plDebugRenderer::DrawLines(GetWorld(), lines, plColor::White, trans);

  {
    const plVec3 vCenter = GetOwner()->GetGlobalPosition() + GetOwner()->GetGlobalRotation() * (plVec3(m_fWidth * 0.5f, m_fHeight * 0.2f, 0));

    plStringBuilder txt;
    txt.AppendFormat("Shards: {}\n", m_Breakable.m_Shards.GetCount());

    if (m_uiShardsSleeping >= 200)
      txt.Append("Sleeping\n");
    else
      txt.Append("Moving\n");

    plDebugRenderer::Draw3DText(GetWorld(), txt, vCenter, plColor::LightGray);
  }
}

void plShatterTask::Execute()
{
  m_vFinalImpulse.SetZero();

  if (!m_ShatterPoints.IsEmpty())
  {
    for (const auto& pt : m_ShatterPoints)
    {
      m_vFinalImpulse += pt.m_vImpulse;
    }

    m_vFinalImpulse /= (float)m_ShatterPoints.GetCount();
  }

  m_pComponent->PrepareBreakAsync(m_Shapes, m_ShatterPoints);
  m_hShardsMesh = m_pComponent->CreateShardsMesh();
}


PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltBreakableSlabComponent);
