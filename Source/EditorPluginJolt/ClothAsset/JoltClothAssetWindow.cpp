#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <EditorFramework/InputContexts/OrbitCameraContext.h>
#include <EditorFramework/Preferences/EditorPreferences.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetManager.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetWindow.h>
#include <EditorUi/Runtime.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/PropertyGrid/ManipulatorManager.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <JoltPlugin/CharacterCloth/JoltClothStressTest.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  // what a map's brush value of 100 percent stands for, where the asset has no property for it
  constexpr float s_fBackstopDistanceRange = 0.2f;
  constexpr float s_fBackstopRadiusRange = 1.0f;
} // namespace

//////////////////////////////////////////////////////////////////////////

plJoltClothPaintContext::plJoltClothPaintContext(plJoltClothAssetDocumentWindow* pWindow, plEngineViewWidget* pView)
  : m_pWindow(pWindow)
{
  SetOwner(pWindow, pView);
}

void plJoltClothPaintContext::DoFocusLost(bool bCancel)
{
  if (!m_bStroke)
    return;

  m_bStroke = false;
  m_pWindow->EndStroke(true);
}

plEditorInput plJoltClothPaintContext::DoMousePressEvent(const plEditorInputEvent& e)
{
  // Alt and the other buttons belong to the camera
  if (e.m_uiButton != plEditorInputEvent::LeftButton || e.HasModifier(plEditorInputEvent::Alt))
    return plEditorInput::MayBeHandledByOthers;

  if (!m_pWindow->BeginStroke(e))
    return plEditorInput::MayBeHandledByOthers;

  m_bStroke = true;
  MakeActiveInputContext();
  return plEditorInput::WasExclusivelyHandled;
}

plEditorInput plJoltClothPaintContext::DoMouseReleaseEvent(const plEditorInputEvent& e)
{
  if (!m_bStroke || e.m_uiButton != plEditorInputEvent::LeftButton)
    return plEditorInput::MayBeHandledByOthers;

  m_bStroke = false;
  m_pWindow->EndStroke(false);

  MakeActiveInputContext(false);
  return plEditorInput::WasExclusivelyHandled;
}

plEditorInput plJoltClothPaintContext::DoMouseMoveEvent(const plEditorInputEvent& e)
{
  if (m_bStroke)
  {
    m_pWindow->ContinueStroke(e);
    return plEditorInput::WasExclusivelyHandled;
  }

  m_pWindow->HoverBrush(e);
  return plEditorInput::MayBeHandledByOthers;
}

//////////////////////////////////////////////////////////////////////////

plJoltClothAssetDocumentWindow::plJoltClothAssetDocumentWindow(plJoltClothAssetDocument* pDocument)
  : plEngineDocumentWindow(pDocument)
{
  SetMenuBarActions("JoltClothAssetMenuBar");
  SetToolBarActions("JoltClothAssetToolBar");

  // 3D View
  {
    plEditorPreferencesUser* pPreferences = plPreferences::QueryPreferences<plEditorPreferencesUser>();
    SetTargetFramerate(pPreferences->GetMaxFramerate());

    m_ViewConfig.m_Camera.LookAt(plVec3(-1.6f, 0, 0), plVec3(0, 0, 0), plVec3(0, 0, 1));
    m_ViewConfig.ApplyPerspectiveSetting(90);

    m_pViewWidget = new plOrbitCamViewWidget(this, &m_ViewConfig);
    m_pViewWidget->ConfigureRelative(plVec3(0, 0, 1), plVec3(5.0f), plVec3(-5, -2, 3), 2.0f);
    AddViewWidget(m_pViewWidget);

    m_pCameraFlyContext = PL_DEFAULT_NEW(plCameraMoveContext, this, m_pViewWidget);
    m_pCameraFlyContext->SetCamera(&m_ViewConfig.m_Camera);
    m_pCameraFlyContext->LoadState();

    m_pPaintContext = PL_DEFAULT_NEW(plJoltClothPaintContext, this, m_pViewWidget);
    m_pHandleContext = PL_DEFAULT_NEW(plSelectionContext, this, m_pViewWidget, &m_ViewConfig.m_Camera);

    m_pViewWidget->SetToolBarActions("JoltClothAssetViewToolBar");
  }

  // the shell's properties panel follows the selection
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  GetDocument()->GetCommandHistory()->m_Events.AddEventHandler(plMakeDelegate(&plJoltClothAssetDocumentWindow::CommandEventHandler, this));
  GetDocument()->m_EventsOne.AddEventHandler(plMakeDelegate(&plJoltClothAssetDocumentWindow::DocumentSavedHandler, this));

  SendLiveResourcePreview();
}

plJoltClothAssetDocumentWindow::~plJoltClothAssetDocumentWindow()
{
  GetDocument()->GetCommandHistory()->m_Events.RemoveEventHandler(plMakeDelegate(&plJoltClothAssetDocumentWindow::CommandEventHandler, this));
  GetDocument()->m_EventsOne.RemoveEventHandler(plMakeDelegate(&plJoltClothAssetDocumentWindow::DocumentSavedHandler, this));

  // the view owns no input context: they go before it
  m_pViewWidget->m_InputContexts.Clear();

  RestoreResource();
}

plJoltClothAssetDocument* plJoltClothAssetDocumentWindow::GetClothDocument() const
{
  return static_cast<plJoltClothAssetDocument*>(GetDocument());
}

