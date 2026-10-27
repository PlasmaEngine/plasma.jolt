#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <Foundation/Types/UniquePtr.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plOrbitCamViewWidget;

/// \brief Document window for the Destructible Mesh asset: 3D preview of the fractured chunks
/// with an 'exploded view' slider in the view strip, plus the property grid.
class plDestructibleMeshAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plDestructibleMeshAssetDocumentWindow(plAssetDocument* pDocument);

  virtual int GetCameraMode() const override { return m_iCameraMode; }
  virtual void SetCameraMode(int iMode) override;

  /// \brief How far the chunks are pushed apart in the preview, 0..2.
  float GetExplodeFactor() const { return m_fExplodeFactor; }
  void SetExplodeFactor(float fFactor);

protected:
  virtual void InternalRedraw() override;
  virtual void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg) override;

private:
  void SendRedrawMsg();
  void QueryObjectBBox(plInt32 iPurpose = 0);

  plEngineViewConfig m_ViewConfig;
  plOrbitCamViewWidget* m_pViewWidget = nullptr;
  plUniquePtr<plCameraMoveContext> m_pCameraFlyContext;
  int m_iCameraMode = 0;

  float m_fExplodeFactor = 0.0f;
};
