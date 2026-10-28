#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/CameraModeSwitchActions.h>
#include <EditorFramework/Actions/CommonAssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Actions/ViewActions.h>
#include <EditorPluginJolt/Actions/JoltActions.h>
#include <EditorPluginJolt/CollisionMeshAsset/JoltCollisionMeshAssetObjects.h>
#include <EditorPluginJolt/Dialogs/JoltProjectSettingsEngineDlg.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/EditActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/UIServices/DynamicEnums.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>

void UpdateCollisionLayerDynamicEnumValues();
void UpdateWeightCategoryDynamicEnumValues();
void UpdateImpulseTypeDynamicEnumValues();

static void ToolsProjectEventHandler(const plToolsProjectEvent& e);

void plDynamicActorComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);
void plRagdollComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);
void plCharacterControllerComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);
void plRopeComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);
void plClothSheetComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

/// The package's icon folder: the icons under their ids (":/TypeIcons/plJoltRopeComponent.svg" is
/// "EditorIcons/TypeIcons/plJoltRopeComponent.svg"). The plugin binary lives in <package>/Bin/<configuration>.
static plString s_sIconFolder;

static void RegisterIconFolder()
{
  plStringBuilder sPlugin, sCopy;
  plPlugin::GetPluginPaths("plEditorPluginJolt", sPlugin, sCopy, 0);

  plStringBuilder sFolder = sPlugin.GetFileDirectory();
  sFolder.AppendPath("..", "..", "EditorIcons");
  sFolder.MakeCleanPath();

  s_sIconFolder = sFolder;
  plEditorUi::AddIconFolder(s_sIconFolder);
}

static void UnregisterIconFolder()
{
  plEditorUi::RemoveIconFolder(s_sIconFolder);
  s_sIconFolder.Clear();
}

void OnLoadPlugin()
{
  RegisterIconFolder();

  plToolsProject::GetSingleton()->s_Events.AddEventHandler(ToolsProjectEventHandler);

  plJoltAssetViewActions::RegisterActions();

  // Collision Mesh
  {
    plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plJoltCollisionMeshAssetProperties::PropertyMetaStateEventHandler);

    // Menu Bar: the same menus as every other asset window
    {
      plActionMapManager::RegisterActionMap("JoltCollisionMeshAssetMenuBar").IgnoreResult();

      plStandardMenus::MapActions("JoltCollisionMeshAssetMenuBar", plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
      plProjectActions::MapActions("JoltCollisionMeshAssetMenuBar");
      plDocumentActions::MapMenuActions("JoltCollisionMeshAssetMenuBar");
      plAssetActions::MapMenuActions("JoltCollisionMeshAssetMenuBar");
      plCommandHistoryActions::MapActions("JoltCollisionMeshAssetMenuBar");
      plEditActions::MapActions("JoltCollisionMeshAssetMenuBar", false, false);
    }

    // Tool Bar: save, undo, redo and transform like every other asset window, then grid and camera mode
    {
      plActionMapManager::RegisterActionMap("JoltCollisionMeshAssetToolBar").IgnoreResult();
      plDocumentActions::MapToolbarActions("JoltCollisionMeshAssetToolBar");
      plCommandHistoryActions::MapActions("JoltCollisionMeshAssetToolBar", "");
      plAssetActions::MapToolBarActions("JoltCollisionMeshAssetToolBar", true);
      plCommonAssetActions::MapToolbarActions("JoltCollisionMeshAssetToolBar", plCommonAssetUiState::Grid);
      plCameraModeSwitchActions::MapToolbarActions("JoltCollisionMeshAssetToolBar");
    }

    // View Tool Bar
    {
      plActionMapManager::RegisterActionMap("JoltCollisionMeshAssetViewToolBar").IgnoreResult();
      plViewActions::MapToolbarActions("JoltCollisionMeshAssetViewToolBar", plViewActions::RenderMode);
      plJoltAssetViewActions::MapViewToolBarActions("JoltCollisionMeshAssetViewToolBar", false);
    }
  }

  // Destructible Mesh
  {
    // Menu Bar
    {
      plActionMapManager::RegisterActionMap("DestructibleMeshAssetMenuBar").IgnoreResult();

      plStandardMenus::MapActions("DestructibleMeshAssetMenuBar", plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
      plProjectActions::MapActions("DestructibleMeshAssetMenuBar");
      plDocumentActions::MapMenuActions("DestructibleMeshAssetMenuBar");
      plAssetActions::MapMenuActions("DestructibleMeshAssetMenuBar");
      plCommandHistoryActions::MapActions("DestructibleMeshAssetMenuBar");
      plEditActions::MapActions("DestructibleMeshAssetMenuBar", false, false);
    }

    // Tool Bar
    {
      plActionMapManager::RegisterActionMap("DestructibleMeshAssetToolBar").IgnoreResult();

      plDocumentActions::MapToolbarActions("DestructibleMeshAssetToolBar");
      plCommandHistoryActions::MapActions("DestructibleMeshAssetToolBar", "");
      plAssetActions::MapToolBarActions("DestructibleMeshAssetToolBar", true);
      plCommonAssetActions::MapToolbarActions("DestructibleMeshAssetToolBar", plCommonAssetUiState::Grid);
      plCameraModeSwitchActions::MapToolbarActions("DestructibleMeshAssetToolBar");
    }

    // View Tool Bar
    {
      plActionMapManager::RegisterActionMap("DestructibleMeshAssetViewToolBar").IgnoreResult();
      plViewActions::MapToolbarActions("DestructibleMeshAssetViewToolBar", plViewActions::RenderMode);
      plJoltAssetViewActions::MapViewToolBarActions("DestructibleMeshAssetViewToolBar", true);
    }
  }

  // Scene
  {
    // Menu Bar
    {
      plJoltActions::RegisterActions();
      plJoltActions::MapMenuActions();
    }

    // Tool Bar
    {
    }
  }

  // component property meta states
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plDynamicActorComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plRagdollComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plCharacterControllerComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plRopeComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plClothSheetComponent_PropertyMetaStateEventHandler);
}