void plJoltClothAssetDocumentWindow::SetCameraMode(int iMode)
{
  if (m_iCameraMode == iMode)
    return;

  m_iCameraMode = iMode;
  UpdateInputContexts();
}

void plJoltClothAssetDocumentWindow::UpdateInputContexts()
{
  m_pViewWidget->m_InputContexts.Clear();

  // the paint tool, the drag test and the stress timeline see the mouse first and leave the camera what they do not use
  if (m_PaintTool != plJoltClothPaintTool::Off || m_bDragTest || m_bStressShown)
    m_pViewWidget->m_InputContexts.PushBack(m_pPaintContext.Borrow());

  // grabs the collider handles
  if (m_bHandlesShown)
    m_pViewWidget->m_InputContexts.PushBack(m_pHandleContext.Borrow());

  if (m_iCameraMode == 0)
    m_pViewWidget->m_InputContexts.PushBack(m_pViewWidget->GetOrbitCamera());
  else
    m_pViewWidget->m_InputContexts.PushBack(m_pCameraFlyContext.Borrow());
}

void plJoltClothAssetDocumentWindow::SetPaintTool(plJoltClothPaintTool::Enum tool)
{
  if (m_PaintTool == tool)
    return;

  if (m_bStroke)
    EndStroke(true);

  m_PaintTool = tool;
  m_bBrushVisible = false;
  m_bPaintValuesDirty = true;

  // painting happens on the bind pose, where there is nothing to drag
  if (m_PaintTool != plJoltClothPaintTool::Off)
  {
    m_bDragTest = false;
    m_vDragOffset.SetZero();
  }

  UpdateInputContexts();
}

void plJoltClothAssetDocumentWindow::SetDragTest(bool bOn)
{
  if (m_bDragTest == bOn)
    return;

  if (bOn)
    SetPaintTool(plJoltClothPaintTool::Off);

  m_bDragTest = bOn;
  m_bDragging = false;
  m_vDragOffset.SetZero();

  UpdateInputContexts();
}

bool plJoltClothAssetDocumentWindow::PickGround(const plEditorInputEvent& e, plVec3& out_vPosition) const
{
  plVec3 vStart, vDirection;
  if (m_pViewWidget->GetPickingRay(static_cast<plUInt16>(plMath::Max(0.0f, e.m_vPosition.x)), static_cast<plUInt16>(plMath::Max(0.0f, e.m_vPosition.y)), vStart, vDirection).Failed())
    return false;

  // The plane through the character's hip that faces the camera: the cursor meets it from every camera angle, also
  // from below hip height. What the character moves by is the part of the drag that runs along the ground.
  const plVec3 vNormal = m_ViewConfig.m_Camera.GetCenterDirForwards();
  const plVec3 vHip(0, 0, 1.0f);

  const float fFacing = vDirection.Dot(vNormal);
  if (fFacing < 1.0e-4f)
    return false;

  out_vPosition = vStart + vDirection * ((vHip - vStart).Dot(vNormal) / fFacing);
  return true;
}

void plJoltClothAssetDocumentWindow::SendPreviewCommand(plStringView sWhat)
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = sWhat;
  GetDocument()->SendMessageToEngine(&msg);
}

void plJoltClothAssetDocumentWindow::Teleport()
{
  SendPreviewCommand("Teleport");
}

void plJoltClothAssetDocumentWindow::TurnTest()
{
  SendPreviewCommand("Turn");
}

void plJoltClothAssetDocumentWindow::SetPaintMap(plJoltClothPaintMap::Enum map)
{
  if (m_PaintMap == map)
    return;

  if (m_bStroke)
    EndStroke(true);

  m_PaintMap = map;
  m_bPaintValuesDirty = true;
}

