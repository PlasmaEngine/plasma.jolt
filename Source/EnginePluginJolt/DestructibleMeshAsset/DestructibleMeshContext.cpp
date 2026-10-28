#include <EnginePluginJolt/EnginePluginJoltPCH.h>

#include <EnginePluginJolt/DestructibleMeshAsset/DestructibleMeshContext.h>
#include <EnginePluginJolt/DestructibleMeshAsset/DestructibleMeshView.h>

#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Lights/SkyLightComponent.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Meshes/MeshResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plDestructibleMeshContext, 1, plRTTIDefaultAllocator<plDestructibleMeshContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "DestructibleMesh"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plDestructibleMeshContext::plDestructibleMeshContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::CreateWorld)
{
}

void plDestructibleMeshContext::HandleMessage(const plEditorEngineDocumentMsg* pDocMsg)
{
  if (auto pMsg = plDynamicCast<const plQuerySelectionBBoxMsgToEngine*>(pDocMsg))
  {
    QuerySelectionBBox(pMsg);
    return;
  }

  if (auto pMsg = plDynamicCast<const plSimpleDocumentConfigMsgToEngine*>(pDocMsg))
  {
    if (pMsg->m_sWhatToDo == "PreviewExplode")
    {
      SetExplodeFactor(pMsg->m_PayloadValue.ConvertTo<float>());
      return;
    }

    if (pMsg->m_sWhatToDo == "CommonAssetUiState")
    {
      if (pMsg->m_sPayload == "Grid")
      {
        m_bDisplayGrid = pMsg->m_PayloadValue.ConvertTo<float>() > 0;
        return;
      }
    }
  }

  plEngineProcessDocumentContext::HandleMessage(pDocMsg);
}

void plDestructibleMeshContext::OnInitialize()
{
  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  plGameObjectDesc obj;
  obj.m_sName.Assign("DestructiblePreview");
  pWorld->CreateObject(obj, m_pRootObject);

  const plTag& tagCastShadows = plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow");
  m_pRootObject->SetTag(tagCastShadows);

  plStringBuilder sResourceGuid;
  plConversionUtils::ToString(GetDocumentGuid(), sResourceGuid);
  m_hDestructible = plResourceManager::LoadResource<plJoltDestructibleMeshResource>(sResourceGuid);

  // lights (same setup as the scene editor's default lighting)
  {
    plGameObjectDesc objLight;
    objLight.m_sName.Assign("SkyLight");

    plGameObject* pObj;
    pWorld->CreateObject(objLight, pObj);

    plSkyLightComponent* pSkyLight = nullptr;
    plSkyLightComponent::CreateComponent(pObj, pSkyLight);
    pSkyLight->SetCubeMapFile("{ 0b202e08-a64f-465d-b38e-15b81d161822 }");
    pSkyLight->SetReflectionProbeMode(plReflectionProbeMode::Static);
    // The preview lighting is unitless and tuned by eye: PI gives the sky the fill it was tuned with.
    pSkyLight->SetDiffuseIntensity(plMath::Pi<float>());
  }

  {
    plGameObjectDesc objLight;
    objLight.m_sName.Assign("DirLight");
    objLight.m_LocalRotation = plQuat::MakeFromEulerAngles(plAngle::MakeFromDegree(-14.510815f), plAngle::MakeFromDegree(43.07951f), plAngle::MakeFromDegree(93.223808f));

    plGameObject* pObj;
    pWorld->CreateObject(objLight, pObj);

    plDirectionalLightComponent* pDirLight = nullptr;
    plDirectionalLightComponent::CreateComponent(pObj, pDirLight);
    pDirLight->SetIntensity(10.0f);
  }
}

