#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginJolt/ClothAsset/JoltClothActions.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetManager.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetWindow.h>
#include <GuiFoundation/Action/ActionMapManager.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothViewMode, 1)
  PL_ENUM_CONSTANTS(plJoltClothViewMode::Shaded, plJoltClothViewMode::SimMesh, plJoltClothViewMode::MaxDistance, plJoltClothViewMode::Backstop)
  PL_ENUM_CONSTANTS(plJoltClothViewMode::Colliders, plJoltClothViewMode::Strain)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothPaintMap, 1)
  PL_ENUM_CONSTANTS(plJoltClothPaintMap::MaxDistance, plJoltClothPaintMap::BackstopDistance, plJoltClothPaintMap::BackstopRadius, plJoltClothPaintMap::Stiffness)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothPaintTool, 1)
  PL_ENUM_CONSTANTS(plJoltClothPaintTool::Off, plJoltClothPaintTool::Brush, plJoltClothPaintTool::Smooth, plJoltClothPaintTool::Fill, plJoltClothPaintTool::Gradient)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothSceneView, 1)
  PL_ENUM_CONSTANTS(plJoltClothSceneView::Off, plJoltClothSceneView::SimMesh, plJoltClothSceneView::MaxDistance, plJoltClothSceneView::Backstop)
  PL_ENUM_CONSTANTS(plJoltClothSceneView::Colliders, plJoltClothSceneView::Strain, plJoltClothSceneView::State, plJoltClothSceneView::Cost)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothSceneViewAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothEnumAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothSliderAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothButtonAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plActionDescriptorHandle plJoltClothActions::s_hCategory;
plActionDescriptorHandle plJoltClothActions::s_hView;
plActionDescriptorHandle plJoltClothActions::s_hPaintTool;
plActionDescriptorHandle plJoltClothActions::s_hPaintMap;
plActionDescriptorHandle plJoltClothActions::s_hBrushRadius;
plActionDescriptorHandle plJoltClothActions::s_hBrushValue;
plActionDescriptorHandle plJoltClothActions::s_hBrushStrength;
plActionDescriptorHandle plJoltClothActions::s_hMirror;
plActionDescriptorHandle plJoltClothActions::s_hAutoMask;
plActionDescriptorHandle plJoltClothActions::s_hRevertMaps;
plActionDescriptorHandle plJoltClothActions::s_hAutoSelectColliders;
plActionDescriptorHandle plJoltClothActions::s_hWind;
plActionDescriptorHandle plJoltClothActions::s_hStressTest;
plActionDescriptorHandle plJoltClothActions::s_hStressBaseline;
plActionDescriptorHandle plJoltClothActions::s_hSceneView;
plActionDescriptorHandle plJoltClothActions::s_hDragTest;
plActionDescriptorHandle plJoltClothActions::s_hTeleport;
plActionDescriptorHandle plJoltClothActions::s_hTurn;
plActionDescriptorHandle plJoltClothActions::s_hFitFromSkin;
plActionDescriptorHandle plJoltClothActions::s_hCreateCloth;

plInt64 plJoltClothSceneViewAction::s_iView = 0;

namespace
{
  // The menu registration creates an action from its name and icon only, so each drop-down is a type of its own.
  class ClothViewAction : public plJoltClothEnumAction
  {
  public:
    ClothViewAction(const plActionContext& context, const char* szName, const char* szIconPath)
      : plJoltClothEnumAction(context, szName, szIconPath, Type::View)
    {
    }
  };

  class ClothPaintToolAction : public plJoltClothEnumAction
  {
  public:
    ClothPaintToolAction(const plActionContext& context, const char* szName, const char* szIconPath)
      : plJoltClothEnumAction(context, szName, szIconPath, Type::PaintTool)
    {
    }
  };

  class ClothPaintMapAction : public plJoltClothEnumAction
  {
  public:
    ClothPaintMapAction(const plActionContext& context, const char* szName, const char* szIconPath)
      : plJoltClothEnumAction(context, szName, szIconPath, Type::PaintMap)
    {
    }
  };

