#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/Actions/JoltActions.h>
#include <EditorPluginJolt/Dialogs/JoltNamePrompt.h>
#include <EditorPluginJolt/Dialogs/JoltProjectSettingsEngineDlg.h>
#include <EditorUi/Surface.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>

void UpdateCollisionLayerDynamicEnumValues();
void UpdateWeightCategoryDynamicEnumValues();
void UpdateImpulseTypeDynamicEnumValues();

namespace
{
  const char* s_szReset = "Reset";
  const char* s_szOk = "Ok";
  const char* s_szCancel = "Cancel";

  constexpr float s_fToolRowHeight = 32.0f;
  constexpr float s_fContentTop = plEditorUiTabs::Height + 1 + s_fToolRowHeight + 1;
  constexpr float s_fMatrixPadding = 10.0f;
  constexpr float s_fWheelStep = 40.0f;
  constexpr plUInt32 s_uiMaxLayers = 32;

  const plHashedString s_sMatrixEntry = plMakeHashedString("Jolt.MatrixDraw");
  const plHashedString s_sStatusLeft = plMakeHashedString("Jolt.StatusLeft");
  const plHashedString s_sStatusRight = plMakeHashedString("Jolt.StatusRight");

  plString FormatNumber(float fValue)
  {
    plStringBuilder s;
    s.SetFormat("{}", plArgF(fValue, 1));
    return s;
  }

  plResult ParseNumber(plStringView sText, float fMin, float fMax, float& out_fValue)
  {
    double fValue = 0.0;
    if (plConversionUtils::StringToFloat(sText, fValue).Failed())
      return PL_FAILURE;

    out_fValue = plMath::Clamp(static_cast<float>(fValue), fMin, fMax);
    return PL_SUCCESS;
  }

