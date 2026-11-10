#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginJolt/EnginePluginJoltDLL.h>
#include <JoltPlugin/CharacterCloth/JoltClothResource.h>

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
  plJoltCharacterClothComponent* GetCloth();
  void DrawPaintOverlay();

  /// Runs the motion script on the cloth and sends the editor what it found.
  void RunStressTest();

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

  // what the paint tool shows
  plDataBuffer m_PaintValues; ///< per particle, 0 to 255 over the range of the painted map
  plVec3 m_vBrushPosition = plVec3::MakeZero();
  float m_fBrushRadius = 0.0f;
  bool m_bBrushVisible = false;
};