  plJoltClothAssetDocumentWindow* FindClothWindow(const plActionContext& context)
  {
    return dynamic_cast<plJoltClothAssetDocumentWindow*>(plDocumentWindow::FindWindowByDocument(context.m_pDocument));
  }

  /// The menu lists an enum by name; these enums read better in the order they are declared in.
  void SortByValue(plDynamicArray<plDynamicMenuAction::Item>& ref_entries)
  {
    ref_entries.Sort([](const plDynamicMenuAction::Item& lhs, const plDynamicMenuAction::Item& rhs)
      { return lhs.m_UserValue.ConvertTo<plInt64>() < rhs.m_UserValue.ConvertTo<plInt64>(); });
  }
} // namespace

void plJoltClothActions::RegisterActions()
{
  s_hCategory = PL_REGISTER_CATEGORY("ClothCategory");

  s_hView = PL_REGISTER_DYNAMIC_MENU("Cloth.View", ClothViewAction, ":/EditorFramework/Icons/RenderMode.svg");
  s_hPaintTool = PL_REGISTER_DYNAMIC_MENU("Cloth.PaintTool", ClothPaintToolAction, ":/JoltPlugin/ClothPaint.svg");
  s_hPaintMap = PL_REGISTER_DYNAMIC_MENU("Cloth.PaintMap", ClothPaintMapAction, ":/JoltPlugin/ClothMap.svg");

  s_hBrushRadius = PL_REGISTER_ACTION_1("Cloth.BrushRadius", plActionScope::Document, "Cloth", "", plJoltClothSliderAction, plJoltClothSliderAction::Type::BrushRadius);
  s_hBrushValue = PL_REGISTER_ACTION_1("Cloth.BrushValue", plActionScope::Document, "Cloth", "", plJoltClothSliderAction, plJoltClothSliderAction::Type::BrushValue);
  s_hBrushStrength = PL_REGISTER_ACTION_1("Cloth.BrushStrength", plActionScope::Document, "Cloth", "", plJoltClothSliderAction, plJoltClothSliderAction::Type::BrushStrength);
  s_hWind = PL_REGISTER_ACTION_1("Cloth.Wind", plActionScope::Document, "Cloth", "", plJoltClothSliderAction, plJoltClothSliderAction::Type::Wind);

  s_hMirror = PL_REGISTER_ACTION_1("Cloth.Mirror", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::Mirror);
  s_hAutoMask = PL_REGISTER_ACTION_1("Cloth.AutoMask", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::AutoMask);
  s_hRevertMaps = PL_REGISTER_ACTION_1("Cloth.RevertMaps", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::RevertMaps);
  s_hAutoSelectColliders = PL_REGISTER_ACTION_1("Cloth.AutoSelectColliders", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::AutoSelectColliders);
  s_hStressTest = PL_REGISTER_ACTION_1("Cloth.StressTest", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::StressTest);
  s_hStressBaseline = PL_REGISTER_ACTION_1("Cloth.StressBaseline", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::StressBaseline);

  s_hSceneView = PL_REGISTER_DYNAMIC_MENU("Cloth.SceneView", plJoltClothSceneViewAction, ":/AssetIcons/Jolt_Cloth.svg");

  s_hDragTest = PL_REGISTER_ACTION_1("Cloth.DragTest", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::DragTest);
  s_hTeleport = PL_REGISTER_ACTION_1("Cloth.Teleport", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::Teleport);
  s_hTurn = PL_REGISTER_ACTION_1("Cloth.Turn", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::Turn);
  s_hFitFromSkin = PL_REGISTER_ACTION_1("Cloth.FitFromSkin", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::FitFromSkin);
  s_hCreateCloth = PL_REGISTER_ACTION_1("Cloth.Create", plActionScope::Document, "Cloth", "", plJoltClothButtonAction, plJoltClothButtonAction::Type::CreateCloth);
}

void plJoltClothActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategory);
  plActionManager::UnregisterAction(s_hView);
  plActionManager::UnregisterAction(s_hPaintTool);
  plActionManager::UnregisterAction(s_hPaintMap);
  plActionManager::UnregisterAction(s_hBrushRadius);
  plActionManager::UnregisterAction(s_hBrushValue);
  plActionManager::UnregisterAction(s_hBrushStrength);
  plActionManager::UnregisterAction(s_hWind);
  plActionManager::UnregisterAction(s_hMirror);
  plActionManager::UnregisterAction(s_hAutoMask);
  plActionManager::UnregisterAction(s_hRevertMaps);
  plActionManager::UnregisterAction(s_hAutoSelectColliders);
  plActionManager::UnregisterAction(s_hStressTest);
  plActionManager::UnregisterAction(s_hStressBaseline);
  plActionManager::UnregisterAction(s_hSceneView);
  plActionManager::UnregisterAction(s_hDragTest);
  plActionManager::UnregisterAction(s_hTeleport);
  plActionManager::UnregisterAction(s_hTurn);
  plActionManager::UnregisterAction(s_hFitFromSkin);
  plActionManager::UnregisterAction(s_hCreateCloth);
}

void plJoltClothActions::MapSceneMenuActions()
{
  // next to the Jolt project settings, which the Jolt actions put there
  if (plActionMap* pMap = plActionMapManager::GetActionMap("EditorPluginScene_Scene2MenuBar"))
    pMap->MapAction(s_hSceneView, "G.Plugins.Settings", "Jolt", 2.0f);
}

void plJoltClothActions::MapAnimatedMeshActions()
{
  // the window belongs to the engine's asset plugin, which registers its toolbar before this plugin loads
  if (plActionMap* pMap = plActionMapManager::GetActionMap("AnimatedMeshAssetToolBar"))
    pMap->MapAction(s_hCreateCloth, "", 20.0f);
}

void plJoltClothActions::MapToolBarActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  pMap->MapAction(s_hCategory, "", 12.0f);

  const plStringView sCategory = "ClothCategory";

  // paint: the tool and its map, the brush, then the actions that replace a whole map
  pMap->MapAction(s_hPaintTool, sCategory, 1.0f);
  pMap->MapAction(s_hPaintMap, sCategory, 2.0f);
  pMap->MapAction(s_hBrushRadius, sCategory, 3.0f);
  pMap->MapAction(s_hBrushValue, sCategory, 4.0f);
  pMap->MapAction(s_hBrushStrength, sCategory, 5.0f);
  pMap->MapAction(s_hMirror, sCategory, 6.0f);
  pMap->MapAction(s_hAutoMask, sCategory, 7.0f);
  pMap->MapAction(s_hRevertMaps, sCategory, 8.0f);

  // colliders
  pMap->MapAction(s_hAutoSelectColliders, sCategory, 9.0f);
  pMap->MapAction(s_hFitFromSkin, sCategory, 9.5f);

  // motion: what moves the character in the preview
  pMap->MapAction(s_hWind, sCategory, 10.0f);
  pMap->MapAction(s_hDragTest, sCategory, 10.2f);
  pMap->MapAction(s_hTurn, sCategory, 10.4f);
  pMap->MapAction(s_hTeleport, sCategory, 10.6f);

  pMap->MapAction(s_hStressTest, sCategory, 11.0f);
  pMap->MapAction(s_hStressBaseline, sCategory, 12.0f);
}

void plJoltClothActions::MapViewToolBarActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  pMap->MapAction(s_hView, "", 5.0f);
}

//////////////////////////////////////////////////////////////////////////

plJoltClothEnumAction::plJoltClothEnumAction(const plActionContext& context, const char* szName, const char* szIconPath, Type type)
  : plEnumerationMenuAction(context, szName, szIconPath)
  , m_Type(type)
{
  switch (m_Type)
  {
    case Type::View:
      InitEnumerationType(plGetStaticRTTI<plJoltClothViewMode>());
      break;
    case Type::PaintTool:
      InitEnumerationType(plGetStaticRTTI<plJoltClothPaintTool>());
      break;
    case Type::PaintMap:
      InitEnumerationType(plGetStaticRTTI<plJoltClothPaintMap>());
      break;
  }
}

plJoltClothAssetDocumentWindow* plJoltClothEnumAction::GetWindow() const
{
  return FindClothWindow(m_Context);
}

