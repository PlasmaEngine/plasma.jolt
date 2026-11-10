#include <EnginePluginJolt/EnginePluginJoltPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessApp.h>
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

void plJoltClothViewContext::HandleViewMessage(const plEditorEngineViewMsg* pMsg)
{
  if (const plViewPickingMsgToEngine* pPick = plDynamicCast<const plViewPickingMsgToEngine*>(pMsg))
  {
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(m_hView, pView))
    {
      pView->SetRenderPassProperty("EditorPickingPass", "Active", true);
      pView->SetRenderPassProperty("EditorPickingPass", "PickSelected", true);
    }

    PickObjectAt(pPick->m_uiPickPosX, pPick->m_uiPickPosY);
    return;
  }

  plEngineProcessViewContext::HandleViewMessage(pMsg);
}

void plJoltClothViewContext::PickObjectAt(plUInt16 x, plUInt16 y)
{
  // remote processes do not support picking
  if (plEditorEngineProcessApp::GetSingleton()->IsRemoteMode())
    return;

  plViewPickingResultMsgToEditor res;
  PL_SCOPE_EXIT(SendViewMessage(&res));

  plView* pView = nullptr;
  if (!plRenderWorld::TryGetView(m_hView, pView))
    return;

  pView->SetRenderPassProperty("EditorPickingPass", "PickingPosition", plVec2(x, y));

  if (!pView->IsRenderPassReadBackPropertyExisting("EditorPickingPass", "PickedPosition"))
    return;

  const plVariant pickedPosition = pView->GetRenderPassReadBackProperty("EditorPickingPass", "PickedPosition");
  if (!pickedPosition.IsA<plVec3>())
    return;

  const plUInt32 uiPickingID = pView->GetRenderPassReadBackProperty("EditorPickingPass", "PickedID").ConvertTo<plUInt32>();
  res.m_vPickedNormal = pView->GetRenderPassReadBackProperty("EditorPickingPass", "PickedNormal").ConvertTo<plVec3>();
  res.m_vPickingRayStartPosition = pView->GetRenderPassReadBackProperty("EditorPickingPass", "PickedRayStartPosition").ConvertTo<plVec3>();
  res.m_vPickedPosition = pickedPosition.ConvertTo<plVec3>();

  // the handles are the only things here that the editor knows by an id
  res.m_OtherGuid = GetDocumentContext()->m_Context.m_OtherPickingMap.GetGuid(uiPickingID & 0x00FFFFFF);
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

  m_pContext->DrawStressTimeline(m_hView, static_cast<float>(pMsg->m_uiWindowWidth), static_cast<float>(pMsg->m_uiWindowHeight));
}