  int CompareNoCase(plStringView a, plStringView b)
  {
    return a.Compare_NoCase(b);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Table adapters

/// Weight categories by key, sorted by a column: Name, Mass (1-1000), Description.
class plJoltProjectSettingsEngineDlg::WeightAdapter : public plEditorUiTableAdapter
{
public:
  explicit WeightAdapter(plWeightCategoryConfig& ref_config)
    : m_Config(ref_config)
  {
  }

  /// Called after a click on a column title sorted the rows.
  plDelegate<void()> m_Sorted;

  void Reload()
  {
    m_Keys.Clear();
    for (plUInt32 i = 0; i < m_Config.m_Categories.GetCount(); ++i)
      m_Keys.PushBack(m_Config.m_Categories.GetKey(i));

    SortKeys();
  }

  plUInt8 GetKey(plUInt32 uiRow) const { return uiRow < m_Keys.GetCount() ? m_Keys[uiRow] : plWeightCategoryConfig::InvalidKey; }

  plInt32 FindRow(plUInt8 uiKey) const
  {
    const plUInt32 uiIndex = m_Keys.IndexOf(uiKey);
    return uiIndex == plInvalidIndex ? -1 : static_cast<plInt32>(uiIndex);
  }

  plUInt32 GetRowCount() const override { return m_Keys.GetCount(); }

  plString GetCell(plUInt32 uiRow, plUInt32 uiColumn) const override
  {
    const plWeightCategory& category = m_Config.m_Categories[m_Keys[uiRow]];
    switch (uiColumn)
    {
      case 0:
        return category.m_sName.GetString();
      case 1:
        return FormatNumber(category.m_fMass);
      default:
        return category.m_sDescription;
    }
  }

  plStatus SetCell(plUInt32 uiRow, plUInt32 uiColumn, plStringView sText) override
  {
    plWeightCategory& category = m_Config.m_Categories[m_Keys[uiRow]];

    if (uiColumn == 1)
    {
      if (ParseNumber(sText, 1.0f, 1000.0f, category.m_fMass).Failed())
        return plStatus("Enter a mass between 1 and 1000.");
      return plStatus(PL_SUCCESS);
    }

    if (uiColumn == 2)
    {
      category.m_sDescription = sText;
      return plStatus(PL_SUCCESS);
    }

    return plStatus("Use Rename Category to change the name.");
  }

  void Sort(plUInt32 uiColumn, bool bAscending) override
  {
    m_uiSortColumn = uiColumn;
    m_bAscending = bAscending;
    SortKeys();

    if (m_Sorted.IsValid())
      m_Sorted();
  }

private:
  void SortKeys()
  {
    m_Keys.Sort([this](plUInt8 a, plUInt8 b)
      {
        const plWeightCategory& ca = m_Config.m_Categories[a];
        const plWeightCategory& cb = m_Config.m_Categories[b];
        int iOrder = 0;
        if (m_uiSortColumn == 1)
          iOrder = ca.m_fMass < cb.m_fMass ? -1 : (ca.m_fMass > cb.m_fMass ? 1 : 0);
        else if (m_uiSortColumn == 2)
          iOrder = CompareNoCase(ca.m_sDescription, cb.m_sDescription);
        if (iOrder == 0)
          iOrder = CompareNoCase(ca.m_sName.GetView(), cb.m_sName.GetView());
        return m_bAscending ? iOrder < 0 : iOrder > 0; });
  }

  plWeightCategoryConfig& m_Config;
  plHybridArray<plUInt8, 32> m_Keys;
  plUInt32 m_uiSortColumn = 0;
  bool m_bAscending = true;
};

/// Impulse types by key, sorted by a column: Name, Impulse (0-10000), Description.
class plJoltProjectSettingsEngineDlg::ImpulseAdapter : public plEditorUiTableAdapter
{
public:
  explicit ImpulseAdapter(plImpulseTypeConfig& ref_config)
    : m_Config(ref_config)
  {
  }

  /// Called after a click on a column title sorted the rows.
  plDelegate<void()> m_Sorted;

  void Reload()
  {
    m_Keys.Clear();
    for (plUInt32 i = 0; i < m_Config.m_Types.GetCount(); ++i)
      m_Keys.PushBack(m_Config.m_Types.GetKey(i));

    SortKeys();
  }

  plUInt8 GetKey(plUInt32 uiRow) const { return uiRow < m_Keys.GetCount() ? m_Keys[uiRow] : plImpulseTypeConfig::InvalidKey; }

  plInt32 FindRow(plUInt8 uiKey) const
  {
    const plUInt32 uiIndex = m_Keys.IndexOf(uiKey);
    return uiIndex == plInvalidIndex ? -1 : static_cast<plInt32>(uiIndex);
  }

  plUInt32 GetRowCount() const override { return m_Keys.GetCount(); }

  plString GetCell(plUInt32 uiRow, plUInt32 uiColumn) const override
  {
    const plImpulseType& type = m_Config.m_Types[m_Keys[uiRow]];
    switch (uiColumn)
    {
      case 0:
        return type.m_sName.GetString();
      case 1:
        return FormatNumber(type.m_fDefaultValue);
      default:
        return type.m_sDescription;
    }
  }

  plStatus SetCell(plUInt32 uiRow, plUInt32 uiColumn, plStringView sText) override
  {
    plImpulseType& type = m_Config.m_Types[m_Keys[uiRow]];

    if (uiColumn == 1)
    {
      if (ParseNumber(sText, 0.0f, 10000.0f, type.m_fDefaultValue).Failed())
        return plStatus("Enter an impulse between 0 and 10000.");
      return plStatus(PL_SUCCESS);
    }

    if (uiColumn == 2)
    {
      type.m_sDescription = sText;
      return plStatus(PL_SUCCESS);
    }

    return plStatus("Use Rename Type to change the name.");
  }

  void Sort(plUInt32 uiColumn, bool bAscending) override
  {
    m_uiSortColumn = uiColumn;
    m_bAscending = bAscending;
    SortKeys();

    if (m_Sorted.IsValid())
      m_Sorted();
  }

private:
  void SortKeys()
  {
    m_Keys.Sort([this](plUInt8 a, plUInt8 b)
      {
        const plImpulseType& ta = m_Config.m_Types[a];
        const plImpulseType& tb = m_Config.m_Types[b];
        int iOrder = 0;
        if (m_uiSortColumn == 1)
          iOrder = ta.m_fDefaultValue < tb.m_fDefaultValue ? -1 : (ta.m_fDefaultValue > tb.m_fDefaultValue ? 1 : 0);
        else if (m_uiSortColumn == 2)
          iOrder = CompareNoCase(ta.m_sDescription, tb.m_sDescription);
        if (iOrder == 0)
          iOrder = CompareNoCase(ta.m_sName.GetView(), tb.m_sName.GetView());
        return m_bAscending ? iOrder < 0 : iOrder > 0; });
  }

  plImpulseTypeConfig& m_Config;
  plHybridArray<plUInt8, 32> m_Keys;
  plUInt32 m_uiSortColumn = 0;
  bool m_bAscending = true;
};

/// The overrides of one impulse type, one row per weight category: Mass Category, Override (check), Impulse. The
/// impulse is only editable while overridden; checking stores the type's default value, unchecking removes it.
class plJoltProjectSettingsEngineDlg::OverrideAdapter : public plEditorUiTableAdapter
{
public:
  OverrideAdapter(plImpulseTypeConfig& ref_impulses, plWeightCategoryConfig& ref_weights)
    : m_Impulses(ref_impulses)
    , m_Weights(ref_weights)
  {
  }

  void SetImpulse(plUInt8 uiImpulseKey)
  {
    m_uiImpulseKey = uiImpulseKey;
    Reload();
  }

  void Reload()
  {
    m_WeightKeys.Clear();
    if (!m_Impulses.m_Types.Contains(m_uiImpulseKey))
      return;

    for (plUInt32 i = 0; i < m_Weights.m_Categories.GetCount(); ++i)
      m_WeightKeys.PushBack(m_Weights.m_Categories.GetKey(i));

    m_WeightKeys.Sort([this](plUInt8 a, plUInt8 b)
      { return CompareNoCase(m_Weights.m_Categories[a].m_sName.GetView(), m_Weights.m_Categories[b].m_sName.GetView()) < 0; });
  }

  plUInt32 GetRowCount() const override { return m_WeightKeys.GetCount(); }

  plString GetCell(plUInt32 uiRow, plUInt32 uiColumn) const override
  {
    const plUInt8 uiWeightKey = m_WeightKeys[uiRow];
    const auto& overrides = m_Impulses.m_Types[m_uiImpulseKey].m_WeightOverrides;
    const plUInt32 uiOverride = overrides.Find(uiWeightKey);

    switch (uiColumn)
    {
      case 0:
        return m_Weights.m_Categories[uiWeightKey].m_sName.GetString();
      case 1:
        return uiOverride != plInvalidIndex ? "true" : "false";
      default:
        return uiOverride != plInvalidIndex ? FormatNumber(overrides.GetValue(uiOverride)) : plString();
    }
  }

  plStatus SetCell(plUInt32 uiRow, plUInt32 uiColumn, plStringView sText) override
  {
    const plUInt8 uiWeightKey = m_WeightKeys[uiRow];
    plImpulseType& type = m_Impulses.m_Types[m_uiImpulseKey];

    if (uiColumn == 1)
    {
      const bool bOverride = sText.IsEqual_NoCase("true") || sText == "1";
      if (!bOverride)
        type.m_WeightOverrides.RemoveAndCopy(uiWeightKey);
      else if (!type.m_WeightOverrides.Contains(uiWeightKey))
        type.m_WeightOverrides[uiWeightKey] = type.m_fDefaultValue;
      return plStatus(PL_SUCCESS);
    }

    if (uiColumn == 2)
    {
      if (!type.m_WeightOverrides.Contains(uiWeightKey))
        return plStatus("Check Override to set an impulse for this mass category.");

      float fValue = 0.0f;
      if (ParseNumber(sText, 0.0f, 10000.0f, fValue).Failed())
        return plStatus("Enter an impulse between 0 and 10000.");

      type.m_WeightOverrides[uiWeightKey] = fValue;
      return plStatus(PL_SUCCESS);
    }

    return plStatus("The mass categories are edited on the Weight Categories tab.");
  }

private:
  plImpulseTypeConfig& m_Impulses;
  plWeightCategoryConfig& m_Weights;
  plUInt8 m_uiImpulseKey = plImpulseTypeConfig::InvalidKey;
  plHybridArray<plUInt8, 32> m_WeightKeys;
};

//////////////////////////////////////////////////////////////////////////
// plJoltProjectSettingsEngineDlg

plJoltProjectSettingsEngineDlg::plJoltProjectSettingsEngineDlg()
  : plEngineDialog("JoltProjectSettings", "Jolt Project Settings")
{
  SetIcon(":/JoltPlugin/JoltPlugin.svg");
  Load();

  if (!IsReady())
    return;

  SetupButtons();
  SetupTables();
  Subscribe();

  Refresh();
}

plJoltProjectSettingsEngineDlg::~plJoltProjectSettingsEngineDlg()
{
  if (!IsReady())
    return;

  Unsubscribe();
}

bool plJoltProjectSettingsEngineDlg::Run(const plVariant& startup)
{
  const plString sStartup = startup.IsValid() ? startup.ConvertTo<plString>() : plString();

  plJoltProjectSettingsEngineDlg dlg;

  if (sStartup == "WeightCategories")
    dlg.SetCurrentTab(Tab::Weights);
  else if (sStartup == "ImpulseTypes")
    dlg.SetCurrentTab(Tab::Impulses);

  return dlg.Exec() == Accepted;
}

void plJoltProjectSettingsEngineDlg::SetCurrentTab(Tab tab)
{
  m_Tab = tab;

  if (!IsReady())
    return;

  m_pTabRow->SetCurrent(static_cast<plUInt32>(tab));
  Rebuild();
}

plVec2 plJoltProjectSettingsEngineDlg::GetPreferredSize() const
{
  return plVec2(899.0f, 710.0f);
}

// Setup.

void plJoltProjectSettingsEngineDlg::SetupButtons()
{
  plEditorUiDialogButton buttons[3];
  buttons[0].m_sId = s_szReset;
  buttons[0].m_sLabel = "Reset";
  buttons[0].m_Role = plEditorUiDialogButton::Role::Left;
  buttons[0].m_sToolTip = "Reverts to the values loaded when the dialog opened";
  buttons[1].m_sId = s_szOk;
  buttons[1].m_sLabel = "OK";
  buttons[1].m_Role = plEditorUiDialogButton::Role::Accept;
  buttons[2].m_sId = s_szCancel;
  buttons[2].m_sLabel = "Cancel";
  buttons[2].m_Role = plEditorUiDialogButton::Role::Reject;
  GetHost().SetButtons(buttons);
}

void plJoltProjectSettingsEngineDlg::SetupTables()
{
  plEditorUiSurface& surface = *GetSurface();

  m_pTabRow = PL_DEFAULT_NEW(plEditorUiTabs, surface, "Jolt.Tabs");

  m_pWeightAdapter = PL_DEFAULT_NEW(WeightAdapter, m_Weights);
  m_pImpulseAdapter = PL_DEFAULT_NEW(ImpulseAdapter, m_Impulses);
  m_pOverrideAdapter = PL_DEFAULT_NEW(OverrideAdapter, m_Impulses, m_Weights);

  m_pWeightTable = PL_DEFAULT_NEW(plEditorUiTable, surface, "Jolt.Weights", *m_pWeightAdapter);
  m_pImpulseTable = PL_DEFAULT_NEW(plEditorUiTable, surface, "Jolt.Impulses", *m_pImpulseAdapter);
  m_pOverrideTable = PL_DEFAULT_NEW(plEditorUiTable, surface, "Jolt.Overrides", *m_pOverrideAdapter);

  SetupWeightColumns();
  SetupImpulseColumns();
  SetupOverrideColumns();
}

void plJoltProjectSettingsEngineDlg::SetupWeightColumns()
{
  plEditorUiTableColumn columns[3];
  columns[0].m_sTitle = "Name";
  columns[0].m_fWidth = 220.0f;
  columns[1].m_sTitle = "Mass";
  columns[1].m_fWidth = 110.0f;
  columns[1].m_bRightAligned = true;
  columns[1].m_bEditable = true;
  columns[2].m_sTitle = "Description";
  columns[2].m_fWidth = 420.0f;
  columns[2].m_bEditable = true;
  m_pWeightTable->SetPersistKey("JoltProjectSettings/WeightColumns");
  m_pWeightTable->SetColumns(columns);
}

void plJoltProjectSettingsEngineDlg::SetupImpulseColumns()
{
  plEditorUiTableColumn columns[3];
  columns[0].m_sTitle = "Name";
  columns[0].m_fWidth = 170.0f;
  columns[1].m_sTitle = "Impulse";
  columns[1].m_fWidth = 90.0f;
  columns[1].m_bRightAligned = true;
  columns[1].m_bEditable = true;
  columns[2].m_sTitle = "Description";
  columns[2].m_fWidth = 180.0f;
  columns[2].m_bEditable = true;
  m_pImpulseTable->SetPersistKey("JoltProjectSettings/ImpulseColumns");
  m_pImpulseTable->SetColumns(columns);
}

void plJoltProjectSettingsEngineDlg::SetupOverrideColumns()
{
  plEditorUiTableColumn columns[3];
  columns[0].m_sTitle = "Mass Category";
  columns[0].m_fWidth = 160.0f;
  columns[1].m_sTitle = "Override";
  columns[1].m_fWidth = 90.0f;
  columns[1].m_bCheck = true;
  columns[1].m_bEditable = true;
  columns[2].m_sTitle = "Impulse";
  columns[2].m_fWidth = 90.0f;
  columns[2].m_bRightAligned = true;
  columns[2].m_bEditable = true;
  m_pOverrideTable->SetPersistKey("JoltProjectSettings/OverrideColumns");
  m_pOverrideTable->SetColumns(columns);
}

void plJoltProjectSettingsEngineDlg::Subscribe()
{
  plEditorUiSurface& surface = *GetSurface();

  m_pWeightTable->m_Events.AddEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));
  m_pImpulseTable->m_Events.AddEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));
  m_pOverrideTable->m_Events.AddEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));

  // a click on a column title sorts the rows: the highlight follows the selected entry to its new row
  m_pWeightAdapter->m_Sorted = [this]()
  { ShowSelectedRow(*m_pWeightTable, GetSelectedWeightRow()); };
  m_pImpulseAdapter->m_Sorted = [this]()
  { ShowSelectedRow(*m_pImpulseTable, GetSelectedImpulseRow()); };

  m_pTabRow->m_CurrentChanged.AddEventHandler([this](plUInt32 uiTab)
    {
      m_Tab = static_cast<Tab>(uiTab);
      m_Hover = {};
      Rebuild(); });

  m_PointerSubscription = surface.m_PointerEvents.AddEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnPointer, this));
  m_KeySubscription = surface.m_KeyEvents.AddEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnKey, this));
}

