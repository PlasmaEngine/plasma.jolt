#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <JoltPlugin/Actors/JoltStaticActorComponent.h>
#include <JoltPlugin/Components/JoltVisColMeshComponent.h>
#include <JoltPlugin/Shapes/JoltShapeConvexHullComponent.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltVisColMeshComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_RESOURCE_ACCESSOR_PROPERTY("CollisionMesh", GetMesh, SetMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Jolt_Colmesh_Triangle;CompatibleAsset_Jolt_Colmesh_Convex", plDependencyFlags::Package)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Misc"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plJoltVisColMeshComponent::plJoltVisColMeshComponent() = default;
plJoltVisColMeshComponent::~plJoltVisColMeshComponent() = default;

void plJoltVisColMeshComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();

  s << m_hCollisionMesh;
}


void plJoltVisColMeshComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  // const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& s = inout_stream.GetStream();

  s >> m_hCollisionMesh;

  GetWorld()->GetOrCreateComponentManager<plJoltVisColMeshComponentManager>()->EnqueueUpdate(GetHandle());
}

plResult plJoltVisColMeshComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  // have to assume this isn't thread safe
  // CreateCollisionRenderMesh();

  if (m_hMesh.IsValid())
  {
    plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::BlockTillLoaded);
    ref_bounds = pMesh->GetBounds();
    return PL_SUCCESS;
  }

  return PL_FAILURE;
}

void plJoltVisColMeshComponent::SetMesh(const plJoltMeshResourceHandle& hMesh)
{
  if (m_hCollisionMesh != hMesh)
  {
    m_hCollisionMesh = hMesh;
    m_hMesh.Invalidate();

    GetWorld()->GetOrCreateComponentManager<plJoltVisColMeshComponentManager>()->EnqueueUpdate(GetHandle());
  }
}

void plJoltVisColMeshComponent::CreateCollisionRenderMesh()
{
  if (!m_hCollisionMesh.IsValid())
  {
    plJoltStaticActorComponent* pSibling = nullptr;
    if (GetOwner()->TryGetComponentOfBaseType(pSibling))
    {
      m_hCollisionMesh = pSibling->GetMesh();
    }
  }

  if (!m_hCollisionMesh.IsValid())
  {
    plJoltShapeConvexHullComponent* pSibling = nullptr;
    if (GetOwner()->TryGetComponentOfBaseType(pSibling))
    {
      m_hCollisionMesh = pSibling->GetMesh();
    }
  }

  if (!m_hCollisionMesh.IsValid())
    return;

  plResourceLock<plJoltMeshResource> pMesh(m_hCollisionMesh, plResourceAcquireMode::BlockTillLoaded);

  if (pMesh.GetAcquireResult() == plResourceAcquireResult::MissingFallback)
    return;

  plStringBuilder sColMeshName = pMesh->GetResourceID();
  sColMeshName.AppendFormat("_{0}_JoltVisColMesh",
    pMesh->GetCurrentResourceChangeCounter()); // the change counter allows to react to resource updates

  m_hMesh = plResourceManager::GetExistingResource<plMeshResource>(sColMeshName);

  if (m_hMesh.IsValid())
  {
    TriggerLocalBoundsUpdate();
    return;
  }

  plCpuMeshResourceHandle hCpuMesh = pMesh->ConvertToCpuMesh();

  if (!hCpuMesh.IsValid())
    return;

  plResourceLock<plCpuMeshResource> pCpuMesh(hCpuMesh, plResourceAcquireMode::BlockTillLoaded);

  plMeshResourceDescriptor md = pCpuMesh->GetDescriptor();

  md.SetMaterial(0, "Materials/Common/ColMesh.plMaterial");

  m_hMesh = plResourceManager::GetOrCreateResource<plMeshResource>(sColMeshName, std::move(md), "Collision Mesh Visualization");

  TriggerLocalBoundsUpdate();
}

void plJoltVisColMeshComponent::Initialize()
{
  SUPER::Initialize();

  GetWorld()->GetOrCreateComponentManager<plJoltVisColMeshComponentManager>()->EnqueueUpdate(GetHandle());
}

