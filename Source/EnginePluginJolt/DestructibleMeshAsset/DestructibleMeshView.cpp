#include <EnginePluginJolt/EnginePluginJoltPCH.h>

#include <EnginePluginJolt/DestructibleMeshAsset/DestructibleMeshContext.h>
#include <EnginePluginJolt/DestructibleMeshAsset/DestructibleMeshView.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

plDestructibleMeshViewContext::plDestructibleMeshViewContext(plDestructibleMeshContext* pContext)
  : plEngineProcessViewContext(pContext)
{
  m_pContext = pContext;

  // Start with something valid.
  m_Camera.SetCameraMode(plCameraMode::PerspectiveFixedFovX, 45.0f, 0.05f, 10000.0f);
  m_Camera.LookAt(plVec3(1, 1, 1), plVec3::MakeZero(), plVec3(0.0f, 0.0f, 1.0f));
}

plDestructibleMeshViewContext::~plDestructibleMeshViewContext() = default;

bool plDestructibleMeshViewContext::UpdateThumbnailCamera(const plBoundingBoxSphere& bounds)
{
  return !FocusCameraOnObject(m_Camera, bounds, 45.0f, -plVec3(5, -2, 3));
}

plViewHandle plDestructibleMeshViewContext::CreateView()
{
  plView* pView = nullptr;
  plRenderWorld::CreateView("Destructible Mesh Editor - View", pView);
  pView->SetCameraUsageHint(plCameraUsageHint::EditorView);

  pView->SetRenderPipelineResource(CreateDefaultRenderPipeline());

  plEngineProcessDocumentContext* pDocumentContext = GetDocumentContext();
  pView->SetWorld(pDocumentContext->GetWorld());
  pView->SetCamera(&m_Camera);
  return pView->GetHandle();
}

void plDestructibleMeshViewContext::SetCamera(const plViewRedrawMsgToEngine* pMsg)
{
  // the chunk preview objects are (re)built lazily, once the transformed resource is available
  m_pContext->UpdatePreview();

  if (m_pContext->m_bDisplayGrid)
  {
    plEngineProcessViewContext::DrawSimpleGrid();
  }

  plEngineProcessViewContext::SetCamera(pMsg);

  auto hResource = m_pContext->GetResource();
  if (hResource.IsValid())
  {
    plResourceLock<plJoltDestructibleMeshResource> pResource(hResource, plResourceAcquireMode::AllowLoadingFallback_NeverFail);

    if (pResource.GetAcquireResult() == plResourceAcquireResult::Final)
    {
      const auto& desc = pResource->GetDescriptor();

      plUInt32 uiNumAnchorBonds = 0;
      plUInt32 uiNumAnchorChunks = 0;
      plUInt32 uiNumLeafChunks = 0;
      plUInt16 uiMaxDepth = 0;

      for (const auto& bond : desc.m_Bonds)
      {
        if (bond.m_uiChunkB == plDestructibleBond::WorldAnchor)
        {
          ++uiNumAnchorBonds;
        }
      }

      for (const auto& chunk : desc.m_Chunks)
      {
        if ((chunk.m_uiFlags & plDestructibleChunkFlags::WorldAnchor) != 0)
        {
          ++uiNumAnchorChunks;
        }

        if (chunk.m_uiNumChildren == 0)
        {
          ++uiNumLeafChunks;
        }

        uiMaxDepth = plMath::Max(uiMaxDepth, chunk.m_uiDepth);
      }

      const plBoundingBox bbox = desc.m_Bounds.GetBox();

      plStringBuilder sText;
      sText.AppendFormat("Chunks: \t{} ({} leaves, {} levels)\n", desc.m_Chunks.GetCount() - 1, uiNumLeafChunks, uiMaxDepth);
      sText.AppendFormat("Bonds: \t{}\n", desc.m_Bonds.GetCount() - uiNumAnchorBonds);
      sText.AppendFormat("Anchor Chunks: \t{}\n", uiNumAnchorChunks);
      sText.AppendFormat("Bounding Box: \twidth={0}, depth={1}, height={2}", plArgF(bbox.GetHalfExtents().x * 2, 2),
        plArgF(bbox.GetHalfExtents().y * 2, 2), plArgF(bbox.GetHalfExtents().z * 2, 2));

      plDebugRenderer::DrawInfoText(m_hView, plDebugTextPlacement::BottomLeft, "AssetStats", sText);
    }
  }
}

PL_STATICLINK_FILE(EnginePluginJolt, EnginePluginJolt_DestructibleMeshAsset_DestructibleMeshView);