void plJoltProjectSettingsEngineDlg::Unsubscribe()
{
  GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  GetSurface()->m_KeyEvents.RemoveEventHandler(m_KeySubscription);

  m_pWeightTable->m_Events.RemoveEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));
  m_pImpulseTable->m_Events.RemoveEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));
  m_pOverrideTable->m_Events.RemoveEventHandler(plMakeDelegate(&plJoltProjectSettingsEngineDlg::OnTableEvent, this));
}

// Configs.

void plJoltProjectSettingsEngineDlg::EnsureConfigFileExists()
{
  EnsureFilterConfigFileExists();
  EnsureWeightsConfigFileExists();
  EnsureImpulseConfigFileExists();
}

void plJoltProjectSettingsEngineDlg::EnsureFilterConfigFileExists()
{
  if (plFileSystem::ExistsFile(plCollisionFilterConfig::s_sConfigFile))
    return;

  plCollisionFilterConfig cfg;

  cfg.SetGroupName(0, "Default");
  cfg.SetGroupName(1, "Transparent");
  cfg.SetGroupName(2, "Debris");
  cfg.SetGroupName(3, "Water");
  cfg.SetGroupName(4, "Foliage");
  cfg.SetGroupName(5, "AI");
  cfg.SetGroupName(6, "Player");
  cfg.SetGroupName(7, "Visibility Raycast");
  cfg.SetGroupName(8, "Interaction Raycast");

  cfg.EnableCollision(0, 0);
  cfg.EnableCollision(0, 1);
  cfg.EnableCollision(0, 2);
  cfg.EnableCollision(0, 3, false);
  cfg.EnableCollision(0, 4, false);
  cfg.EnableCollision(0, 5);
  cfg.EnableCollision(0, 6);
  cfg.EnableCollision(0, 7);
  cfg.EnableCollision(0, 8);

  cfg.EnableCollision(1, 1);
  cfg.EnableCollision(1, 2);
  cfg.EnableCollision(1, 3, false);
  cfg.EnableCollision(1, 4, false);
  cfg.EnableCollision(1, 5);
  cfg.EnableCollision(1, 6);
  cfg.EnableCollision(1, 7, false);
  cfg.EnableCollision(1, 8);

  cfg.EnableCollision(2, 2, false);
  cfg.EnableCollision(2, 3, false);
  cfg.EnableCollision(2, 4, false);
  cfg.EnableCollision(2, 5, false);
  cfg.EnableCollision(2, 6, false);
  cfg.EnableCollision(2, 7, false);
  cfg.EnableCollision(2, 8, false);

  cfg.EnableCollision(3, 3, false);
  cfg.EnableCollision(3, 4, false);
  cfg.EnableCollision(3, 5, false);
  cfg.EnableCollision(3, 6, false);
  cfg.EnableCollision(3, 7, false);
  cfg.EnableCollision(3, 8);

  cfg.EnableCollision(4, 4, false);
  cfg.EnableCollision(4, 5, false);
  cfg.EnableCollision(4, 6, false);
  cfg.EnableCollision(4, 7);
  cfg.EnableCollision(4, 8, false);

  cfg.EnableCollision(5, 5);
  cfg.EnableCollision(5, 6);
  cfg.EnableCollision(5, 7);
  cfg.EnableCollision(5, 8);

  cfg.EnableCollision(6, 6);
  cfg.EnableCollision(6, 7);
  cfg.EnableCollision(6, 8);

  cfg.EnableCollision(7, 7, false);
  cfg.EnableCollision(7, 8, false);

  cfg.EnableCollision(8, 8, false);

  cfg.Save().IgnoreResult();
}

