#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <EditorFramework/InputContexts/OrbitCameraContext.h>
#include <EditorPluginJolt/Actions/JoltActions.h>
#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetWindow.h>

plDestructibleMeshAssetDocumentWindow::plDestructibleMeshAssetDocumentWindow(plAssetDocument* pDocument)
  : plEngineDocumentWindow(pDocument)
{
  SetMenuBarActions("DestructibleMeshAssetMenuBar");
  SetToolBarActions("DestructibleMeshAssetToolBar");

  // 3D View; its strip holds the Exploded View slider (plJoltExplodedViewAction)
  {
    SetTargetFramerate(25);

    m_ViewConfig.m_Camera.LookAt(plVec3(-1.6f, 0, 0), plVec3(0, 0, 0), plVec3(0, 0, 1));
    m_ViewConfig.ApplyPerspectiveSetting(90);

    m_pViewWidget = new plOrbitCamViewWidget(this, &m_ViewConfig);
    m_pViewWidget->ConfigureRelative(plVec3(0), plVec3(5.0f), plVec3(5, -2, 3), 2.0f);
    AddViewWidget(m_pViewWidget);
    m_pCameraFlyContext = PL_DEFAULT_NEW(plCameraMoveContext, this, m_pViewWidget);
    m_pCameraFlyContext->SetCamera(&m_ViewConfig.m_Camera);
    m_pCameraFlyContext->LoadState();

    m_pViewWidget->SetToolBarActions("DestructibleMeshAssetViewToolBar");
  }

  // the shell's properties panel follows the selection
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  QueryObjectBBox();
}

void plDestructibleMeshAssetDocumentWindow::SetExplodeFactor(float fFactor)
{
  m_fExplodeFactor = plMath::Clamp(fFactor, 0.0f, 2.0f);
  TriggerRedraw();
}

void plDestructibleMeshAssetDocumentWindow::SendRedrawMsg()
{
  // do not try to redraw while the process is crashed, it is obviously futile
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  // sent every redraw, so the state survives engine process restarts
  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "PreviewExplode";
    msg.m_PayloadValue = m_fExplodeFactor;
    GetDocument()->SendMessageToEngine(&msg);
  }

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(false);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }
}

void plDestructibleMeshAssetDocumentWindow::QueryObjectBBox(plInt32 iPurpose /*= 0*/)
{
  plQuerySelectionBBoxMsgToEngine msg;
  msg.m_uiViewID = 0xFFFFFFFF;
  msg.m_iPurpose = iPurpose;
  GetDocument()->SendMessageToEngine(&msg);
}

void plDestructibleMeshAssetDocumentWindow::SetCameraMode(int iMode)
{
  if (m_iCameraMode == iMode)
    return;
  m_iCameraMode = iMode;

  m_pViewWidget->m_InputContexts.Clear();
  if (iMode == 0) // Orbit
    m_pViewWidget->m_InputContexts.PushBack(m_pViewWidget->GetOrbitCamera());
  else            // Free fly
    m_pViewWidget->m_InputContexts.PushBack(m_pCameraFlyContext.Borrow());
}

void plDestructibleMeshAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plEngineDocumentWindow::InternalRedraw();
}

void plDestructibleMeshAssetDocumentWindow::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
{
  if (pMsg->GetDynamicRTTI()->IsDerivedFrom<plQuerySelectionBBoxResultMsgToEditor>())
  {
    const plQuerySelectionBBoxResultMsgToEditor* pMessage = static_cast<const plQuerySelectionBBoxResultMsgToEditor*>(pMsg);

    if (pMessage->m_vCenter.IsValid() && pMessage->m_vHalfExtents.IsValid())
    {
      m_pViewWidget->SetOrbitVolume(pMessage->m_vCenter, pMessage->m_vHalfExtents.CompMax(plVec3(0.1f)));
    }
    else
    {
      // try again
      QueryObjectBBox(pMessage->m_iPurpose);
    }

    return;
  }

  plEngineDocumentWindow::ProcessMessageEventHandler(pMsg);
}
