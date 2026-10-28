#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <EditorFramework/InputContexts/OrbitCameraContext.h>
#include <EditorPluginJolt/CollisionMeshAsset/JoltCollisionMeshAssetWindow.h>

plJoltCollisionMeshAssetDocumentWindow::plJoltCollisionMeshAssetDocumentWindow(plAssetDocument* pDocument)
  : plEngineDocumentWindow(pDocument)
{
  SetMenuBarActions("JoltCollisionMeshAssetMenuBar");
  SetToolBarActions("JoltCollisionMeshAssetToolBar");

  // 3D View
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

    m_pViewWidget->SetToolBarActions("JoltCollisionMeshAssetViewToolBar");
  }

  // the shell's properties panel follows the selection
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  m_pAssetDoc = pDocument;
}


void plJoltCollisionMeshAssetDocumentWindow::SendRedrawMsg()
{
  // do not try to redraw while the process is crashed, it is obviously futile
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(false);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }

  QueryObjectBBox();
}

void plJoltCollisionMeshAssetDocumentWindow::QueryObjectBBox(plInt32 iPurpose /*= 0*/)
{
  plQuerySelectionBBoxMsgToEngine msg;
  msg.m_uiViewID = 0xFFFFFFFF;
  msg.m_iPurpose = iPurpose;
  GetDocument()->SendMessageToEngine(&msg);
}

void plJoltCollisionMeshAssetDocumentWindow::SetCameraMode(int iMode)
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

void plJoltCollisionMeshAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plEngineDocumentWindow::InternalRedraw();
}

void plJoltCollisionMeshAssetDocumentWindow::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
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