static void AddWeightCfg(plWeightCategoryConfig& ref_cfg, plStringView sName, float fMass, plStringView sDesc)
{
  plUInt8 idx = ref_cfg.GetFreeKey();
  auto& e = ref_cfg.m_Categories[idx];
  e.m_sName.Assign(sName);
  e.m_fMass = fMass;
  e.m_sDescription = sDesc;
}

void plJoltProjectSettingsEngineDlg::EnsureWeightsConfigFileExists()
{
  if (plFileSystem::ExistsFile(plWeightCategoryConfig::s_sConfigFile))
    return;

  plWeightCategoryConfig cfg;
  AddWeightCfg(cfg, "Barrel", 35.0f, "");
  AddWeightCfg(cfg, "Car", 500.0f, "");
  AddWeightCfg(cfg, "Chair", 10.0f, "");
  AddWeightCfg(cfg, "Crate - Large", 100.0f, "larger than 1 meter");
  AddWeightCfg(cfg, "Crate - Medium", 40.0f, "up to 1 meter");
  AddWeightCfg(cfg, "Crate - Small", 10.0f, "smaller than 0.5 meters");
  AddWeightCfg(cfg, "Creature - Small", 10.0f, "small animals, rats, birds");
  AddWeightCfg(cfg, "Creature - Medium", 70.0f, "Humanoids, regular monsters");
  AddWeightCfg(cfg, "Creature - Large", 150.0f, "Large monsters");
  AddWeightCfg(cfg, "Debris", 1.0f, "tiny objects, cans, garbage");
  AddWeightCfg(cfg, "Decoration - Small", 2.0f, "picture frames, plates, mugs");
  AddWeightCfg(cfg, "Decoration - Medium", 5.0f, "paintings, desk lamps, regular vases");
  AddWeightCfg(cfg, "Decoration - Large", 8.0f, "ceiling lamps, large vases");
  AddWeightCfg(cfg, "Furniture - Large", 80.0f, "shelves, tables, large cupboards");
  AddWeightCfg(cfg, "Furniture - Medium", 25.0f, "sideboards, cupboards, small tables");
  AddWeightCfg(cfg, "Furniture - Small", 10.0f, "stools, bedside tables");
  AddWeightCfg(cfg, "Truck", 1000.0f, "trucks, trains, containers, large machinery");

  cfg.Save().IgnoreResult();
}

static void AddForceCfg(plImpulseTypeConfig& ref_cfg, plStringView sName, float fForce, plStringView sDesc)
{
  plUInt8 idx = ref_cfg.GetFreeKey();
  auto& e = ref_cfg.m_Types[idx];
  e.m_sName.Assign(sName);
  e.m_fDefaultValue = fForce;
  e.m_sDescription = sDesc;
}

void plJoltProjectSettingsEngineDlg::EnsureImpulseConfigFileExists()
{
  if (plFileSystem::ExistsFile(plImpulseTypeConfig::s_sConfigFile))
    return;

  plImpulseTypeConfig cfg;
  AddForceCfg(cfg, "Projectile - Light", 10.0f, "");
  AddForceCfg(cfg, "Projectile - Medium", 40.0f, "");
  AddForceCfg(cfg, "Projectile - Heavy", 150.0f, "");

  AddForceCfg(cfg, "Explosion - Small", 150.0f, "");
  AddForceCfg(cfg, "Explosion - Medium", 250.0f, "");
  AddForceCfg(cfg, "Explosion - Large", 500.0f, "");

  AddForceCfg(cfg, "Throw Object", 250.0f, "");

  cfg.Save().IgnoreResult();
}

plStatus plJoltProjectSettingsEngineDlg::SaveConfigs(const plCollisionFilterConfig& layers, const plWeightCategoryConfig& weights, const plImpulseTypeConfig& impulses)
{
  if (layers.Save().Failed())
    return plStatus(plFmt("Failed to save the Collision Layer file\n'{0}'", plCollisionFilterConfig::s_sConfigFile));

  UpdateCollisionLayerDynamicEnumValues();

  if (weights.Save().Failed())
    return plStatus(plFmt("Failed to save the Weight Categories file\n'{0}'", plWeightCategoryConfig::s_sConfigFile));

  UpdateWeightCategoryDynamicEnumValues();

  if (impulses.Save().Failed())
    return plStatus(plFmt("Failed to save the Force Categories file\n'{0}'", plImpulseTypeConfig::s_sConfigFile));

  UpdateImpulseTypeDynamicEnumValues();

  return plStatus(PL_SUCCESS);
}

void plJoltProjectSettingsEngineDlg::Load()
{
  EnsureConfigFileExists();

  if (m_Layers.Load().Failed() || m_Weights.Load().Failed() || m_Impulses.Load().Failed())
    return;

  m_LayersReset = m_Layers;
  m_WeightsReset = m_Weights;
  m_ImpulsesReset = m_Impulses;
}