void plJoltClothAssetDocumentWindow::SendRedrawMsg()
{
  // do not try to redraw while the process is crashed, it is obviously futile
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  plJoltClothAssetDocument* pDoc = GetClothDocument();
  const bool bPainting = m_PaintTool != plJoltClothPaintTool::Off;

  {
    const plHybridArray<plPropertySelection, 8>* pHandles = nullptr;
    const bool bHandles = plManipulatorManager::GetSingleton()->GetActiveManipulator(pDoc, pHandles) != nullptr && pHandles != nullptr && !pHandles->IsEmpty();

    if (m_bHandlesShown != bHandles)
    {
      m_bHandlesShown = bHandles;
      UpdateInputContexts();
    }

    if (m_bHandlesShown)
    {
      const plVec2 vSize = m_pViewWidget->GetViewportSize();
      m_pHandleContext->SetWindowConfig(plVec2I32(static_cast<plInt32>(vSize.x), static_cast<plInt32>(vSize.y)));

      // the handles follow the joints as the preview moves them
      plStringBuilder sJoints;
      pDoc->GetColliderJoints(sJoints);

      plSimpleDocumentConfigMsgToEngine msg;
      msg.m_sWhatToDo = "JointTransforms";
      msg.m_sPayload = sJoints;
      pDoc->SendMessageToEngine(&msg);
    }
  }

  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "PreviewMesh";
    msg.m_sPayload = pDoc->GetProperties()->m_sMesh;
    pDoc->SendMessageToEngine(&msg);
  }

  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "PreviewClip";
    msg.m_sPayload = pDoc->GetProperties()->m_sPreviewClip;
    pDoc->SendMessageToEngine(&msg);
  }

  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "ClothView";
    msg.m_PayloadValue = static_cast<plInt32>(m_ViewMode);
    pDoc->SendMessageToEngine(&msg);
  }

  {
    // painting happens on the bind pose: the preview stands still and shows the painted map
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "PaintMode";
    msg.m_PayloadValue = bPainting;
    pDoc->SendMessageToEngine(&msg);
  }

  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "Wind";
    msg.m_PayloadValue = m_fWind;
    pDoc->SendMessageToEngine(&msg);
  }

  {
    // how far the drag test pulls the character from its place; zero lets it swing back
    plStringBuilder sOffset;
    sOffset.SetFormat("{} {}", plArgF(m_vDragOffset.x, 4), plArgF(m_vDragOffset.y, 4));

    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "DragOffset";
    msg.m_sPayload = sOffset;
    pDoc->SendMessageToEngine(&msg);
  }

  {
    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "SimulationSpeed";

    if (pDoc->GetCommonAssetUiState(plCommonAssetUiState::Pause) != 0.0)
      msg.m_PayloadValue = 0.0;
    else
      msg.m_PayloadValue = pDoc->GetCommonAssetUiState(plCommonAssetUiState::SimulationSpeed);

    pDoc->SendMessageToEngine(&msg);
  }

  if (bPainting)
  {
    if (m_bPaintValuesDirty)
      SendPaintValues();

    plSimpleDocumentConfigMsgToEngine msg;
    msg.m_sWhatToDo = "PaintBrush";

    if (m_bBrushVisible)
    {
      plStringBuilder sBrush;
      sBrush.SetFormat("{} {} {} {}", plArgF(m_vBrushWorldPosition.x, 4), plArgF(m_vBrushWorldPosition.y, 4), plArgF(m_vBrushWorldPosition.z, 4), plArgF(m_fBrushRadius, 4));
      msg.m_sPayload = sBrush;
    }

    pDoc->SendMessageToEngine(&msg);
  }

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(m_bHandlesShown);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }

  QueryObjectBBox();
}

void plJoltClothAssetDocumentWindow::QueryObjectBBox(plInt32 iPurpose /*= 0*/)
{
  plQuerySelectionBBoxMsgToEngine msg;
  msg.m_uiViewID = 0xFFFFFFFF;
  msg.m_iPurpose = iPurpose;
  GetDocument()->SendMessageToEngine(&msg);
}

void plJoltClothAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plEngineDocumentWindow::InternalRedraw();
}

void plJoltClothAssetDocumentWindow::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
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

  // The preview has loaded the cloth from disk, which may be older than the document or not there at all:
  // it gets the current cook.
  if (const plSimpleDocumentConfigMsgToEditor* pReady = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg); pReady != nullptr && pReady->m_sWhatToDo == "ClothPreviewReady")
  {
    SendCookedResource();
    m_bPaintValuesDirty = true;
    return;
  }

  if (const plSimpleDocumentConfigMsgToEditor* pJoints = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg); pJoints != nullptr && pJoints->m_sWhatToDo == "JointTransforms")
  {
    // "joint|px py pz qx qy qz qw;" for every joint that carries one of the asset's own colliders
    plHybridArray<plStringView, 16> entries;
    pJoints->m_sPayload.GetView().Split(false, entries, ";");

    for (plStringView sEntry : entries)
    {
      const char* szSeparator = sEntry.FindSubString("|");
      if (szSeparator == nullptr)
        continue;

      plStringBuilder sValues = plStringView(szSeparator + 1, sEntry.GetEndPointer());
      const char* szText = sValues.GetData();

      double v[7] = {0, 0, 0, 0, 0, 0, 1};
      bool bValid = true;

      for (double& fValue : v)
        bValid = bValid && plConversionUtils::StringToFloat(szText, fValue, &szText).Succeeded();

      if (!bValid)
        continue;

      plQuat qRotation(static_cast<float>(v[3]), static_cast<float>(v[4]), static_cast<float>(v[5]), static_cast<float>(v[6]));
      qRotation.Normalize();

      GetClothDocument()->SetJointTransform(plStringView(sEntry.GetStartPointer(), szSeparator), plTransform(plVec3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])), qRotation));
    }

    return;
  }

  if (const plSimpleDocumentConfigMsgToEditor* pShown = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg); pShown != nullptr && pShown->m_sWhatToDo == "StressFrameShown")
  {
    // the frame is on screen: go on to the one the mouse has reached since
    m_bStressFramePending = false;

    if (m_bStressShown && m_fStressFrameWanted != m_fStressFrameAsked)
      ShowStressFrame(m_fStressFrameWanted);

    return;
  }

  if (const plSimpleDocumentConfigMsgToEditor* pResult = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg); pResult != nullptr && pResult->m_sWhatToDo == "StressResult")
  {
    ShowStressResult(pResult->m_sPayload, pResult->m_PayloadValue);
    return;
  }

  plEngineDocumentWindow::ProcessMessageEventHandler(pMsg);
}