void plDestructibleMeshContext::UpdatePreview()
{
  if (!m_hDestructible.IsValid() || m_pRootObject == nullptr)
    return;

  plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::AllowLoadingFallback_NeverFail);

  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  if (pResource->GetCurrentResourceChangeCounter() == m_uiRebuiltChangeCounter)
    return;

  const auto& desc = pResource->GetDescriptor();
  const plMeshResourceHandle& hRenderMesh = pResource->GetRenderMesh();

  if (!hRenderMesh.IsValid() || desc.m_Chunks.GetCount() < 2)
    return;

  plResourceLock<plMeshResource> pMesh(hRenderMesh, plResourceAcquireMode::BlockTillLoaded_NeverFail);

  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  // throw away the previous preview objects
  for (const auto& hObject : m_ChunkObjects)
  {
    pWorld->DeleteObjectNow(hObject);
  }

  m_ChunkObjects.Clear();
  m_ChunkOffsets.Clear();

  const auto subMeshes = pMesh->GetSubMeshes();
  const auto materials = pMesh->GetMaterials();

  plStringBuilder sChunkMeshId, sDocGuid;
  plConversionUtils::ToString(GetDocumentGuid(), sDocGuid);

  const plVec3 vRootCentroid = desc.m_Chunks[0].m_vCentroid;

  for (plUInt32 uiChunk = 1; uiChunk < desc.m_Chunks.GetCount(); ++uiChunk)
  {
    const auto& chunk = desc.m_Chunks[uiChunk];

    // only leaves are shown - they partition the mesh completely
    if (chunk.m_uiNumChildren > 0 || chunk.m_uiNumRenderSubMeshes == 0)
      continue;

    // per-chunk mesh resource that shares the full render mesh buffer but only draws this chunk's sub-meshes
    sChunkMeshId.SetFormat("{}-chunk{}-{}", sDocGuid, uiChunk, pResource->GetCurrentResourceChangeCounter());

    plMeshResourceHandle hChunkMesh = plResourceManager::GetExistingResource<plMeshResource>(sChunkMeshId);

    if (!hChunkMesh.IsValid())
    {
      plMeshResourceDescriptor chunkDesc;
      chunkDesc.UseExistingMeshBuffer(pMesh->GetMeshBuffer());

      for (plUInt32 m = 0; m < materials.GetCount(); ++m)
      {
        plStringView sMaterial = plMaterialResource::GetDefaultMaterialFileName(plMaterialResource::DefaultMaterialType::OpenPBR);
        if (materials[m].IsValid())
        {
          sMaterial = materials[m].GetResourceID();
        }

        chunkDesc.SetMaterial(m, sMaterial);
      }

      for (plUInt32 s = 0; s < chunk.m_uiNumRenderSubMeshes; ++s)
      {
        const plUInt32 uiSubMesh = chunk.m_uiFirstRenderSubMesh + s;

        if (uiSubMesh >= subMeshes.GetCount())
          continue;

        chunkDesc.AddSubMesh(subMeshes[uiSubMesh].m_uiPrimitiveCount, subMeshes[uiSubMesh].m_uiFirstPrimitive, subMeshes[uiSubMesh].m_uiMaterialIndex);
      }

      chunkDesc.SetBounds(chunk.m_Bounds);

      hChunkMesh = plResourceManager::CreateResource<plMeshResource>(sChunkMeshId, std::move(chunkDesc), "Destructible chunk preview");
    }

    plGameObjectDesc obj;
    obj.m_sName.Assign("Chunk");
    obj.m_hParent = m_pRootObject->GetHandle();
    // The explode slider changes these transforms at runtime. Dynamic objects keep their mesh
    // instance data out of the static render cache, so every slider step is visible immediately.
    obj.m_bDynamic = true;

    // layered explode: full offset along the top-level chunk direction, half along each deeper level,
    // so the hierarchy reads visually when the slider is dragged
    plVec3 vExplodeDir = plVec3::MakeZero();
    float fLevelScale = 1.0f;

    {
      plHybridArray<plUInt32, 4> ancestorChain; // from level 1 down to this chunk
      plUInt32 uiWalk = uiChunk;
      while (uiWalk != 0 && uiWalk != plInvalidIndex)
      {
        ancestorChain.PushBack(uiWalk);
        uiWalk = desc.m_Chunks[uiWalk].m_uiParentIndex;
      }

      plVec3 vPrevCentroid = vRootCentroid;
      for (plUInt32 a = ancestorChain.GetCount(); a > 0; --a)
      {
        const auto& ancestor = desc.m_Chunks[ancestorChain[a - 1]];
        vExplodeDir += (ancestor.m_vCentroid - vPrevCentroid) * fLevelScale;
        vPrevCentroid = ancestor.m_vCentroid;
        fLevelScale *= 0.5f;
      }
    }

    plGameObject* pChunkObject = nullptr;
    m_ChunkObjects.PushBack(pWorld->CreateObject(obj, pChunkObject));
    m_ChunkOffsets.PushBack(vExplodeDir);

    plMeshComponent* pMeshComponent = nullptr;
    plMeshComponent::CreateComponent(pChunkObject, pMeshComponent);
    pMeshComponent->SetMesh(hChunkMesh);

    // subtle per-chunk tint so neighboring chunks with the same material remain distinguishable
    const float fHue = plMath::Fraction((float)uiChunk * 0.618034f) * 360.0f;
    pMeshComponent->SetColor(plColor::MakeHSV(fHue, 0.15f, 1.0f));
  }

  m_uiRebuiltChangeCounter = pResource->GetCurrentResourceChangeCounter();

  ApplyExplode();
}