bool plJoltProjectSettingsEngineDlg::Save()
{
  const plStatus saved = SaveConfigs(m_Layers, m_Weights, m_Impulses);
  if (saved.Failed())
  {
    // one line: the file follows the message
    plStringBuilder sMessage = saved.m_sMessage;
    sMessage.ReplaceAll("\n", " ");
    GetHost().SetValidation(plEditorUiSeverity::Error, sMessage);
    return false;
  }

  GetHost().SetValidation(plEditorUiSeverity::None, {});
  return true;
}

void plJoltProjectSettingsEngineDlg::Reset()
{
  m_Layers = m_LayersReset;
  m_Weights = m_WeightsReset;
  m_Impulses = m_ImpulsesReset;

  GetHost().SetValidation(plEditorUiSeverity::None, {});
  Refresh();
}

void plJoltProjectSettingsEngineDlg::Refresh()
{
  ReadLayers();

  ReloadWeights();
  ReloadImpulses();
  m_uiSelectedWeight = plWeightCategoryConfig::InvalidKey;
  m_uiSelectedImpulse = plImpulseTypeConfig::InvalidKey;
  ShowOverrides(plImpulseTypeConfig::InvalidKey);

  m_iSelectedLayer = -1;
  m_Hover = {};

  UpdateTabs();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::OnButton(plStringView sId)
{
  if (sId == s_szReset)
  {
    Reset();
    return;
  }

  if (sId == s_szOk && !Save())
    return;

  plEngineDialog::OnButton(sId);
}

// Body.

void plJoltProjectSettingsEngineDlg::BuildBody(plUiElement* pBody, const plVec2& vSize)
{
  using namespace plEditorUi;

  const float fWidth = plMath::Max(vSize.x, 200.0f);

  BuildHeader(pBody, fWidth);
  BuildTools(pBody, fWidth);

  // the tab's content and its status row, above the footer
  plUiPanel* pContent = Panel(pBody);
  Fill(pContent, plVec4(0, s_fContentTop, 0, 0));
  pContent->m_Color = Surface();
  BuildContent(pContent);
  StatusRow(pContent, s_sStatusLeft.GetView(), s_sStatusRight.GetView());

  UpdateBindings();

  if (m_Tab == Tab::Layers)
    DrawMatrix();
}

void plJoltProjectSettingsEngineDlg::BuildHeader(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  plUiPanel* pHeader = Panel(pBody);
  pHeader->m_vAnchorMax.y = 0;
  pHeader->m_vOffsets = plVec4(0, 0, 0, plEditorUiTabs::Height);
  pHeader->m_Color = Background();

  plUiPanel* pTabs = m_pTabRow->Build(pHeader, fWidth - 12.0f);
  pTabs->m_vAnchorMin = plVec2(0, 0);
  pTabs->m_vAnchorMax = plVec2(1, 0);
  pTabs->m_vOffsets = plVec4(6, 0, -6, plEditorUiTabs::Height);

  plUiPanel* pLine = Panel(pBody);
  pLine->m_vAnchorMax.y = 0;
  pLine->m_vOffsets = plVec4(0, plEditorUiTabs::Height, 0, 1);
  pLine->m_Color = Line();
}

void plJoltProjectSettingsEngineDlg::BuildTools(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  plBlackboard& board = GetSurface()->GetBlackboard();
  const float fTop = plEditorUiTabs::Height + 1;

  plUiPanel* pRow = Panel(pBody);
  pRow->m_vAnchorMax.y = 0;
  pRow->m_vOffsets = plVec4(0, fTop, 0, s_fToolRowHeight);
  pRow->m_Color = Surface();

  plUiPanel* pLine = Panel(pBody);
  pLine->m_vAnchorMax.y = 0;
  pLine->m_vOffsets = plVec4(0, fTop + s_fToolRowHeight, 0, 1);
  pLine->m_Color = Line();

  const char* szRename = "Rename Layer";
  const char* szAdd = "Add Layer";
  const char* szRemove = "Remove Layer";

  if (m_Tab == Tab::Weights)
  {
    szRename = "Rename Category";
    szAdd = "Add Category";
    szRemove = "Remove Category";
  }
  else if (m_Tab == Tab::Impulses)
  {
    szRename = "Rename Type";
    szAdd = "Add Type";
    szRemove = "Remove Type";
  }

  // Rename on the left; Add and Remove at the right end. Rename and Remove need a picked row.
  const bool bSelection = HasSelection();
  const float y = (s_fToolRowHeight - ControlHeight) * 0.5f;
  PushButton(pRow, board, plRectFloat(6, y, PushButtonWidth(szRename), ControlHeight), szRename, "Jolt.Rename", bSelection);

  const float fRemoveWidth = PushButtonWidth(szRemove, true);
  const float fAddWidth = PushButtonWidth(szAdd, true);
  PushButton(pRow, board, plRectFloat(fWidth - 6 - fRemoveWidth, y, fRemoveWidth, ControlHeight), szRemove, "Jolt.Remove", bSelection, ButtonStyle::Normal, IconPath(Icon::Remove));
  PushButton(pRow, board, plRectFloat(fWidth - 6 - fRemoveWidth - 4 - fAddWidth, y, fAddWidth, ControlHeight), szAdd, "Jolt.Add", true, ButtonStyle::Normal, IconPath(Icon::Add));
}

bool plJoltProjectSettingsEngineDlg::HasSelection() const
{
  switch (m_Tab)
  {
    case Tab::Layers:
      return m_iSelectedLayer >= 0;
    case Tab::Weights:
      return GetSelectedWeightRow() >= 0;
    default:
      return GetSelectedImpulseRow() >= 0;
  }
}

void plJoltProjectSettingsEngineDlg::BuildContent(plUiElement* pContent)
{
  using namespace plEditorUi;

  plUiPanel* pArea = Panel(pContent);
  Fill(pArea, plVec4(0, 0, 0, StatusRowHeight + 1));
  pArea->m_Color = Surface();

  if (m_Tab == Tab::Layers)
  {
    DrawList(pArea, "Jolt.Matrix", s_sMatrixEntry.GetView());
    return;
  }

  if (m_Tab == Tab::Weights)
  {
    m_pWeightTable->Build(pArea);
    return;
  }

  // impulse types on the left (3/5), the overrides of the selected type on the right
  plUiPanel* pLeft = Panel(pArea);
  pLeft->m_vAnchorMax.x = 0.6f;
  pLeft->m_vOffsets = plVec4(0, 0, -1, 0);
  pLeft->m_Color = Surface();
  m_pImpulseTable->Build(pLeft);

  plUiPanel* pSplit = Panel(pArea);
  pSplit->m_vAnchorMin.x = pSplit->m_vAnchorMax.x = 0.6f;
  pSplit->m_vOffsets = plVec4(-1, 0, 1, 0);
  pSplit->m_Color = Line();

  plUiPanel* pRight = Panel(pArea);
  pRight->m_vAnchorMin.x = 0.6f;
  pRight->m_vOffsets = plVec4(0, 0, 0, 0);
  pRight->m_Color = Surface();

  if (GetSelectedImpulseRow() >= 0)
  {
    m_pOverrideTable->Build(pRight);
  }
  else
  {
    EmptyState(pRight, Icon::List, "No impulse type selected", "Pick a type to override its impulse per mass category.");
  }
}

plRectFloat plJoltProjectSettingsEngineDlg::GetMatrixRect() const
{
  const plRectFloat& body = GetBodyRect();
  const float fBottom = plEditorUi::StatusRowHeight + 1;
  return plRectFloat(body.x, body.y + s_fContentTop, body.width, plMath::Max(body.height - s_fContentTop - fBottom, 1.0f));
}

void plJoltProjectSettingsEngineDlg::UpdateTabs()
{
  plEditorUiTab tabs[3];
  tabs[0].m_sLabel = "Collision Layers";
  tabs[1].m_sLabel = "Weight Categories";
  tabs[2].m_sLabel = "Impulse Types";

  plStringBuilder count;
  count.SetFormat("{}", m_LayerGroups.GetCount());
  tabs[0].m_sCount = count;
  count.SetFormat("{}", m_Weights.m_Categories.GetCount());
  tabs[1].m_sCount = count;
  count.SetFormat("{}", m_Impulses.m_Types.GetCount());
  tabs[2].m_sCount = count;

  m_pTabRow->SetTabs(tabs);
  m_pTabRow->SetCurrent(static_cast<plUInt32>(m_Tab));
}

void plJoltProjectSettingsEngineDlg::UpdateBindings()
{
  plBlackboard& board = GetSurface()->GetBlackboard();

  plEditorUi::WriteEnabled(board, "Jolt.Rename", HasSelection());
  plEditorUi::WriteEnabled(board, "Jolt.Remove", HasSelection());

  plStringBuilder left;
  plStringBuilder right;

  switch (m_Tab)
  {
    case Tab::Layers:
    {
      left.SetFormat("{} / {} layers     {} colliding pairs", m_LayerGroups.GetCount(), s_uiMaxLayers, CountCollidingPairs());

      if (m_Hover.m_iRow >= 0 && m_Hover.m_iColumn >= 0)
      {
        const bool bCollide = m_Layers.IsCollisionEnabled(m_LayerGroups[m_Hover.m_iRow], m_LayerGroups[m_Hover.m_iColumn]);
        right.SetFormat("{} \xC3\x97 {}: {}", m_LayerNames[m_Hover.m_iRow], m_LayerNames[m_Hover.m_iColumn], bCollide ? "collide" : "ignore");
      }
      else
      {
        right = "each pair once";
      }
      break;
    }

    case Tab::Weights:
    {
      left.SetFormat("{} categories", m_Weights.m_Categories.GetCount());
      const plInt32 iRow = GetSelectedWeightRow();
      if (iRow >= 0)
        right = m_pWeightAdapter->GetCell(iRow, 0);
      break;
    }

    case Tab::Impulses:
    {
      left.SetFormat("{} impulse types", m_Impulses.m_Types.GetCount());
      const plInt32 iRow = GetSelectedImpulseRow();
      if (iRow >= 0)
        right = m_pImpulseAdapter->GetCell(iRow, 0);
      break;
    }
  }

  board.SetEntryValue(s_sStatusLeft, plString(left));
  board.SetEntryValue(s_sStatusRight, plString(right));
}

plInt32 plJoltProjectSettingsEngineDlg::GetSelectedWeightRow() const
{
  return m_pWeightAdapter->FindRow(m_uiSelectedWeight);
}

plInt32 plJoltProjectSettingsEngineDlg::GetSelectedImpulseRow() const
{
  return m_pImpulseAdapter->FindRow(m_uiSelectedImpulse);
}

// Collision layers.

void plJoltProjectSettingsEngineDlg::ReadLayers()
{
  m_LayerGroups.Clear();
  m_LayerNames.Clear();

  const plUInt32 uiLayers = m_Layers.GetNumNamedGroups();
  for (plUInt32 i = 0; i < uiLayers; ++i)
  {
    const plUInt32 uiGroup = m_Layers.GetNamedGroupIndex(i);
    m_LayerGroups.PushBack(uiGroup);
    m_LayerNames.PushBack(m_Layers.GetGroupName(uiGroup));
  }
}

plEditorUiDraw::PairMatrixLayout plJoltProjectSettingsEngineDlg::GetMatrixLayout() const
{
  plEditorUiDraw::PairMatrixLayout layout;
  layout.m_uiCount = m_LayerGroups.GetCount();
  layout.m_bTriangle = true;

  // wide enough for the longest name after its number
  float fNames = 0.0f;
  for (const plString& sName : m_LayerNames)
    fNames = plMath::Max(fNames, plEditorUi::MeasureText(sName));
  layout.m_fHeaderWidth = plMath::Clamp(plMath::Ceil(fNames) + 44.0f, 120.0f, 260.0f);

  layout.m_vTopLeft = plVec2(s_fMatrixPadding, s_fMatrixPadding) - m_vMatrixScroll;
  return layout;
}

void plJoltProjectSettingsEngineDlg::ClampMatrixScroll()
{
  plEditorUiDraw::PairMatrixLayout layout = GetMatrixLayout();
  const plVec2 vSize = layout.GetSize();
  const plRectFloat area = GetMatrixRect();

  m_vMatrixScroll.x = plMath::Clamp(m_vMatrixScroll.x, 0.0f, plMath::Max(vSize.x + 2 * s_fMatrixPadding - area.width, 0.0f));
  m_vMatrixScroll.y = plMath::Clamp(m_vMatrixScroll.y, 0.0f, plMath::Max(vSize.y + 2 * s_fMatrixPadding - area.height, 0.0f));
}

void plJoltProjectSettingsEngineDlg::DrawMatrix()
{
  plHybridArray<plStringView, 32> names;
  for (const plString& sName : m_LayerNames)
    names.PushBack(sName);

  m_Writer.Clear();
  plEditorUiDraw::PairMatrix(
    m_Writer, GetMatrixLayout(), names, [this](plUInt32 uiRow, plUInt32 uiColumn)
    { return m_Layers.IsCollisionEnabled(m_LayerGroups[uiRow], m_LayerGroups[uiColumn]); },
    m_Hover, m_iSelectedLayer);
  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sMatrixEntry);
}

