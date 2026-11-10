#pragma once

#include <EditorPluginJolt/EditorPluginJoltDLL.h>
#include <GuiFoundation/Action/BaseActions.h>

class plJoltClothAssetDocumentWindow;

/// \brief What the cloth window draws over the garment. The values match the views of the cvar Jolt.Cloth.View.
struct plJoltClothViewMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Shaded,
    SimMesh,
    MaxDistance,
    Backstop,
    Colliders,
    Strain,

    Default = Shaded
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothViewMode);

/// \brief The map the paint tools work on.
struct plJoltClothPaintMap
{
  using StorageType = plUInt8;

  enum Enum
  {
    MaxDistance,
    BackstopDistance,
    BackstopRadius,
    Stiffness,

    Default = MaxDistance
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothPaintMap);

/// \brief What dragging over the garment does. Off leaves the mouse to the camera.
struct plJoltClothPaintTool
{
  using StorageType = plUInt8;

  enum Enum
  {
    Off,
    Brush,    ///< moves the map towards the brush value
    Smooth,   ///< evens the map out
    Fill,     ///< a click sets the whole piece under the cursor
    Gradient, ///< a drag ramps the piece from 0 at its start to the brush value at its end

    Default = Off
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothPaintTool);

/// \brief The actions of the cloth window: its views, the paint tools and the preview's wind.
class plJoltClothActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapToolBarActions(plStringView sMapping);
  static void MapViewToolBarActions(plStringView sMapping);

  /// \brief Adds the Cloth View menu to the scene window, next to the Jolt project settings.
  static void MapSceneMenuActions();

  /// \brief Adds Create Cloth to the toolbar of the Animated Mesh window.
  static void MapAnimatedMeshActions();

  static plActionDescriptorHandle s_hCategory;
  static plActionDescriptorHandle s_hView;
  static plActionDescriptorHandle s_hPaintTool;
  static plActionDescriptorHandle s_hPaintMap;
  static plActionDescriptorHandle s_hBrushRadius;
  static plActionDescriptorHandle s_hBrushValue;
  static plActionDescriptorHandle s_hBrushStrength;
  static plActionDescriptorHandle s_hMirror;
  static plActionDescriptorHandle s_hAutoMask;
  static plActionDescriptorHandle s_hRevertMaps;
  static plActionDescriptorHandle s_hAutoSelectColliders;
  static plActionDescriptorHandle s_hWind;
  static plActionDescriptorHandle s_hStressTest;
  static plActionDescriptorHandle s_hStressBaseline;
  static plActionDescriptorHandle s_hSceneView;
  static plActionDescriptorHandle s_hDragTest;
  static plActionDescriptorHandle s_hTeleport;
  static plActionDescriptorHandle s_hTurn;
  static plActionDescriptorHandle s_hFitFromSkin;
  static plActionDescriptorHandle s_hCreateCloth;
};

/// \brief The cloth views of a scene: what the cvar Jolt.Cloth.View draws over every garment.
struct plJoltClothSceneView
{
  using StorageType = plUInt8;

  enum Enum
  {
    Off,
    SimMesh,
    MaxDistance,
    Backstop,
    Colliders,
    Strain,
    State,
    Cost,

    Default = Off
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothSceneView);

/// \brief The Cloth View menu of the scene window. It sets the cvar in the engine process, so it shows in every view.
class plJoltClothSceneViewAction : public plEnumerationMenuAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothSceneViewAction, plEnumerationMenuAction);

public:
  plJoltClothSceneViewAction(const plActionContext& context, const char* szName, const char* szIconPath);

  virtual void GetEntries(plDynamicArray<plDynamicMenuAction::Item>& out_entries) override;
  virtual plInt64 GetValue() const override;
  virtual void Execute(const plVariant& value) override;

private:
  static plInt64 s_iView;
};

/// \brief One of the cloth window's drop-downs: the view, the paint tool or the painted map.
class plJoltClothEnumAction : public plEnumerationMenuAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothEnumAction, plEnumerationMenuAction);

public:
  enum class Type
  {
    View,
    PaintTool,
    PaintMap,
  };

  plJoltClothEnumAction(const plActionContext& context, const char* szName, const char* szIconPath, Type type);

  virtual void GetEntries(plDynamicArray<plDynamicMenuAction::Item>& out_entries) override;
  virtual plInt64 GetValue() const override;
  virtual void Execute(const plVariant& value) override;

private:
  plJoltClothAssetDocumentWindow* GetWindow() const;

  Type m_Type;
};

/// \brief One of the cloth window's sliders: brush radius, value and strength, and the preview's wind.
class plJoltClothSliderAction : public plSliderAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothSliderAction, plSliderAction);

public:
  enum class Type
  {
    BrushRadius,   ///< centimetres
    BrushValue,    ///< percent of the map's range
    BrushStrength, ///< percent
    Wind,          ///< metres per second
  };

  plJoltClothSliderAction(const plActionContext& context, const char* szName, Type type);

  virtual void Execute(const plVariant& value) override;

private:
  plJoltClothAssetDocumentWindow* GetWindow() const;

  Type m_Type;
};

/// \brief One of the cloth window's buttons.
class plJoltClothButtonAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothButtonAction, plButtonAction);

public:
  enum class Type
  {
    Mirror,
    AutoMask,
    RevertMaps,
    AutoSelectColliders,
    StressTest,
    StressBaseline,
    DragTest,
    Teleport,
    Turn,
    FitFromSkin,
    CreateCloth, ///< on the Animated Mesh window: makes the cloth asset of its mesh
  };

  plJoltClothButtonAction(const plActionContext& context, const char* szName, Type type);

  virtual void Execute(const plVariant& value) override;

private:
  plJoltClothAssetDocumentWindow* GetWindow() const;

  Type m_Type;
};
