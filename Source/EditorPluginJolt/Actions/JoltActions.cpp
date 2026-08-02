#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/Actions/JoltActions.h>
#include <EditorPluginJolt/Dialogs/JoltProjectSettingsDlg.moc.h>
#include <GuiFoundation/Action/ActionMapManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltAction, 0, plRTTINoAllocator)
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
    plQtJoltProjectSettingsDlg dlg(value);
    if (dlg.exec() == QDialog::Accepted)
    {
      plToolsProject::BroadcastConfigChanged();
    }
  }
}