plUInt32 plJoltProjectSettingsEngineDlg::CountCollidingPairs() const
{
  plUInt32 uiPairs = 0;
  for (plUInt32 r = 0; r < m_LayerGroups.GetCount(); ++r)
  {
    for (plUInt32 c = 0; c <= r; ++c)
    {
      if (m_Layers.IsCollisionEnabled(m_LayerGroups[r], m_LayerGroups[c]))
        ++uiPairs;
    }
  }
  return uiPairs;
}

void plJoltProjectSettingsEngineDlg::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  if (m_Tab != Tab::Layers)
    return;

  const plRectFloat area = GetMatrixRect();
  const bool bInside = area.Contains(e.m_vPosition) && e.m_sElement.IsEmpty();
  const plEditorUiDraw::PairMatrixHit hit = bInside ? plEditorUiDraw::HitPairMatrix(GetMatrixLayout(), e.m_vPosition - area.GetTopLeft()) : plEditorUiDraw::PairMatrixHit();

  switch (e.m_Type)
  {
    case Type::Move:
    case Type::DragMove:
      if (hit.m_iRow != m_Hover.m_iRow || hit.m_iColumn != m_Hover.m_iColumn)
      {
        m_Hover = hit;
        DrawMatrix();
        UpdateBindings();
      }
      return;

    case Type::Press:
      if (e.m_Button != plEditorUiPointerEvent::Button::Left || hit.m_iRow < 0)
        return;

      if (hit.m_iColumn >= 0)
      {
        // a pair collides both ways
        const plUInt32 a = m_LayerGroups[hit.m_iRow];
        const plUInt32 b = m_LayerGroups[hit.m_iColumn];
        m_Layers.EnableCollision(a, b, !m_Layers.IsCollisionEnabled(a, b));
      }
      else
      {
        m_iSelectedLayer = hit.m_iRow;
      }

      DrawMatrix();
      UpdateBindings();
      return;

    case Type::Wheel:
      if (!bInside)
        return;

      if (e.HasModifier(plEditorUiPointerEvent::Shift))
        m_vMatrixScroll.x -= e.m_fWheel * s_fWheelStep;
      else
        m_vMatrixScroll.y -= e.m_fWheel * s_fWheelStep;

      ClampMatrixScroll();
      DrawMatrix();
      return;

    default:
      return;
  }
}