void plJoltClothAssetDocumentWindow::RunStressTest()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  // with a result on screen the button takes it away again
  if (m_bStressShown)
  {
    m_bStressShown = false;
    m_bStressScrub = false;
    UpdateInputContexts();

    SendPreviewCommand("StressLeave");
    return;
  }

  // the motion script, then the preview clip and the clips listed for the test: "clip|name" entries
  const plJoltClothAssetProperties* pProp = GetClothDocument()->GetProperties();

  plHybridArray<plString, 8> clips;
  if (!pProp->m_sPreviewClip.IsEmpty())
    clips.PushBack(pProp->m_sPreviewClip);

  for (const plString& sClip : pProp->m_StressClips)
  {
    if (!sClip.IsEmpty() && !clips.Contains(sClip))
      clips.PushBack(sClip);
  }

  plStringBuilder sClips;
  for (const plString& sClip : clips)
  {
    plStringBuilder sName = "clip";

    if (auto asset = plAssetCurator::GetSingleton()->FindSubAsset(sClip); asset.isValid())
      sName = asset->GetName();

    sClips.AppendWithSeparator(";", sClip, "|", sName);
  }

  // the test runs on what the preview shows, which is the document as it is now
  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "StressTest";
  msg.m_sPayload = sClips;
  GetDocument()->SendMessageToEngine(&msg);
}

bool plJoltClothAssetDocumentWindow::GetStressTimelinePosition(const plEditorInputEvent& e, float& out_fWhere) const
{
  // the engine process draws the timeline along the top of the view, one percent in from its sides
  const plVec2 vSize = m_pViewWidget->GetViewportSize();
  if (vSize.x < 1.0f || vSize.y < 1.0f)
    return false;

  out_fWhere = plMath::Saturate((e.m_vPosition.x / vSize.x - 0.01f) / 0.98f);
  return e.m_vPosition.y <= vSize.y * 0.09f;
}

void plJoltClothAssetDocumentWindow::ShowStressFrame(float fWhere)
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  // The preview plays the test up to the frame, which takes a moment. While it does, only the newest frame that was
  // asked for is remembered, so that scrubbing does not queue up a test for every mouse move.
  m_fStressFrameWanted = fWhere;

  if (m_bStressFramePending)
    return;

  m_bStressFramePending = true;
  m_fStressFrameAsked = fWhere;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "StressFrame";
  msg.m_PayloadValue = fWhere;
  GetDocument()->SendMessageToEngine(&msg);
}

plString plJoltClothAssetDocumentWindow::GetStressBaselineFile() const
{
  plStringBuilder sFile = GetDocument()->GetDocumentPath();
  sFile.ChangeFileExtension("plClothBaseline");
  return sFile;
}

void plJoltClothAssetDocumentWindow::ShowStressResult(plStringView sText, const plVariant& worst)
{
  plStringBuilder sReport = sText;

  m_LastStressResult.Clear();
  if (worst.IsA<plDataBuffer>())
    m_LastStressResult = worst.Get<plDataBuffer>();

  // a test that ran left its timeline in the view: the mouse can reach it now
  m_bStressShown = !m_LastStressResult.IsEmpty();
  m_fStressScrubbedTo = -1.0f;
  m_bStressFramePending = false;

  if (m_bStressShown)
  {
    m_PaintTool = plJoltClothPaintTool::Off;
    m_bDragTest = false;
    m_vDragOffset.SetZero();
  }

  UpdateInputContexts();

  // how the run compares with the baseline that is kept next to the asset
  plJoltClothStressResult result;
  plRawMemoryStreamReader resultReader(m_LastStressResult.GetData(), m_LastStressResult.GetCount());

  if (!m_LastStressResult.IsEmpty() && plJoltClothStressResult::DeserializeBaseline(resultReader, result.m_Worst).Succeeded())
  {
    plOSFile file;
    plDynamicArray<plUInt8> content;
    plJoltClothStressFrame baseline;

    if (file.Open(GetStressBaselineFile(), plFileOpenMode::Read).Succeeded())
    {
      content.SetCountUninitialized(static_cast<plUInt32>(file.GetFileSize()));
      file.Read(content.GetData(), content.GetCount());

      plRawMemoryStreamReader baselineReader(content.GetData(), content.GetCount());
      plStringBuilder sWhy;

      if (plJoltClothStressResult::DeserializeBaseline(baselineReader, baseline).Failed())
        sReport.Append("\nThe baseline next to the asset cannot be read. Set Stress Baseline writes a new one.\n");
      else if (result.IsWorseThan(baseline, 0.2f, sWhy))
        sReport.Append("\nWorse than the baseline: ", sWhy, "\n");
      else
        sReport.Append("\nNo worse than the baseline.\n");
    }
    else
    {
      sReport.Append("\nThere is no baseline yet. Set Stress Baseline keeps this run as the one later runs are compared with.\n");
    }
  }

  plLog::Info("Cloth stress test:\n{}", sReport);

  // a dialog must not open from inside the message that asked for it
  plEditorUiRuntime::RunAfterFrame([sShown = plString(sReport)]()
    { plEditorUiServices::MessageBoxInformation(plFmt("{}", sShown)); });
}

void plJoltClothAssetDocumentWindow::SetStressBaseline()
{
  if (m_LastStressResult.IsEmpty())
  {
    plEditorUiServices::MessageBoxInformation("Run the stress test first. Its result becomes the baseline.");
    return;
  }

  const plString sFile = GetStressBaselineFile();

  plOSFile file;
  if (file.Open(sFile, plFileOpenMode::Write).Failed() || file.Write(m_LastStressResult.GetData(), m_LastStressResult.GetCount()).Failed())
  {
    plLog::Error("The stress baseline '{}' could not be written.", sFile);
    return;
  }

  plLog::Success("Stress baseline written to '{}'.", sFile);
  plEditorUiServices::MessageBoxInformation(plFmt("The last stress test is the baseline now. It is kept next to the asset:\n{}", plPathUtils::GetFileNameAndExtension(sFile)));
}

