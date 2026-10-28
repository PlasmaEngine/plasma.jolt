#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/OrbitCameraContext.h>
#include <EditorPluginJolt/Actions/JoltActions.h>
#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetWindow.h>
#include <EditorPluginJolt/Dialogs/JoltProjectSettingsEngineDlg.h>
#include <GuiFoundation/Action/ActionMapManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltFrameAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltExplodedViewAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plActionDescriptorHandle plJoltActions::s_hCategoryJolt;
plActionDescriptorHandle plJoltActions::s_hProjectSettings;

void plJoltActions::RegisterActions()
{
  s_hCategoryJolt = PL_REGISTER_CATEGORY("Jolt");
  s_hProjectSettings = PL_REGISTER_ACTION_1("Jolt.Settings.Project", plActionScope::Document, "Jolt", "", plJoltAction, plJoltAction::ActionType::ProjectSettings);
}

void plJoltActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategoryJolt);
  plActionManager::UnregisterAction(s_hProjectSettings);
}

void plJoltActions::MapMenuActions()
{
  plActionMap* pMap = plActionMapManager::GetActionMap("EditorPluginScene_Scene2MenuBar");
  if (pMap != nullptr)
  {
    pMap->MapAction(s_hCategoryJolt, "G.Plugins.Settings", 10.0f);
    pMap->MapAction(s_hProjectSettings, "G.Plugins.Settings", "Jolt", 1.0f);
  }
}

plJoltAction::plJoltAction(const plActionContext& context, const char* szName, ActionType type)
  : plButtonAction(context, szName, false, "")
{
  m_Type = type;

  switch (m_Type)
  {
    case ActionType::ProjectSettings:
      SetIconPath(":/JoltPlugin/JoltPlugin.svg");
      break;
  }
}

plJoltAction::~plJoltAction() = default;

void plJoltAction::Execute(const plVariant& value)
{
  if (m_Type == ActionType::ProjectSettings)
  {
    if (plJoltProjectSettingsEngineDlg::Run(value))
    {
      plToolsProject::BroadcastConfigChanged();
    }
  }
}

//////////////////////////////////////////////////////////////////////////

plActionDescriptorHandle plJoltAssetViewActions::s_hFrame;
plActionDescriptorHandle plJoltAssetViewActions::s_hExplodedView;

void plJoltAssetViewActions::RegisterActions()
{
  s_hFrame = PL_REGISTER_ACTION_0("JoltAsset.Frame", plActionScope::Window, "Jolt", "", plJoltFrameAction);
  s_hExplodedView = PL_REGISTER_ACTION_0("DestructibleMesh.ExplodedView", plActionScope::Window, "Jolt", "", plJoltExplodedViewAction);
}

void plJoltAssetViewActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hFrame);
  plActionManager::UnregisterAction(s_hExplodedView);
}

void plJoltAssetViewActions::MapViewToolBarActions(plStringView sMapping, bool bExplodedView)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  if (bExplodedView)
    pMap->MapAction(s_hExplodedView, "", 10.0f);

  pMap->MapAction(s_hFrame, "", 20.0f);
}

plJoltFrameAction::plJoltFrameAction(const plActionContext& context, const char* szName)
  : plButtonAction(context, szName, false, ":/EditorFramework/Icons/FocusOnSelection.svg")
{
}

void plJoltFrameAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);

  // view strips get the view widget as their window
  if (auto* pView = dynamic_cast<plOrbitCamViewWidget*>(m_Context.m_pWindow))
    pView->GetOrbitCamera()->MoveCameraToDefaultPosition();
}

plJoltExplodedViewAction::plJoltExplodedViewAction(const plActionContext& context, const char* szName)
  : plSliderAction(context, szName)
{
  SetRange(0, 200);

  if (auto* pWindow = dynamic_cast<plDestructibleMeshAssetDocumentWindow*>(plDocumentWindow::FindWindowByDocument(context.m_pDocument)))
    SetValue(static_cast<plInt32>(pWindow->GetExplodeFactor() * 100.0f + 0.5f));
  else
    SetValue(0);
}

void plJoltExplodedViewAction::Execute(const plVariant& value)
{
  // the view strip reads the value back from the action
  SetValue(value.ConvertTo<plInt32>(), false);

  if (auto* pWindow = dynamic_cast<plDestructibleMeshAssetDocumentWindow*>(plDocumentWindow::FindWindowByDocument(m_Context.m_pDocument)))
    pWindow->SetExplodeFactor(value.ConvertTo<plInt32>() / 100.0f);
}
