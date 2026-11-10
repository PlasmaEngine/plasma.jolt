#include <EnginePluginJolt/EnginePluginJoltPCH.h>

#include <EnginePluginJolt/ClothAsset/JoltClothContext.h>
#include <EnginePluginJolt/ClothAsset/JoltClothView.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

plJoltClothViewContext::plJoltClothViewContext(plJoltClothContext* pContext)
  : plEngineProcessViewContext(pContext)
{
  m_pContext = pContext;

  // Start with something valid.
  m_Camera.SetCameraMode(plCameraMode::PerspectiveFixedFovX, 45.0f, 0.05f, 10000.0f);
  m_Camera.LookAt(plVec3(1, 1, 1), plVec3::MakeZero(), plVec3(0.0f, 0.0f, 1.0f));
}

plJoltClothViewContext::~plJoltClothViewContext() = default;

bool plJoltClothViewContext::UpdateThumbnailCamera(const plBoundingBoxSphere& bounds)
{
  // from behind and to the side, where a cape or a coat shows best
  return !FocusCameraOnObject(m_Camera, bounds, 45.0f, -plVec3(-5, -2, 3));
}

plViewHandle plJoltClothViewContext::CreateView()
{
  plView* pView = nullptr;
  plRenderWorld::CreateView("Cloth Editor - View", pView);
  pView->SetCameraUsageHint(plCameraUsageHint::EditorView);

  pView->SetRenderPipelineResource(CreateDefaultRenderPipeline());

  plEngineProcessDocumentContext* pDocumentContext = GetDocumentContext();
  pView->SetWorld(pDocumentContext->GetWorld());
  pView->SetCamera(&m_Camera);
  return pView->GetHandle();
}

void plJoltClothViewContext::SetCamera(const plViewRedrawMsgToEngine* pMsg)
{
  if (m_pContext->m_bDisplayGrid)
  {
    plEngineProcessViewContext::DrawSimpleGrid();
  }

  plEngineProcessViewContext::SetCamera(pMsg);

  plStringBuilder sText;
  m_pContext->GetStatsText(sText);

  if (!sText.IsEmpty())
    plDebugRenderer::DrawInfoText(m_hView, plDebugTextPlacement::BottomLeft, "AssetStats", sText);
}