plString plJoltProjectSettingsEngineDlg::GetToolTipAt(const plVec2& vLogical) const
{
  const plRectFloat area = GetMatrixRect();
  if (m_Tab != Tab::Layers || !area.Contains(vLogical))
    return plEngineDialog::GetToolTipAt(vLogical);

  // the column headers only show numbers: name the layer
  const plEditorUiDraw::PairMatrixLayout layout = GetMatrixLayout();
  const plVec2 vLocal = vLogical - area.GetTopLeft();
  for (plUInt32 c = 0; c < layout.m_uiCount; ++c)
  {
    const plRectFloat cell = layout.GetCellRect(layout.m_uiCount - 1, c);
    const plRectFloat header(cell.x, layout.m_vTopLeft.y, cell.width, layout.m_fHeaderHeight);
    if (header.Contains(vLocal))
      return m_LayerNames[c];
  }

  return plEngineDialog::GetToolTipAt(vLogical);
}

void plJoltProjectSettingsEngineDlg::AddLayer()
{
  const plUInt32 uiNewIdx = m_Layers.FindUnnamedGroup();

  if (uiNewIdx == plInvalidIndex)
  {
    plEditorUiServices::MessageBoxInformation("The maximum number of collision layers has been reached.");
    return;
  }

  const plString sName = plJoltNamePrompt::Ask("Add Layer", {}, plJoltNamePrompt::LayerValidator(m_Layers, plInvalidIndex, "A Collision Layer with the given name already exists."));
  if (sName.IsEmpty())
    return;

  m_Layers.SetGroupName(uiNewIdx, sName);

  ReadLayers();
  UpdateTabs();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RenameLayer()
{
  if (m_iSelectedLayer < 0 || m_iSelectedLayer >= static_cast<plInt32>(m_LayerGroups.GetCount()))
    return;

  const plUInt32 uiGroup = m_LayerGroups[m_iSelectedLayer];

  // the layer's own name is not taken
  const plString sName = plJoltNamePrompt::Ask("Rename Layer", m_Layers.GetGroupName(uiGroup), plJoltNamePrompt::LayerValidator(m_Layers, uiGroup, "A collision layer with that name already exists."));
  if (sName.IsEmpty())
    return;

  m_Layers.SetGroupName(uiGroup, sName);

  ReadLayers();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RemoveLayer()
{
  if (m_iSelectedLayer < 0 || m_iSelectedLayer >= static_cast<plInt32>(m_LayerGroups.GetCount()))
    return;

  if (!AskYesNo(this, "Remove Collision Layer", "Remove selected Collision Layer?", false))
    return;

  m_Layers.SetGroupName(m_LayerGroups[m_iSelectedLayer], "");
  m_iSelectedLayer = -1;
  m_Hover = {};

  ReadLayers();
  UpdateTabs();
  ClampMatrixScroll();
  Rebuild();
}

// Weight categories and impulse types.

// A table keeps filling the rows it last counted when the dialog grows, also while its tab is hidden: each table reads
// its rows again whenever its adapter changes.

void plJoltProjectSettingsEngineDlg::ReloadWeights()
{
  m_pWeightAdapter->Reload();
  m_pWeightTable->Refresh();

  // the overrides have one row per weight category
  m_pOverrideAdapter->Reload();
  m_pOverrideTable->Refresh();
}

void plJoltProjectSettingsEngineDlg::ReloadImpulses()
{
  m_pImpulseAdapter->Reload();
  m_pImpulseTable->Refresh();
}

void plJoltProjectSettingsEngineDlg::ShowOverrides(plUInt8 uiImpulseKey)
{
  m_pOverrideAdapter->SetImpulse(uiImpulseKey);
  m_pOverrideTable->Refresh();
}

void plJoltProjectSettingsEngineDlg::OnTableEvent(const plEditorUiTableEvent& e)
{
  if (e.m_Type == plEditorUiTableEvent::Type::EditFailed)
  {
    // shown after the table finished handling the key, not inside it
    const plString sMessage = e.m_sMessage;
    m_Timers.Start(plTime::MakeZero(), [sMessage]()
      { plEditorUiServices::MessageBoxWarning(sMessage.GetView()); });
    return;
  }

  if (e.m_Type == plEditorUiTableEvent::Type::LayoutChanged)
  {
    Rebuild();
    return;
  }

  if (m_Tab == Tab::Weights)
    PickWeight();
  else if (m_Tab == Tab::Impulses)
    PickImpulse();

  UpdateBindings();
}

void plJoltProjectSettingsEngineDlg::PickWeight()
{
  m_uiSelectedWeight = m_pWeightAdapter->GetKey(m_pWeightTable->GetCurrentRow());
}

void plJoltProjectSettingsEngineDlg::PickImpulse()
{
  // the current row of the impulse table picks the type whose overrides show on the right; a range of rows keeps it
  const plUInt8 uiKey = m_pImpulseAdapter->GetKey(m_pImpulseTable->GetCurrentRow());
  if (uiKey == plImpulseTypeConfig::InvalidKey || uiKey == m_uiSelectedImpulse)
    return;

  plUInt32 uiFirstRow = 0, uiFirstColumn = 0, uiLastRow = 0, uiLastColumn = 0;
  m_pImpulseTable->GetSelection(uiFirstRow, uiFirstColumn, uiLastRow, uiLastColumn);
  PL_IGNORE_UNUSED(uiFirstColumn);
  PL_IGNORE_UNUSED(uiLastColumn);

  if (uiFirstRow != uiLastRow)
    return;

  m_uiSelectedImpulse = uiKey;
  ShowOverrides(uiKey);
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::ShowSelectedRow(plEditorUiTable& ref_table, plInt32 iRow)
{
  // a table highlights a display row: after the rows moved it moves to the row of the selected entry
  if (iRow >= 0)
    ref_table.SetCurrentCell(static_cast<plUInt32>(iRow), ref_table.GetCurrentColumn());
}

void plJoltProjectSettingsEngineDlg::OnKey(const plEditorUiKeyEvent& e)
{
  // Enter in an open cell edit commits the cell; the dialog host would press OK before the table received the commit
  if (e.m_bHandled || e.m_Key != plEditorUiKeyEvent::Key::Enter || e.m_uiModifiers != 0 || !IsEditingCell())
    return;

  GetSurface()->GetCanvas().InjectNavAction(plUiNavAction::Accept);
  e.m_bHandled = true;
}

bool plJoltProjectSettingsEngineDlg::IsEditingCell() const
{
  return m_pWeightTable->IsEditing() || m_pImpulseTable->IsEditing() || m_pOverrideTable->IsEditing();
}

void plJoltProjectSettingsEngineDlg::AddCategory()
{
  const plUInt8 uiFreeKey = m_Weights.GetFreeKey();
  if (uiFreeKey == plWeightCategoryConfig::InvalidKey)
  {
    plEditorUiServices::MessageBoxWarning("You managed to create too many categories.");
    return;
  }

  const plString sName = plJoltNamePrompt::Ask("Add Weight Category", {}, plJoltNamePrompt::WeightValidator(m_Weights));
  if (sName.IsEmpty())
    return;

  m_Weights.m_Categories[uiFreeKey].m_sName.Assign(sName);

  m_Weights.m_Categories.Sort();
  ReloadWeights();
  ShowSelectedRow(*m_pWeightTable, GetSelectedWeightRow());
  UpdateTabs();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RenameCategory()
{
  const plUInt8 uiKey = m_uiSelectedWeight;
  if (!m_Weights.m_Categories.Contains(uiKey))
    return;

  // the category's own name is not taken
  const plString sName = plJoltNamePrompt::Ask("Rename Category", m_Weights.m_Categories[uiKey].m_sName.GetView(), plJoltNamePrompt::WeightValidator(m_Weights, uiKey));
  if (sName.IsEmpty())
    return;

  m_Weights.m_Categories[uiKey].m_sName.Assign(sName);

  ReloadWeights();
  ShowSelectedRow(*m_pWeightTable, GetSelectedWeightRow());
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RemoveCategory()
{
  if (!m_Weights.m_Categories.Contains(m_uiSelectedWeight))
    return;

  if (!AskYesNo(this, "Remove Weight Category", "Remove selected category?", false))
    return;

  m_Weights.m_Categories.RemoveAndCopy(m_uiSelectedWeight);
  m_uiSelectedWeight = plWeightCategoryConfig::InvalidKey;

  ReloadWeights();
  UpdateTabs();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::AddImpulse()
{
  const plUInt8 uiFreeKey = m_Impulses.GetFreeKey();
  if (uiFreeKey == plImpulseTypeConfig::InvalidKey)
  {
    plEditorUiServices::MessageBoxWarning("You managed to create too many impulse types.");
    return;
  }

  const plString sName = plJoltNamePrompt::Ask("Add Impulse Type", {}, plJoltNamePrompt::ImpulseValidator(m_Impulses));
  if (sName.IsEmpty())
    return;

  m_Impulses.m_Types[uiFreeKey].m_sName.Assign(sName);

  m_Impulses.m_Types.Sort();
  ReloadImpulses();
  m_uiSelectedImpulse = plImpulseTypeConfig::InvalidKey;
  ShowOverrides(plImpulseTypeConfig::InvalidKey);
  UpdateTabs();
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RenameImpulse()
{
  const plUInt8 uiKey = m_uiSelectedImpulse;
  if (!m_Impulses.m_Types.Contains(uiKey))
    return;

  // the type's own name is not taken
  const plString sName = plJoltNamePrompt::Ask("Rename Impulse Type", m_Impulses.m_Types[uiKey].m_sName.GetView(), plJoltNamePrompt::ImpulseValidator(m_Impulses, uiKey));
  if (sName.IsEmpty())
    return;

  m_Impulses.m_Types[uiKey].m_sName.Assign(sName);

  ReloadImpulses();
  ShowSelectedRow(*m_pImpulseTable, GetSelectedImpulseRow());
  Rebuild();
}

void plJoltProjectSettingsEngineDlg::RemoveImpulse()
{
  if (!m_Impulses.m_Types.Contains(m_uiSelectedImpulse))
    return;

  if (!AskYesNo(this, "Remove Impulse Type", "Remove selected impulse type?", false))
    return;

  m_Impulses.m_Types.RemoveAndCopy(m_uiSelectedImpulse);
  m_uiSelectedImpulse = plImpulseTypeConfig::InvalidKey;
  ShowOverrides(plImpulseTypeConfig::InvalidKey);

  ReloadImpulses();
  UpdateTabs();
  Rebuild();
}

// Events.

void plJoltProjectSettingsEngineDlg::Defer(plDelegate<void()> action)
{
  // prompts and message boxes open after the UI event has been handled, not inside it
  m_Timers.Start(plTime::MakeZero(), [action]()
    { action(); });
}

void plJoltProjectSettingsEngineDlg::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();

  if (sId == "Jolt.Add")
  {
    Defer([this]()
      {
        if (m_Tab == Tab::Layers)
          AddLayer();
        else if (m_Tab == Tab::Weights)
          AddCategory();
        else
          AddImpulse(); });
  }
  else if (sId == "Jolt.Rename")
  {
    Defer([this]()
      {
        if (m_Tab == Tab::Layers)
          RenameLayer();
        else if (m_Tab == Tab::Weights)
          RenameCategory();
        else
          RenameImpulse(); });
  }
  else if (sId == "Jolt.Remove")
  {
    Defer([this]()
      {
        if (m_Tab == Tab::Layers)
          RemoveLayer();
        else if (m_Tab == Tab::Weights)
          RemoveCategory();
        else
          RemoveImpulse(); });
  }
}