void plJoltClothEnumAction::GetEntries(plDynamicArray<plDynamicMenuAction::Item>& out_entries)
{
  SUPER::GetEntries(out_entries);
  SortByValue(out_entries);
}

plInt64 plJoltClothEnumAction::GetValue() const
{
  const plJoltClothAssetDocumentWindow* pWindow = GetWindow();
  if (pWindow == nullptr)
    return 0;

  switch (m_Type)
  {
    case Type::View:
      return pWindow->GetViewMode();
    case Type::PaintTool:
      return pWindow->GetPaintTool();
    case Type::PaintMap:
      return pWindow->GetPaintMap();
  }

  return 0;
}

void plJoltClothEnumAction::Execute(const plVariant& value)
{
  plJoltClothAssetDocumentWindow* pWindow = GetWindow();
  if (pWindow == nullptr)
    return;

  const plInt64 iValue = value.ConvertTo<plInt64>();

  switch (m_Type)
  {
    case Type::View:
      pWindow->SetViewMode(static_cast<plJoltClothViewMode::Enum>(iValue));
      break;
    case Type::PaintTool:
      pWindow->SetPaintTool(static_cast<plJoltClothPaintTool::Enum>(iValue));
      break;
    case Type::PaintMap:
      pWindow->SetPaintMap(static_cast<plJoltClothPaintMap::Enum>(iValue));
      break;
  }

  TriggerUpdate();
}

//////////////////////////////////////////////////////////////////////////

plJoltClothSliderAction::plJoltClothSliderAction(const plActionContext& context, const char* szName, Type type)
  : plSliderAction(context, szName)
  , m_Type(type)
{
  const plJoltClothAssetDocumentWindow* pWindow = GetWindow();

  switch (m_Type)
  {
    case Type::BrushRadius:
      SetRange(1, 100);
      SetValue(pWindow != nullptr ? plMath::RoundToInt(pWindow->GetBrushRadius() * 100.0f) : 12);
      break;

    case Type::BrushValue:
      SetRange(0, 100);
      SetValue(pWindow != nullptr ? plMath::RoundToInt(pWindow->GetBrushValue() * 100.0f) : 100);
      break;

    case Type::BrushStrength:
      SetRange(1, 100);
      SetValue(pWindow != nullptr ? plMath::RoundToInt(pWindow->GetBrushStrength() * 100.0f) : 100);
      break;

    case Type::Wind:
      SetRange(0, 30);
      SetValue(pWindow != nullptr ? plMath::RoundToInt(pWindow->GetWind()) : 0);
      break;
  }
}

plJoltClothAssetDocumentWindow* plJoltClothSliderAction::GetWindow() const
{
  return FindClothWindow(m_Context);
}

void plJoltClothSliderAction::Execute(const plVariant& value)
{
  const plInt32 iValue = value.ConvertTo<plInt32>();

  // the toolbar reads the value back from the action
  SetValue(iValue, false);

  plJoltClothAssetDocumentWindow* pWindow = GetWindow();
  if (pWindow == nullptr)
    return;

  switch (m_Type)
  {
    case Type::BrushRadius:
      pWindow->SetBrushRadius(iValue / 100.0f);
      break;
    case Type::BrushValue:
      pWindow->SetBrushValue(iValue / 100.0f);
      break;
    case Type::BrushStrength:
      pWindow->SetBrushStrength(iValue / 100.0f);
      break;
    case Type::Wind:
      pWindow->SetWind(static_cast<float>(iValue));
      break;
  }
}

//////////////////////////////////////////////////////////////////////////

