#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginJolt/EnginePluginJoltDLL.h>
#include <JoltPlugin/CharacterCloth/JoltClothResource.h>
#include <JoltPlugin/CharacterCloth/JoltClothStressTest.h>

class plJoltCharacterClothComponent;

/// \brief The preview of the cloth window: the cloth's mesh with its garments simulating, a clip playing and wind
/// blowing if asked for, the cloth views, and the painted map while a paint tool is on.
class PL_ENGINEPLUGINJOLT_DLL plJoltClothContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothContext, plEngineProcessDocumentContext);

public:
  plJoltClothContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

  /// \brief What the view writes into its corner: sizes of the cloth and what it costs. Empty without a cloth.
  void GetStatsText(plStringBuilder& out_sText);

  /// \brief Draws the timeline of the last stress test along the top of a view: one cell per frame, coloured by the
  /// gate that failed in it, with the parts of the test named. Nothing while no result is shown.
  void DrawStressTimeline(const plViewHandle& hView, float fWidth, float fHeight);

  bool m_bDisplayGrid = true;

protected:
  virtual void OnInitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override;
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override;
  virtual bool UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext) override;

private:
  void QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg);

  /// Makes the preview's components again from the current settings. Assumes the world's write lock.
  void RebuildPreview();

  /// Makes the preview's components for a clip (none for the bind pose), with the cloth simulating or skinned.
  void RebuildPreview(plStringView sClip, bool bSimulate);
  plJoltCharacterClothComponent* GetCloth();
  void DrawPaintOverlay();

  /// Moves the preview's character for the drag test, the turn test and the teleport. Once per redraw.
  void UpdatePreviewMotion();

  /// One part of the stress test: a move of the motion script, or a clip.
  struct StressPart
  {
    plString m_sName;
    plUInt32 m_uiFirstFrame = 0;
    plUInt32 m_uiNumFrames = 0;
  };

  /// Runs the motion script and the given clips on the preview's character and sends the editor what it found.
  /// sClips: "clip|name" entries separated by ';'.
  void RunStressTest(plStringView sClips);

  /// Plays the stress test in the preview's world: the motion script on the bind pose, then every clip. Stops after
  /// uiStopAtFrame (plInvalidIndex plays all of it) and leaves the world as it is then. With pTest every frame is
  /// measured; out_pParts gets the parts with their frames. Assumes the world's write lock.
  void SimulateStress(plUInt32 uiStopAtFrame, plJoltClothStressTest* pTest, plDynamicArray<StressPart>* out_pParts);

  /// Shows the preview as it was in one frame of the last stress test, standing still.
  void ShowStressFrame(plUInt32 uiFrame);

  /// Takes the timeline away and lets the preview play again.
  void LeaveStress();

  /// The world transforms of the joints of every body shape, from the pose the mesh draws. False without a pose.
  bool GetBodyJoints(plDynamicArray<plTransform>& out_joints);

  /// The world transforms of the named joints (separated by ';') in the pose the mesh draws, as
  /// "joint|px py pz qx qy qz qw;" entries. Empty without a pose.
  void GetJointTransformsText(plStringView sJoints, plStringBuilder& out_sText);

  plGameObject* m_pGameObject = nullptr;
  plComponentHandle m_hMeshComponent;
  plComponentHandle m_hAnimationComponent;
  plComponentHandle m_hClothComponent;
  plJoltClothResourceHandle m_hCloth;

  plString m_sMesh;
  plString m_sClip;
  plInt32 m_iView = 0;
  bool m_bPaintMode = false;
  float m_fWind = 0.0f;
  plUInt32 m_uiThumbnailWaitFrames = 0;

  // what moves the character in the preview
  plTime m_LastMotionUpdate;
  plVec3 m_vDragTarget = plVec3::MakeZero();   ///< where the drag test wants the character; zero is its place
  plVec3 m_vDragPosition = plVec3::MakeZero(); ///< where the character is on its way there
  float m_fTurnLeft = 0.0f;                    ///< degrees of the turn test still to go
  float m_fYaw = 0.0f;
  plUInt32 m_uiTeleportFrames = 0;
  plVec3 m_vAppliedPosition = plVec3::MakeZero();
  float m_fAppliedYaw = 0.0f;

  // the last stress test: what it measured, and what it played
  plJoltClothStressResult m_StressResult;
  plDynamicArray<StressPart> m_StressParts;
  plHybridArray<plString, 4> m_StressClips;     ///< the clips that were played
  plHybridArray<plString, 4> m_StressClipNames; ///< what the timeline calls them
  plJoltClothDescriptor m_StressBody;           ///< every shape of the body, as colliders
  bool m_bStressShown = false;
  plUInt32 m_uiStressFrame = plInvalidIndex; ///< the frame the preview stands still in, or plInvalidIndex

  // what the paint tool shows
  plDataBuffer m_PaintValues; ///< per particle, 0 to 255 over the range of the painted map
  plVec3 m_vBrushPosition = plVec3::MakeZero();
  float m_fBrushRadius = 0.0f;
  bool m_bBrushVisible = false;
};