void plJoltVisColMeshComponent::OnDeactivated()
{
  SUPER::OnDeactivated();
  if (plRenderDataManager* pRenderDataManager = GetWorld()->GetModule<plRenderDataManager>())
    pRenderDataManager->DeleteInstanceData(m_InstanceDataOffset);
}

void plJoltVisColMeshComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!m_hMesh.IsValid())
    return;

  const bool bDynamic = GetOwner()->IsDynamic();
  auto hInstanceDataBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, GetOwner()->GetGlobalTransform(), m_InstanceDataOffset, GetUniqueIdForRendering());

  plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::AllowLoadingFallback);
  plArrayPtr<const plMeshResourceDescriptor::SubMesh> parts = pMesh->GetSubMeshes();

  for (plUInt32 uiPartIndex = 0; uiPartIndex < parts.GetCount(); ++uiPartIndex)
  {
    const plUInt32 uiMaterialIndex = parts[uiPartIndex].m_uiMaterialIndex;
    plMaterialResourceHandle hMaterial = pMesh->GetMaterials()[uiMaterialIndex];

    plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
    pRenderData->SetFallbackGlobalBounds(GetOwner()->GetGlobalBounds().GetBox());
    pRenderData->Fill(m_InstanceDataOffset, hInstanceDataBuffer, hMaterial, m_hMesh, uiMaterialIndex, uiPartIndex);

    msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitOpaque, plRenderData::Caching::IfStatic);
  }
}

//////////////////////////////////////////////////////////////////////////

void plJoltVisColMeshComponentManager::Initialize()
{
  SUPER::Initialize();

  plWorldModule::UpdateFunctionDesc desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltVisColMeshComponentManager::Update, this);
  desc.m_Phase = plWorldUpdatePhase::PreAsync;

  RegisterUpdateFunction(desc);

  plResourceManager::GetResourceEvents().AddEventHandler(plMakeDelegate(&plJoltVisColMeshComponentManager::ResourceEventHandler, this));
}

void plJoltVisColMeshComponentManager::Deinitialize()
{
  plResourceManager::GetResourceEvents().RemoveEventHandler(plMakeDelegate(&plJoltVisColMeshComponentManager::ResourceEventHandler, this));

  SUPER::Deinitialize();
}

void plJoltVisColMeshComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  plDeque<plComponentHandle> requireUpdate;
  plDeque<plJoltMeshResourceHandle> changedMeshes;
  {
    PL_LOCK(m_Mutex);
    m_RequireUpdate.Swap(requireUpdate);
    m_ChangedMeshes.Swap(changedMeshes);
  }

  // Resource notifications can arrive on loader threads. Resolve components only
  // here, while world component storage is stable.
  for (auto it = GetComponents(); it.IsValid(); it.Next())
  {
    if (changedMeshes.Contains(it->GetMesh()))
      requireUpdate.PushBack(it->GetHandle());
  }

  for (const auto& hComp : requireUpdate)
  {
    plJoltVisColMeshComponent* pComp = nullptr;
    if (!TryGetComponent(hComp, pComp))
      continue;

    pComp->CreateCollisionRenderMesh();
  }
}

void plJoltVisColMeshComponentManager::EnqueueUpdate(plComponentHandle hComponent)
{
  PL_LOCK(m_Mutex);
  m_RequireUpdate.PushBack(hComponent);
}

void plJoltVisColMeshComponentManager::ResourceEventHandler(const plResourceEvent& e)
{
  if ((e.m_Type == plResourceEvent::Type::ResourceContentUnloading || e.m_Type == plResourceEvent::Type::ResourceContentUpdated) && e.m_pResource->GetDynamicRTTI()->IsDerivedFrom<plJoltMeshResource>())
  {
    PL_LOCK(m_Mutex);

    m_ChangedMeshes.PushBack(plJoltMeshResourceHandle(static_cast<plJoltMeshResource*>(e.m_pResource)));
  }
}


PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltVisColMeshComponent);
