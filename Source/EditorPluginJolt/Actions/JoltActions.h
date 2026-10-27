#pragma once

#include <EditorPluginJolt/EditorPluginJoltDLL.h>
#include <GuiFoundation/Action/BaseActions.h>
#include <GuiFoundation/GuiFoundationDLL.h>

class PL_EDITORPLUGINJOLT_DLL plJoltActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapMenuActions();

  static plActionDescriptorHandle s_hCategoryJolt;
  static plActionDescriptorHandle s_hProjectSettings;
};

class PL_EDITORPLUGINJOLT_DLL plJoltAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltAction, plButtonAction);

public:
  enum class ActionType
  {
    ProjectSettings,
  };

  plJoltAction(const plActionContext& context, const char* szName, ActionType type);
  ~plJoltAction();

  virtual void Execute(const plVariant& value) override;

private:
  ActionType m_Type;
};

/// \brief The view strips of the collision mesh and destructible mesh windows: Frame, and the destructible mesh's
/// Exploded View slider.
class PL_EDITORPLUGINJOLT_DLL plJoltAssetViewActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapViewToolBarActions(plStringView sMapping, bool bExplodedView);

  static plActionDescriptorHandle s_hFrame;
  static plActionDescriptorHandle s_hExplodedView;
};

/// \brief Moves the orbit camera of the view back to its default position around the bounding box (what F does).
class PL_EDITORPLUGINJOLT_DLL plJoltFrameAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltFrameAction, plButtonAction);

public:
  plJoltFrameAction(const plActionContext& context, const char* szName);

  virtual void Execute(const plVariant& value) override;
};

/// \brief Exploded View of the destructible mesh window: 0..200 maps to an explode factor of 0..2.
class PL_EDITORPLUGINJOLT_DLL plJoltExplodedViewAction : public plSliderAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltExplodedViewAction, plSliderAction);

public:
  plJoltExplodedViewAction(const plActionContext& context, const char* szName);

  virtual void Execute(const plVariant& value) override;
};