void plJoltClothAssetDocumentWindow::DocumentSavedHandler(const plDocumentEvent& e)
{
  if (e.m_Type != plDocumentEvent::Type::DocumentSaved || !m_bCookValid)
    return;

  // A cloth that cooks is ready to be worn. The question opens once the save is through.
  const plUuid clothGuid = GetDocument()->GetGuid();
  const plString sMesh = GetClothDocument()->GetProperties()->m_sMesh;

  plEditorUiRuntime::RunAfterFrame([clothGuid, sMesh]()
    { plJoltClothAssetDocumentManager::OfferClothComponents(clothGuid, sMesh); });
}

void plJoltClothAssetDocumentWindow::CommandEventHandler(const plCommandHistoryEvent& e)
{
  // every change to the asset shows in the preview at once; the file on disk only changes with a transform
  if (e.m_Type == plCommandHistoryEvent::Type::TransactionEnded || e.m_Type == plCommandHistoryEvent::Type::UndoEnded || e.m_Type == plCommandHistoryEvent::Type::RedoEnded)
  {
    SendLiveResourcePreview();
  }
}

void plJoltClothAssetDocumentWindow::SendLiveResourcePreview()
{
  plJoltClothAssetDocument* pDoc = GetClothDocument();

  plJoltClothDescriptor desc;
  const plStatus status = pDoc->BuildCloth(nullptr, desc);

  if (status.Failed())
  {
    m_bCookValid = false;
    plLog::Info("Cloth preview: {}", status.m_sMessage);
    return;
  }

  m_Cooked = std::move(desc);
  m_bCookValid = true;

  UpdatePaintData();
  SendCookedResource();
}

