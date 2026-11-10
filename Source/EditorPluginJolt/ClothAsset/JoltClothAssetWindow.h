#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <EditorFramework/InputContexts/EditorInputContext.h>
#include <EditorFramework/InputContexts/SelectionContext.h>
#include <EditorPluginJolt/ClothAsset/JoltClothActions.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAsset.h>
#include <Foundation/Types/UniquePtr.h>
#include <JoltPlugin/CharacterCloth/JoltClothMapTools.h>

class plOrbitCamViewWidget;
class plJoltClothAssetDocumentWindow;
struct plCommandHistoryEvent;
struct plDocumentEvent;

/// \brief Takes the left mouse button of the cloth view while a paint tool is on, and hands the strokes to the window.
/// Alt and the other buttons still move the camera.
class plJoltClothPaintContext : public plEditorInputContext
{
public:
  plJoltClothPaintContext(plJoltClothAssetDocumentWindow* pWindow, plEngineViewWidget* pView);

protected:
  virtual void DoFocusLost(bool bCancel) override;
  virtual plEditorInput DoMousePressEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseReleaseEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseMoveEvent(const plEditorInputEvent& e) override;
  virtual void OnSetOwner(plEngineDocumentWindow* pOwnerWindow, plEngineViewWidget* pOwnerView) override {}

private:
  plJoltClothAssetDocumentWindow* m_pWindow = nullptr;
  bool m_bStroke = false;
};

/// \brief The cloth window: the character with its cloth simulating, the views of its maps and colliders, and the
/// tools that paint the maps.
class plJoltClothAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plJoltClothAssetDocumentWindow(plJoltClothAssetDocument* pDocument);
  ~plJoltClothAssetDocumentWindow();

  plJoltClothAssetDocument* GetClothDocument() const;

  virtual int GetCameraMode() const override { return m_iCameraMode; }
  virtual void SetCameraMode(int iMode) override;

  plJoltClothViewMode::Enum GetViewMode() const { return m_ViewMode; }
  void SetViewMode(plJoltClothViewMode::Enum mode) { m_ViewMode = mode; }

  plJoltClothPaintTool::Enum GetPaintTool() const { return m_PaintTool; }
  void SetPaintTool(plJoltClothPaintTool::Enum tool);

  plJoltClothPaintMap::Enum GetPaintMap() const { return m_PaintMap; }
  void SetPaintMap(plJoltClothPaintMap::Enum map);

  float GetBrushRadius() const { return m_fBrushRadius; }
  void SetBrushRadius(float fRadius) { m_fBrushRadius = fRadius; }

  /// \brief The brush value as a fraction of the painted map's range.
  float GetBrushValue() const { return m_fBrushValue; }
  void SetBrushValue(float fValue) { m_fBrushValue = fValue; }

  float GetBrushStrength() const { return m_fBrushStrength; }
  void SetBrushStrength(float fStrength) { m_fBrushStrength = fStrength; }

  bool GetMirror() const { return m_bMirror; }
  void SetMirror(bool bMirror) { m_bMirror = bMirror; }

  float GetWind() const { return m_fWind; }
  void SetWind(float fWind) { m_fWind = fWind; }

  /// \brief While the drag test is on, dragging in the view pulls the character along the ground and letting go lets
  /// it swing back. It shows how the cloth lags behind a move. A paint tool switches it off.
  bool GetDragTest() const { return m_bDragTest; }
  void SetDragTest(bool bOn);

  /// \brief Jumps the preview's character away and back, as a respawn does, to show what the teleport mode makes of it.
  void Teleport();

  /// \brief Turns the preview's character once around in a second.
  void TurnTest();

  /// \brief Replaces the max distance map with a first guess: the top of every piece pinned, the rest ramping up.
  void AutoMask();

  /// \brief Throws the painted maps away, back to the imported masks and the asset's defaults.
  void RevertMaps();

  /// \brief Lists the joints whose skeleton shapes the cloth can reach, so that the list can be edited.
  void AutoSelectColliders();

  /// \brief Replaces the asset's own colliders with capsules fitted to the body's skin, for the parts the cloth can
  /// reach. For characters whose skeleton has no shapes.
  void FitCollidersFromSkin();

  /// \brief Runs the cloth through the motion script in the preview and shows what it found.
  void RunStressTest();

  /// \brief Keeps the last stress test result next to the asset, as what later runs are compared with.
  void SetStressBaseline();

  // called by plJoltClothPaintContext
  bool BeginStroke(const plEditorInputEvent& e);
  void ContinueStroke(const plEditorInputEvent& e);
  void EndStroke(bool bCancel);
  void HoverBrush(const plEditorInputEvent& e);