plJoltClothButtonAction::plJoltClothButtonAction(const plActionContext& context, const char* szName, Type type)
  : plButtonAction(context, szName, false, "")
  , m_Type(type)
{
  switch (m_Type)
  {
    case Type::Mirror:
    {
      const plJoltClothAssetDocumentWindow* pWindow = GetWindow();

      SetIconPath(":/JoltPlugin/ClothMirror.svg");
      SetCheckable(true, false);
      SetChecked(pWindow != nullptr && pWindow->GetMirror(), false);
      break;
    }

    case Type::AutoMask:
      SetIconPath(":/JoltPlugin/ClothAutoMask.svg");
      break;

    case Type::RevertMaps:
      SetIconPath(":/JoltPlugin/ClothRevert.svg");
      break;

    case Type::AutoSelectColliders:
      SetIconPath(":/JoltPlugin/ClothColliders.svg");
      break;

    case Type::StressTest:
      SetIconPath(":/JoltPlugin/ClothStress.svg");
      break;

    case Type::StressBaseline:
      SetIconPath(":/JoltPlugin/ClothBaseline.svg");
      break;

    case Type::DragTest:
    {
      const plJoltClothAssetDocumentWindow* pWindow = GetWindow();

      SetIconPath(":/JoltPlugin/ClothDrag.svg");
      SetCheckable(true, false);
      SetChecked(pWindow != nullptr && pWindow->GetDragTest(), false);
      break;
    }

    case Type::Teleport:
      SetIconPath(":/JoltPlugin/ClothTeleport.svg");
      break;

    case Type::Turn:
      SetIconPath(":/JoltPlugin/ClothTurn.svg");
      break;

    case Type::FitFromSkin:
      SetIconPath(":/JoltPlugin/ClothFitColliders.svg");
      break;

    case Type::CreateCloth:
      SetIconPath(":/AssetIcons/Jolt_Cloth.svg");
      break;
  }
}

plJoltClothAssetDocumentWindow* plJoltClothButtonAction::GetWindow() const
{
  return FindClothWindow(m_Context);
}

void plJoltClothButtonAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);

  if (m_Type == Type::CreateCloth)
  {
    // this one sits in the Animated Mesh window
    const plStatus status = plJoltClothAssetDocumentManager::CreateClothForMesh(m_Context.m_pDocument->GetDocumentPath());

    if (status.Failed())
      plLog::Error("Create Cloth: {}", status.m_sMessage);

    return;
  }

  plJoltClothAssetDocumentWindow* pWindow = GetWindow();
  if (pWindow == nullptr)
    return;

  switch (m_Type)
  {
    case Type::Mirror:
      pWindow->SetMirror(!pWindow->GetMirror());
      SetChecked(pWindow->GetMirror());
      break;

    case Type::AutoMask:
      pWindow->AutoMask();
      break;

    case Type::RevertMaps:
      pWindow->RevertMaps();
      break;

    case Type::AutoSelectColliders:
      pWindow->AutoSelectColliders();
      break;

    case Type::StressTest:
      pWindow->RunStressTest();
      break;

    case Type::StressBaseline:
      pWindow->SetStressBaseline();
      break;

    case Type::DragTest:
      pWindow->SetDragTest(!pWindow->GetDragTest());
      SetChecked(pWindow->GetDragTest());
      break;

    case Type::Teleport:
      pWindow->Teleport();
      break;

    case Type::Turn:
      pWindow->TurnTest();
      break;

    case Type::FitFromSkin:
      pWindow->FitCollidersFromSkin();
      break;

    case Type::CreateCloth:
      break;
  }
}

//////////////////////////////////////////////////////////////////////////

plJoltClothSceneViewAction::plJoltClothSceneViewAction(const plActionContext& context, const char* szName, const char* szIconPath)
  : plEnumerationMenuAction(context, szName, szIconPath)
{
  InitEnumerationType(plGetStaticRTTI<plJoltClothSceneView>());
}

void plJoltClothSceneViewAction::GetEntries(plDynamicArray<plDynamicMenuAction::Item>& out_entries)
{
  SUPER::GetEntries(out_entries);
  SortByValue(out_entries);
}

plInt64 plJoltClothSceneViewAction::GetValue() const
{
  return s_iView;
}

void plJoltClothSceneViewAction::Execute(const plVariant& value)
{
  s_iView = value.ConvertTo<plInt64>();

  // the enum has the values of the cvar
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();
  if (pConnection != nullptr && !pConnection->IsProcessCrashed())
  {
    plChangeCVarMsgToEngine msg;
    msg.m_sCVarName = "Jolt.Cloth.View";
    msg.m_NewValue = static_cast<plInt32>(s_iView);
    pConnection->SendMessage(&msg);
  }

  TriggerUpdate();
}