void plJoltClothAssetDocumentWindow::SendCookedResource()
{
  if (!m_bCookValid || plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  plJoltClothAssetDocument* pDoc = GetClothDocument();

  plResourceUpdateMsgToEngine msg;
  msg.m_sResourceType = "Jolt_Cloth";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(pDoc->GetGuid(), tmp);

  plContiguousMemoryStreamStorage streamStorage;
  plMemoryStreamWriter memoryWriter(&streamStorage);

  // what the file reader puts in front of a resource: its path, then the asset header
  plStringBuilder sAbsFilePath = pDoc->GetDocumentPath();
  sAbsFilePath.ChangeFileExtension("plBinJoltCloth");
  memoryWriter << sAbsFilePath;

  const plUInt64 uiHash = plAssetCurator::GetSingleton()->GetAssetTransformHash(pDoc->GetGuid());
  plAssetFileHeader assetHeader;
  assetHeader.SetFileHashAndVersion(uiHash, pDoc->GetAssetTypeVersion());
  assetHeader.Write(memoryWriter).IgnoreResult();

  if (plJoltClothAssetDocument::WriteCloth(memoryWriter, m_Cooked).Failed())
    return;

  msg.m_Data = plArrayPtr<const plUInt8>(streamStorage.GetData(), streamStorage.GetStorageSize32());

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}

void plJoltClothAssetDocumentWindow::RestoreResource()
{
  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = "Jolt_Cloth";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}

//////////////////////////////////////////////////////////////////////////
// Painting

void plJoltClothAssetDocumentWindow::UpdatePaintData()
{
  m_Topology.Build(m_Cooked.m_Positions, m_Cooked.m_Indices);

  // the cook has applied what was painted so far, so its maps are the ones to go on painting
  plJoltClothBuilder::ExtractPaintedMaps(m_Cooked, m_Working);

  // the preview's character stands at the origin: its bind space only differs by the skeleton's root transform
  const plSkeletonResourceDescriptor* pSkeleton = GetClothDocument()->GetCookedSkeleton();
  m_mBindToWorld = pSkeleton != nullptr ? pSkeleton->m_RootTransform.GetAsMat4() : plMat4::MakeIdentity();

  m_bPaintValuesDirty = true;
}

plDynamicArray<float>& plJoltClothAssetDocumentWindow::GetWorkingMap()
{
  switch (m_PaintMap)
  {
    case plJoltClothPaintMap::BackstopDistance:
      return m_Working.m_BackstopDistance;
    case plJoltClothPaintMap::BackstopRadius:
      return m_Working.m_BackstopRadius;
    case plJoltClothPaintMap::Stiffness:
      return m_Working.m_StiffnessScale;
    default:
      return m_Working.m_MaxDistance;
  }
}

float plJoltClothAssetDocumentWindow::GetMapRange() const
{
  switch (m_PaintMap)
  {
    case plJoltClothPaintMap::BackstopDistance:
      return s_fBackstopDistanceRange;
    case plJoltClothPaintMap::BackstopRadius:
      return s_fBackstopRadiusRange;
    case plJoltClothPaintMap::Stiffness:
      return 1.0f;
    default:
      return plMath::Max(0.01f, GetClothDocument()->GetProperties()->m_fMaxDistance);
  }
}

bool plJoltClothAssetDocumentWindow::PickSurface(const plEditorInputEvent& e, plJoltClothMapTools::RayHit& out_hit) const
{
  plVec3 vStart, vDirection;
  if (m_pViewWidget->GetPickingRay(static_cast<plUInt16>(plMath::Max(0.0f, e.m_vPosition.x)), static_cast<plUInt16>(plMath::Max(0.0f, e.m_vPosition.y)), vStart, vDirection).Failed())
    return false;

  // the sim mesh is in bind space
  const plMat4 mWorldToBind = m_mBindToWorld.GetInverse();

  plVec3 vBindDirection = mWorldToBind.TransformDirection(vDirection);
  if (vBindDirection.NormalizeIfNotZero(plVec3::MakeZero()).Failed())
    return false;

  return plJoltClothMapTools::RayCast(m_Topology, mWorldToBind.TransformPosition(vStart), vBindDirection, out_hit);
}

plVec3 plJoltClothAssetDocumentWindow::MirrorPosition(const plVec3& vBindPosition) const
{
  // a character's left and right are the two sides of the plane Y = 0
  plVec3 vWorld = m_mBindToWorld.TransformPosition(vBindPosition);
  vWorld.y = -vWorld.y;

  return m_mBindToWorld.GetInverse().TransformPosition(vWorld);
}

void plJoltClothAssetDocumentWindow::ApplyBrushAt(const plVec3& vBindPosition)
{
  plDynamicArray<float>& map = GetWorkingMap();

  // the brush is sized in metres, the sim mesh may be in other units
  const float fBindScale = plMath::Max(1.0e-6f, m_mBindToWorld.TransformDirection(plVec3(1, 0, 0)).GetLength());

  plJoltClothBrush brush;
  brush.m_vCenter = vBindPosition;
  brush.m_fRadius = m_fBrushRadius / fBindScale;
  brush.m_fStrength = m_fBrushStrength;

  plJoltClothBrush mirrored = brush;
  mirrored.m_vCenter = MirrorPosition(vBindPosition);

  if (m_PaintTool == plJoltClothPaintTool::Smooth)
  {
    // smoothing adds up while the mouse moves, so it is kept gentle
    brush.m_fStrength *= 0.5f;
    mirrored.m_fStrength *= 0.5f;

    plJoltClothMapTools::Smooth(m_Topology, brush, map);

    if (m_bMirror)
      plJoltClothMapTools::Smooth(m_Topology, mirrored, map);
  }
  else
  {
    plJoltClothMapTools::AccumulateStroke(m_Topology, brush, m_StrokeWeights);

    if (m_bMirror)
      plJoltClothMapTools::AccumulateStroke(m_Topology, mirrored, m_StrokeWeights);

    plJoltClothMapTools::ApplyPaint(m_StrokeStart, m_StrokeWeights, m_fBrushValue * GetMapRange(), map);
  }

  m_bPaintValuesDirty = true;
}

bool plJoltClothAssetDocumentWindow::BeginStroke(const plEditorInputEvent& e)
{
  if (m_bStressShown)
  {
    // a press on the timeline shows that frame, and dragging scrubs through the test
    float fWhere = 0.0f;
    if (!GetStressTimelinePosition(e, fWhere))
      return false;

    m_bStressScrub = true;
    ShowStressFrame(fWhere);
    return true;
  }

  if (m_bDragTest)
  {
    // the drag goes on from wherever the character is, also while it still swings back
    plVec3 vGround;
    if (!PickGround(e, vGround))
      return false;

    m_bDragging = true;
    m_vDragStart = vGround - m_vDragOffset;
    return true;
  }

  if (m_PaintTool == plJoltClothPaintTool::Off || !m_bCookValid || m_Topology.m_Positions.IsEmpty())
    return false;

  plDynamicArray<float>& map = GetWorkingMap();
  if (map.GetCount() != m_Topology.m_Positions.GetCount())
    return false;

  plJoltClothMapTools::RayHit hit;
  const bool bHit = PickSurface(e, hit);

  // fill and gradient need a piece of cloth to start on
  if (!bHit && (m_PaintTool == plJoltClothPaintTool::Fill || m_PaintTool == plJoltClothPaintTool::Gradient))
    return false;

  m_bStroke = true;
  m_StrokeStart = map;
  m_StrokeWeights.Clear();
  m_StrokeWeights.SetCount(map.GetCount(), 0.0f);

  if (bHit)
  {
    m_vStrokeFrom = hit.m_vPosition;
    m_uiStrokeIsland = m_Topology.m_Island[hit.m_uiClosestParticle];
  }

  if (m_PaintTool == plJoltClothPaintTool::Fill)
  {
    // Shift fills every piece
    plJoltClothMapTools::Fill(m_Topology, e.HasModifier(plEditorInputEvent::Shift) ? plInvalidIndex : m_uiStrokeIsland, m_fBrushValue * GetMapRange(), map);
    m_bPaintValuesDirty = true;
  }
  else if (bHit && m_PaintTool != plJoltClothPaintTool::Gradient)
  {
    ApplyBrushAt(hit.m_vPosition);
  }

  return true;
}

void plJoltClothAssetDocumentWindow::ContinueStroke(const plEditorInputEvent& e)
{
  if (m_bStressScrub)
  {
    float fWhere = 0.0f;
    GetStressTimelinePosition(e, fWhere);

    // every frame is simulated from the start of the test: only when the mouse reached another one
    if (plMath::Abs(fWhere - m_fStressScrubbedTo) > 0.002f)
    {
      m_fStressScrubbedTo = fWhere;
      ShowStressFrame(fWhere);
    }

    return;
  }

  if (m_bDragging)
  {
    plVec3 vGround;
    if (PickGround(e, vGround))
    {
      // not further than the view shows, and less than the distance the cloth takes for a teleport
      m_vDragOffset = vGround - m_vDragStart;
      m_vDragOffset.z = 0.0f;

      const float fLength = m_vDragOffset.GetLength();
      if (fLength > 1.5f)
        m_vDragOffset *= 1.5f / fLength;
    }

    return;
  }

  if (!m_bStroke)
    return;

  plJoltClothMapTools::RayHit hit;
  if (!PickSurface(e, hit))
  {
    m_bBrushVisible = false;
    return;
  }

  m_vBrushWorldPosition = m_mBindToWorld.TransformPosition(hit.m_vPosition);
  m_bBrushVisible = true;

  if (m_PaintTool == plJoltClothPaintTool::Gradient)
  {
    // the ramp is always drawn from the values the stroke started with
    plDynamicArray<float>& map = GetWorkingMap();
    map = m_StrokeStart;

    plJoltClothMapTools::Gradient(m_Topology, m_uiStrokeIsland, m_vStrokeFrom, hit.m_vPosition, 0.0f, m_fBrushValue * GetMapRange(), map);
    m_bPaintValuesDirty = true;
  }
  else if (m_PaintTool == plJoltClothPaintTool::Brush || m_PaintTool == plJoltClothPaintTool::Smooth)
  {
    ApplyBrushAt(hit.m_vPosition);
  }
}

void plJoltClothAssetDocumentWindow::EndStroke(bool bCancel)
{
  if (m_bStressScrub)
  {
    m_bStressScrub = false;
    return;
  }

  if (m_bDragging)
  {
    // let go: the preview brings the character back to its place
    m_bDragging = false;
    m_vDragOffset.SetZero();
    return;
  }

  if (!m_bStroke)
    return;

  m_bStroke = false;

  plDynamicArray<float>& map = GetWorkingMap();

  if (bCancel)
  {
    map = m_StrokeStart;
    m_bPaintValuesDirty = true;
    return;
  }

  plJoltClothPaintedMaps maps;
  GetMapsToStore(m_PaintMap, maps);

  // one undo step per stroke; the preview follows through the command history
  GetClothDocument()->SetPaintedMaps(maps, "Paint Cloth Map");
}

void plJoltClothAssetDocumentWindow::GetMapsToStore(plJoltClothPaintMap::Enum changedMap, plJoltClothPaintedMaps& out_maps) const
{
  // Only maps that were painted are stored. The others keep following the imported masks and the asset's defaults.
  plJoltClothPaintedMaps stored;
  GetClothDocument()->GetPaintedMaps(stored).IgnoreResult();

  out_maps.Clear();
  out_maps.m_Positions = m_Working.m_Positions;

  if (!stored.m_MaxDistance.IsEmpty() || changedMap == plJoltClothPaintMap::MaxDistance)
    out_maps.m_MaxDistance = m_Working.m_MaxDistance;
  if (!stored.m_BackstopDistance.IsEmpty() || changedMap == plJoltClothPaintMap::BackstopDistance)
    out_maps.m_BackstopDistance = m_Working.m_BackstopDistance;
  if (!stored.m_BackstopRadius.IsEmpty() || changedMap == plJoltClothPaintMap::BackstopRadius)
    out_maps.m_BackstopRadius = m_Working.m_BackstopRadius;
  if (!stored.m_StiffnessScale.IsEmpty() || changedMap == plJoltClothPaintMap::Stiffness)
    out_maps.m_StiffnessScale = m_Working.m_StiffnessScale;
}

void plJoltClothAssetDocumentWindow::HoverBrush(const plEditorInputEvent& e)
{
  if (m_PaintTool == plJoltClothPaintTool::Off)
  {
    m_bBrushVisible = false;
    return;
  }

  plJoltClothMapTools::RayHit hit;

  m_bBrushVisible = m_bCookValid && PickSurface(e, hit);

  if (m_bBrushVisible)
    m_vBrushWorldPosition = m_mBindToWorld.TransformPosition(hit.m_vPosition);
}

void plJoltClothAssetDocumentWindow::SendPaintValues()
{
  m_bPaintValuesDirty = false;

  const plDynamicArray<float>& map = GetWorkingMap();
  const float fRange = GetMapRange();

  // one byte per particle: 0 is 0, 255 the top of the map's range
  plDataBuffer values;
  values.SetCountUninitialized(map.GetCount());

  for (plUInt32 p = 0; p < map.GetCount(); ++p)
  {
    values[p] = static_cast<plUInt8>(plMath::Saturate(map[p] / fRange) * 255.0f + 0.5f);
  }

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "PaintValues";
  msg.m_PayloadValue = values;
  GetDocument()->SendMessageToEngine(&msg);
}

void plJoltClothAssetDocumentWindow::AutoMask()
{
  if (!m_bCookValid || m_Topology.m_Positions.IsEmpty())
    return;

  if (m_bStroke)
    EndStroke(true);

  plJoltClothAssetDocument* pDoc = GetClothDocument();

  plVec3 vUp = m_mBindToWorld.GetInverse().TransformDirection(plVec3(0, 0, 1));
  vUp.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

  // the other painted maps stay as they are
  plJoltClothPaintedMaps maps;
  GetMapsToStore(plJoltClothPaintMap::MaxDistance, maps);

  plJoltClothMapTools::AutoMask(m_Topology, vUp, 0.1f, pDoc->GetProperties()->m_fMaxDistance, maps.m_MaxDistance);

  pDoc->SetPaintedMaps(maps, "Auto Mask Cloth");
}

void plJoltClothAssetDocumentWindow::RevertMaps()
{
  if (m_bStroke)
    EndStroke(true);

  if (GetClothDocument()->GetProperties()->m_PaintedMaps.IsEmpty())
    return;

  GetClothDocument()->SetPaintedMaps(plJoltClothPaintedMaps(), "Revert Cloth Maps");
}

void plJoltClothAssetDocumentWindow::AutoSelectColliders()
{
  plJoltClothAssetDocument* pDoc = GetClothDocument();
  const plSkeletonResourceDescriptor* pSkeleton = pDoc->GetCookedSkeleton();

  if (!m_bCookValid || pSkeleton == nullptr)
  {
    plLog::Warning("Auto Select needs the cloth's mesh and its skeleton to be transformed.");
    return;
  }

  // the shapes in reach of the cloth as it is now
  plJoltClothDescriptor inReach = m_Cooked;
  inReach.m_Colliders.Clear();

  plJoltClothSkeletonColliderOptions options;
  options.m_fInflate = pDoc->GetProperties()->m_fColliderInflate;
  plJoltClothBuilder::AddSkeletonColliders(*pSkeleton, options, inReach);

  plHybridArray<plString, 16> joints;
  for (const plJoltClothCollider& collider : inReach.m_Colliders)
  {
    const plString sJoint = collider.m_sJoint.GetString();

    if (!joints.Contains(sJoint))
      joints.PushBack(sJoint);
  }

  const plDocumentObject* pObject = pDoc->GetPropertyObject();

  plObjectCommandAccessor accessor(pDoc->GetCommandHistory());
  accessor.StartTransaction("Auto Select Cloth Colliders");

  accessor.SetValueByName(pObject, "ColliderSource", static_cast<plInt64>(plJoltClothColliderSource::SkeletonShapes)).AssertSuccess();
  accessor.ClearByName(pObject, "SkeletonShapeJoints").AssertSuccess();

  for (plUInt32 j = 0; j < joints.GetCount(); ++j)
  {
    accessor.InsertValueByName(pObject, "SkeletonShapeJoints", joints[j], j).AssertSuccess();
  }

  accessor.FinishTransaction();
}

void plJoltClothAssetDocumentWindow::FitCollidersFromSkin()
{
  plJoltClothAssetDocument* pDoc = GetClothDocument();
  const plMeshResourceDescriptor* pMesh = pDoc->GetCookedMesh();

  if (!m_bCookValid || pMesh == nullptr)
  {
    plLog::Warning("Fit From Skin needs the cloth's mesh to be transformed.");
    return;
  }

  // the cloth as it is, without colliders: every fitted shape it can reach is kept
  plJoltClothDescriptor reach = m_Cooked;
  reach.m_Colliders.Clear();

  plDynamicArray<plJoltClothCollider> fitted;
  plJoltClothBuilder::FitCollidersFromSkin(*pMesh, plJoltClothSkinFitOptions(), &reach, fitted);

  if (fitted.IsEmpty())
  {
    plEditorUiServices::MessageBoxInformation("No shape could be fitted. The body needs vertices that are skinned to the bones near the garment.");
    return;
  }

  const plDocumentObject* pObject = pDoc->GetPropertyObject();

  plObjectCommandAccessor accessor(pDoc->GetCommandHistory());
  accessor.StartTransaction("Fit Cloth Colliders From Skin");

  accessor.ClearByName(pObject, "Colliders").AssertSuccess();

  for (plUInt32 c = 0; c < fitted.GetCount(); ++c)
  {
    const plJoltClothCollider& collider = fitted[c];

    plUuid guid = plUuid::MakeUuid();
    accessor.AddObjectByName(pObject, "Colliders", c, plGetStaticRTTI<plJoltClothAssetCollider>(), guid).AssertSuccess();

    const plDocumentObject* pCollider = accessor.GetObject(guid);
    const plInt64 iShape = collider.m_Type == plJoltClothCollider::Type::Sphere ? plJoltClothColliderShape::Sphere : plJoltClothColliderShape::Capsule;

    accessor.SetValueByName(pCollider, "Joint", collider.m_sJoint.GetString()).AssertSuccess();
    accessor.SetValueByName(pCollider, "Shape", iShape).AssertSuccess();
    accessor.SetValueByName(pCollider, "Offset", collider.m_Transform.m_vPosition).AssertSuccess();
    accessor.SetValueByName(pCollider, "Rotation", collider.m_Transform.m_qRotation).AssertSuccess();
    accessor.SetValueByName(pCollider, "Radius", collider.m_fRadius).AssertSuccess();
    accessor.SetValueByName(pCollider, "Length", collider.m_fLength).AssertSuccess();
  }

  accessor.FinishTransaction();

  plLog::Success("Fit From Skin: {} cloth collider(s) fitted to the body.", fitted.GetCount());
}