void plDestructibleMeshContext::SetExplodeFactor(float fExplode)
{
  if (m_fExplode == fExplode)
    return;

  m_fExplode = fExplode;

  PL_LOCK(m_pWorld->GetWriteMarker());
  ApplyExplode();
}

void plDestructibleMeshContext::ApplyExplode()
{
  // assumes the world write lock is held
  for (plUInt32 i = 0; i < m_ChunkObjects.GetCount(); ++i)
  {
    plGameObject* pObject = nullptr;
    if (m_pWorld->TryGetObject(m_ChunkObjects[i], pObject))
    {
      pObject->SetLocalPosition(m_ChunkOffsets[i] * m_fExplode);
      pObject->UpdateGlobalTransformAndBounds();
    }
  }
}

plEngineProcessViewContext* plDestructibleMeshContext::CreateViewContext()
{
  return PL_DEFAULT_NEW(plDestructibleMeshViewContext, this);
}

void plDestructibleMeshContext::DestroyViewContext(plEngineProcessViewContext* pContext)
{
  PL_DEFAULT_DELETE(pContext);
}

bool plDestructibleMeshContext::UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext)
{
  plBoundingBoxSphere bounds = GetWorldBounds(m_pWorld);

  plDestructibleMeshViewContext* pMeshViewContext = static_cast<plDestructibleMeshViewContext*>(pThumbnailViewContext);
  return pMeshViewContext->UpdateThumbnailCamera(bounds);
}

void plDestructibleMeshContext::QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg)
{
  if (m_pRootObject == nullptr)
    return;

  plBoundingBoxSphere bounds = plBoundingBoxSphere::MakeInvalid();

  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    m_pRootObject->UpdateLocalBounds();
    m_pRootObject->UpdateGlobalTransformAndBounds();
    const auto& b = m_pRootObject->GetGlobalBounds();

    if (b.IsValid())
      bounds.ExpandToInclude(b);
  }

  if (!bounds.IsValid())
  {
    // chunk objects are children; use their accumulated bounds instead
    if (m_hDestructible.IsValid())
    {
      plResourceLock<plJoltDestructibleMeshResource> pResource(m_hDestructible, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
      if (pResource.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        bounds = pResource->GetDescriptor().m_Bounds;
      }
    }
  }

  const plQuerySelectionBBoxMsgToEngine* msg = static_cast<const plQuerySelectionBBoxMsgToEngine*>(pMsg);

  plQuerySelectionBBoxResultMsgToEditor res;
  res.m_uiViewID = msg->m_uiViewID;
  res.m_iPurpose = msg->m_iPurpose;
  res.m_vCenter = bounds.m_vCenter;
  res.m_vHalfExtents = bounds.m_vBoxHalfExtends;
  res.m_DocumentGuid = pMsg->m_DocumentGuid;

  SendProcessMessage(&res);
}

PL_STATICLINK_FILE(EnginePluginJolt, EnginePluginJolt_DestructibleMeshAsset_DestructibleMeshContext);
