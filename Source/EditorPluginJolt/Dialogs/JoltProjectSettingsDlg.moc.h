#pragma once

#include <EditorPluginJolt/EditorPluginJoltDLL.h>
#include <EditorPluginJolt/ui_JoltProjectSettingsDlg.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Strings/HashedString.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <GameEngine/Physics/ImpulseType.h>
#include <GameEngine/Physics/WeightCategory.h>
#include <QDialog>

class plQtJoltProjectSettingsDlg : public QDialog, public Ui_JoltProjectSettingsDlg
{
public:
  Q_OBJECT

public:
  plQtJoltProjectSettingsDlg(const plVariant& startup = {}, QWidget* pParent = nullptr);

  static void EnsureConfigFileExists();

private Q_SLOTS:
  void onCheckBoxClicked(bool checked);
  void onWeightChanged(double value);
  void onWeightDescChanged(const QString& txt);
  void onImpulseChanged(double value);
  void onImpulseDescChanged(const QString& txt);
  void onImpulseOverrideChecked(int);
  void onImpulseOverrideValue(double fValue);
  void on_DefaultButtons_clicked(QAbstractButton* pButton);
  void on_ButtonAddLayer_clicked();
  void on_ButtonRemoveLayer_clicked();
  void on_ButtonRenameLayer_clicked();
  void on_FilterTable_itemSelectionChanged();
  void on_ImpulsesTable_itemSelectionChanged();
  void on_WeightsTable_itemSelectionChanged();

  void on_ButtonAddCategory_clicked();
  void on_ButtonRemoveCategory_clicked();
  void on_ButtonRenameCategory_clicked();

  void on_ButtonAddImpulse_clicked();
  void on_ButtonRemoveImpulse_clicked();
  void on_ButtonRenameImpulse_clicked();

private:
  static void EnsureFilterConfigFileExists();
  static void EnsureWeightsConfigFileExists();
  static void EnsureImpulseConfigFileExists();

  void SetupFilterTable();
  void SetupWeightTable();
  void SetupImpulseTable();

  plResult Save();
  plResult Load();

  plUInt32 m_IndexRemap[32];
  plHybridArray<plHashedString, 32> m_RowToWeight;
  plHybridArray<plHashedString, 32> m_RowToImpulse;
  plCollisionFilterConfig m_Config;
  plCollisionFilterConfig m_ConfigReset;
  plWeightCategoryConfig m_WeightConfig;
  plWeightCategoryConfig m_WeightConfigReset;
  plImpulseTypeConfig m_ImpulseConfig;
  plImpulseTypeConfig m_ImpulseConfigReset;
};