void OnUnloadPlugin()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plDynamicActorComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plRagdollComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plCharacterControllerComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plRopeComponent_PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plClothSheetComponent_PropertyMetaStateEventHandler);

  plJoltActions::UnregisterActions();
  plJoltAssetViewActions::UnregisterActions();
  plToolsProject::GetSingleton()->s_Events.RemoveEventHandler(ToolsProjectEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plJoltCollisionMeshAssetProperties::PropertyMetaStateEventHandler);

  UnregisterIconFolder();
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}

void UpdateCollisionLayerDynamicEnumValues()
{
  auto& cfe = plDynamicEnum::GetDynamicEnum("PhysicsCollisionLayer");
  cfe.Clear();

  plCollisionFilterConfig cfg;
  if (cfg.Load().Failed())
  {
    return;
  }

  // add all names and values that are valid (non-empty)
  for (plInt32 i = 0; i < 32; ++i)
  {
    if (!cfg.GetGroupName(i).IsEmpty())
    {
      cfe.SetValueAndName(i, cfg.GetGroupName(i));
    }
  }
}

void UpdateWeightCategoryDynamicEnumValues()
{
  auto& cfe = plDynamicEnum::GetDynamicEnum("PhysicsWeightCategoryWithDensity");
  auto& cfeNC = plDynamicEnum::GetDynamicEnum("PhysicsWeightCategory");

  cfe.Clear();
  cfe.SetEditCommand("Jolt.Settings.Project", "WeightCategories");

  cfeNC.Clear();
  cfeNC.SetEditCommand("Jolt.Settings.Project", "WeightCategories");

  plWeightCategoryConfig cfg;
  if (cfg.Load().Succeeded())
  {
    for (const auto it : cfg.m_Categories)
    {
      cfe.SetValueAndName(it.key, it.value.m_sName.GetView());
      cfeNC.SetValueAndName(it.key, it.value.m_sName.GetView());
    }
  }

  cfeNC.SetValueAndName(plWeightCategoryConfig::DefaultValueKey, "<Default>");
  cfeNC.SetValueAndName(plWeightCategoryConfig::CustomMassKey, "<Custom Mass>");

  cfe.SetValueAndName(plWeightCategoryConfig::DefaultValueKey, "<Default>");
  cfe.SetValueAndName(plWeightCategoryConfig::CustomMassKey, "<Custom Mass>");
  cfe.SetValueAndName(plWeightCategoryConfig::CustomDensityKey, "<Custom Density>");
}

void UpdateImpulseTypeDynamicEnumValues()
{
  auto& cfe = plDynamicEnum::GetDynamicEnum("PhysicsImpulseType");

  cfe.Clear();
  cfe.SetEditCommand("Jolt.Settings.Project", "ImpulseTypes");

  plImpulseTypeConfig cfg;
  if (cfg.Load().Succeeded())
  {
    for (const auto it : cfg.m_Types)
    {
      cfe.SetValueAndName(it.key, it.value.m_sName.GetView());
    }
  }

  cfe.SetValueAndName(plImpulseTypeConfig::CustomValueKey, "<Custom Value>");
  cfe.SetValueAndName(plImpulseTypeConfig::NoValueKey, "<None>");
}

static void ToolsProjectEventHandler(const plToolsProjectEvent& e)
{
  if (e.m_Type == plToolsProjectEvent::Type::ProjectSaveState)
  {
    plJoltProjectSettingsEngineDlg::EnsureConfigFileExists();
  }

  if (e.m_Type == plToolsProjectEvent::Type::ProjectOpened)
  {
    plJoltProjectSettingsEngineDlg::EnsureConfigFileExists();
    UpdateCollisionLayerDynamicEnumValues();
    UpdateWeightCategoryDynamicEnumValues();
    UpdateImpulseTypeDynamicEnumValues();
  }
}


//////////////////////////////////////////////////////////////////////////

void plJoltWeightComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  auto& props = *e.m_pPropertyStates;

  const plInt32 iCategory = e.m_pObject->GetTypeAccessor().GetValue("WeightCategory").ConvertTo<plInt32>();

  if (iCategory == plWeightCategoryConfig::DefaultValueKey)
  {
    props["WeightScale"].m_Visibility = plPropertyUiState::Invisible;
    props["Mass"].m_Visibility = plPropertyUiState::Invisible;
    props["Density"].m_Visibility = plPropertyUiState::Invisible;
  }
  else if (iCategory == plWeightCategoryConfig::CustomMassKey)
  {
    props["WeightScale"].m_Visibility = plPropertyUiState::Invisible;
    props["Density"].m_Visibility = plPropertyUiState::Invisible;
  }
  else if (iCategory == plWeightCategoryConfig::CustomDensityKey)
  {
    props["WeightScale"].m_Visibility = plPropertyUiState::Invisible;
    props["Mass"].m_Visibility = plPropertyUiState::Invisible;
  }
  else
  {
    props["Density"].m_Visibility = plPropertyUiState::Invisible;
    props["Mass"].m_Visibility = plPropertyUiState::Invisible;
  }
}

void plDynamicActorComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  static const plRTTI* pRtti = plRTTI::FindTypeByName("plJoltDynamicActorComponent");
  PL_ASSERT_DEBUG(pRtti != nullptr, "Did the typename change?");

  if (e.m_pObject->GetTypeAccessor().GetType() != pRtti)
    return;

  plJoltWeightComponent_PropertyMetaStateEventHandler(e);
}

void plRagdollComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  static const plRTTI* pRtti = plRTTI::FindTypeByName("plJoltRagdollComponent");
  PL_ASSERT_DEBUG(pRtti != nullptr, "Did the typename change?");

  if (e.m_pObject->GetTypeAccessor().GetType() != pRtti)
    return;

  plJoltWeightComponent_PropertyMetaStateEventHandler(e);
}

void plCharacterControllerComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  static const plRTTI* pRtti = plRTTI::FindTypeByName("plJoltCharacterControllerComponent");
  PL_ASSERT_DEBUG(pRtti != nullptr, "Did the typename change?");

  if (e.m_pObject->GetTypeAccessor().GetType() != pRtti)
    return;

  plJoltWeightComponent_PropertyMetaStateEventHandler(e);
}

void plRopeComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  static const plRTTI* pRtti = plRTTI::FindTypeByName("plJoltRopeComponent");
  PL_ASSERT_DEBUG(pRtti != nullptr, "Did the typename change?");

  if (e.m_pObject->GetTypeAccessor().GetType() != pRtti)
    return;

  plJoltWeightComponent_PropertyMetaStateEventHandler(e);
}

void plClothSheetComponent_PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  static const plRTTI* pRtti = plRTTI::FindTypeByName("plJoltClothSheetComponent");
  PL_ASSERT_DEBUG(pRtti != nullptr, "Did the typename change?");

  if (e.m_pObject->GetTypeAccessor().GetType() != pRtti)
    return;

  plJoltWeightComponent_PropertyMetaStateEventHandler(e);
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>

class plJoltRopeComponentPatch_1_2 : public plGraphPatch
{
public:
  plJoltRopeComponentPatch_1_2()
    : plGraphPatch("plJoltRopeComponent", 3)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    pNode->RenameProperty("Anchor", "Anchor2");
    pNode->RenameProperty("AttachToOrigin", "AttachToAnchor1");
    pNode->RenameProperty("AttachToAnchor", "AttachToAnchor2");
  }
};

plJoltRopeComponentPatch_1_2 g_plJoltRopeComponentPatch_1_2;

//////////////////////////////////////////////////////////////////////////

class plJoltHitboxComponentPatch_1_2 : public plGraphPatch
{
public:
  plJoltHitboxComponentPatch_1_2()
    : plGraphPatch("plJoltBoneColliderComponent", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    ref_context.RenameClass("plJoltHitboxComponent");
  }
};

plJoltHitboxComponentPatch_1_2 g_plJoltHitboxComponentPatch_1_2;

//////////////////////////////////////////////////////////////////////////

class plJoltDynamicActorComponentPatch_5_6 : public plGraphPatch
{
public:
  plJoltDynamicActorComponentPatch_5_6()
    : plGraphPatch("plJoltDynamicActorComponent", 6)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    auto pPropMass = pNode->FindProperty("Mass");

    float fMass = 0.0f;

    if (pPropMass && pPropMass->m_Value.IsNumber())
    {
      fMass = pPropMass->m_Value.ConvertTo<float>();
    }

    if (fMass != 0.0f)
    {
      pNode->AddProperty("WeightCategory", static_cast<plUInt8>(plWeightCategoryConfig::CustomMassKey));
    }
    else
    {
      pNode->AddProperty("WeightCategory", static_cast<plUInt8>(plWeightCategoryConfig::CustomDensityKey));
    }
  }
};

plJoltDynamicActorComponentPatch_5_6 g_plJoltDynamicActorComponentPatch_5_6;