protected:
  virtual void InternalRedraw() override;
  virtual void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg) override;

private:
  void SendRedrawMsg();
  void QueryObjectBBox(plInt32 iPurpose = 0);
  void CommandEventHandler(const plCommandHistoryEvent& e);
  void DocumentSavedHandler(const plDocumentEvent& e);

  /// Cooks the cloth from the document as it is and shows it in the preview, without saving anything.
  void SendLiveResourcePreview();
  void SendCookedResource();
  void RestoreResource();

  /// The painted maps as they go into the document: the maps painted before, and the one that just changed.
  void GetMapsToStore(plJoltClothPaintMap::Enum changedMap, plJoltClothPaintedMaps& out_maps) const;

  /// Takes the sim mesh and maps of the last cook as what the tools paint on.
  void UpdatePaintData();
  plDynamicArray<float>& GetWorkingMap();
  float GetMapRange() const;
  bool PickSurface(const plEditorInputEvent& e, plJoltClothMapTools::RayHit& out_hit) const;
  plVec3 MirrorPosition(const plVec3& vBindPosition) const;
  void ApplyBrushAt(const plVec3& vBindPosition);
  void SendPaintValues();
  void UpdateInputContexts();
  bool PickGround(const plEditorInputEvent& e, plVec3& out_vPosition) const;

  /// Where along the stress timeline the mouse is, from 0 to 1, and whether it is over the timeline at all.
  bool GetStressTimelinePosition(const plEditorInputEvent& e, float& out_fWhere) const;
  void ShowStressFrame(float fWhere);
  void SendPreviewCommand(plStringView sWhat);
  void ShowStressResult(plStringView sText, const plVariant& worst);
  plString GetStressBaselineFile() const;

  plEngineViewConfig m_ViewConfig;
  plOrbitCamViewWidget* m_pViewWidget = nullptr;
  plUniquePtr<plCameraMoveContext> m_pCameraFlyContext;
  plUniquePtr<plJoltClothPaintContext> m_pPaintContext;
  int m_iCameraMode = 0;

  // The handles of the asset's own colliders are switched on in the properties. While they show, the view picks
  // what is under the mouse so that they can be grabbed.
  plUniquePtr<plSelectionContext> m_pHandleContext;
  bool m_bHandlesShown = false;

  plJoltClothViewMode::Enum m_ViewMode = plJoltClothViewMode::Shaded;
  plJoltClothPaintTool::Enum m_PaintTool = plJoltClothPaintTool::Off;
  plJoltClothPaintMap::Enum m_PaintMap = plJoltClothPaintMap::MaxDistance;
  float m_fBrushRadius = 0.12f;
  float m_fBrushValue = 1.0f;
  float m_fBrushStrength = 1.0f;
  bool m_bMirror = false;
  float m_fWind = 0.0f;

  // The last cook, and what the tools paint on: its sim mesh in bind space and a working copy of its maps.
  plJoltClothDescriptor m_Cooked;
  bool m_bCookValid = false;
  plJoltClothMapTopology m_Topology;
  plJoltClothPaintedMaps m_Working;
  plMat4 m_mBindToWorld = plMat4::MakeIdentity();
  bool m_bPaintValuesDirty = true;

  // the stroke in progress
  bool m_bStroke = false;
  plDynamicArray<float> m_StrokeStart;
  plDynamicArray<float> m_StrokeWeights;
  plVec3 m_vStrokeFrom = plVec3::MakeZero();
  plUInt32 m_uiStrokeIsland = plInvalidIndex;

  // where the brush is drawn in the preview
  plVec3 m_vBrushWorldPosition = plVec3::MakeZero();
  bool m_bBrushVisible = false;

  // the drag test: where on the ground the drag started, and how far the character is pulled from its place
  bool m_bDragTest = false;
  bool m_bDragging = false;
  plVec3 m_vDragStart = plVec3::MakeZero();
  plVec3 m_vDragOffset = plVec3::MakeZero();

  plDataBuffer m_LastStressResult; ///< the worst values of the last stress test, as a baseline is stored

  // the stress timeline that the preview draws along the top of the view
  bool m_bStressShown = false;
  bool m_bStressScrub = false;
  float m_fStressScrubbedTo = -1.0f;
  bool m_bStressFramePending = false; ///< the preview is still on its way to the frame that was asked for
  float m_fStressFrameAsked = -1.0f;
  float m_fStressFrameWanted = -1.0f;
};
