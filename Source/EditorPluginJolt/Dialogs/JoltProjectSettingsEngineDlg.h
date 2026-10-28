#pragma once

#include <EditorPluginJolt/EditorPluginJoltDLL.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Table.h>
#include <EditorUi/Tabs.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <GameEngine/Physics/ImpulseType.h>
#include <GameEngine/Physics/WeightCategory.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineDialog.h>

struct plEditorUiKeyEvent;
struct plEditorUiPointerEvent;

/// \brief The Jolt Project Settings drawn by the engine.
///
/// Three tabs: the collision layer pair matrix (a triangle, each pair once), the weight categories table and the
/// impulse types table with the per mass category overrides. Each tab has a row of tools and a status row. The footer
/// has Reset (back to the values loaded at open), OK and Cancel; Enter presses OK unless a table has focus, Escape
/// presses Cancel. OK saves the three config files; a failed save shows in the validation line and the dialog stays
/// open. Add and Rename ask for the name in the engine prompt, which refuses names that are taken.
class plJoltProjectSettingsEngineDlg : public plEngineDialog
{
public:
  enum class Tab : plUInt8
  {
    Layers,
    Weights,
    Impulses,
  };

  plJoltProjectSettingsEngineDlg();
  ~plJoltProjectSettingsEngineDlg();

  /// Runs the dialog. The startup value "WeightCategories" or "ImpulseTypes" opens that tab. Returns true when OK saved
  /// the settings.
  static bool Run(const plVariant& startup);

  void SetCurrentTab(Tab tab);

  /// Writes the default collision layers, weight categories and impulse types for each config file the project lacks.
  static void EnsureConfigFileExists();

  /// Saves the collision layers, weight categories and impulse types in this order and refreshes the matching dynamic
  /// enum after each file. Stops at the first file that fails, with the message naming it.
  static plStatus SaveConfigs(const plCollisionFilterConfig& layers, const plWeightCategoryConfig& weights, const plImpulseTypeConfig& impulses);

protected:
  void BuildBody(plUiElement* pBody, const plVec2& vSize) override;
  void OnButton(plStringView sId) override;
  void OnUiEvent(const plMsgUiEvent& event) override;
  plString GetToolTipAt(const plVec2& vLogical) const override;
  plVec2 GetPreferredSize() const override;

private:
  class WeightAdapter;
  class ImpulseAdapter;
  class OverrideAdapter;

  // Setup
  void SetupButtons();
  void SetupTables();
  void SetupWeightColumns();
  void SetupImpulseColumns();
  void SetupOverrideColumns();
  void Subscribe();
  void Unsubscribe();

  // Configs
  static void EnsureFilterConfigFileExists();
  static void EnsureWeightsConfigFileExists();
  static void EnsureImpulseConfigFileExists();
  void Load();
  bool Save();
  void Reset();
  void Refresh();

  // Body
  void BuildHeader(plUiElement* pBody, float fWidth);
  void BuildTools(plUiElement* pBody, float fWidth);
  void BuildContent(plUiElement* pContent);
  plRectFloat GetMatrixRect() const;
  bool HasSelection() const;
  void UpdateBindings();
  void UpdateTabs();

  // Collision layers
  void ReadLayers();
  plEditorUiDraw::PairMatrixLayout GetMatrixLayout() const;
  void DrawMatrix();
  void ClampMatrixScroll();
  plUInt32 CountCollidingPairs() const;
  void OnPointer(const plEditorUiPointerEvent& e);
  void AddLayer();
  void RenameLayer();
  void RemoveLayer();

  // Weight categories and impulse types
  void ReloadWeights();
  void ReloadImpulses();
  void ShowOverrides(plUInt8 uiImpulseKey);
  void OnTableEvent(const struct plEditorUiTableEvent& e);
  void PickWeight();
  void PickImpulse();
  void ShowSelectedRow(plEditorUiTable& ref_table, plInt32 iRow);
  void OnKey(const plEditorUiKeyEvent& e);
  bool IsEditingCell() const;
  void AddCategory();
  void RenameCategory();
  void RemoveCategory();
  void AddImpulse();
  void RenameImpulse();
  void RemoveImpulse();
  plInt32 GetSelectedWeightRow() const;
  plInt32 GetSelectedImpulseRow() const;
  void Defer(plDelegate<void()> action);

  plCollisionFilterConfig m_Layers;
  plCollisionFilterConfig m_LayersReset;
  plWeightCategoryConfig m_Weights;
  plWeightCategoryConfig m_WeightsReset;
  plImpulseTypeConfig m_Impulses;
  plImpulseTypeConfig m_ImpulsesReset;

  Tab m_Tab = Tab::Layers;

  // collision layers: named groups in display order, their names, the matrix state
  plHybridArray<plUInt32, 32> m_LayerGroups;
  plHybridArray<plString, 32> m_LayerNames;
  plInt32 m_iSelectedLayer = -1;
  plEditorUiDraw::PairMatrixHit m_Hover;
  plVec2 m_vMatrixScroll = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;

  plUniquePtr<plEditorUiTabs> m_pTabRow;
  plUniquePtr<WeightAdapter> m_pWeightAdapter;
  plUniquePtr<ImpulseAdapter> m_pImpulseAdapter;
  plUniquePtr<OverrideAdapter> m_pOverrideAdapter;
  plUniquePtr<plEditorUiTable> m_pWeightTable;
  plUniquePtr<plEditorUiTable> m_pImpulseTable;
  plUniquePtr<plEditorUiTable> m_pOverrideTable;
  // The selection is an entry key, so it stays on the entry when the rows move. A table has a current cell from the
  // start; it counts once picked.
  plUInt8 m_uiSelectedWeight = plWeightCategoryConfig::InvalidKey;
  plUInt8 m_uiSelectedImpulse = plImpulseTypeConfig::InvalidKey; ///< its overrides show on the right

  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_KeySubscription = 0;

  plEditorUiTimerGroup m_Timers; ///< the deferred prompts and messages; stopped when the dialog goes
};
