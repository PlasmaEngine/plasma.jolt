#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.moc.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <Foundation/Types/UniquePtr.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plQtOrbitCamViewWidget;
class QSlider;

/// \brief Document window for the Destructible Mesh asset: 3D preview of the fractured chunks
/// with an 'exploded view' slider, plus the property grid.
class plQtDestructibleMeshAssetDocumentWindow : public plQtEngineDocumentWindow
{
public:
  plQtDestructibleMeshAssetDocumentWindow(plAssetDocument* pDocument);

  virtual int GetCameraMode() const override { return m_iCameraMode; }
  virtual void SetCameraMode(int iMode) override;

protected:
  virtual void InternalRedraw() override;
  virtual void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg) override;

private:
  void SendRedrawMsg();
  void QueryObjectBBox(plInt32 iPurpose = 0);

  plEngineViewConfig m_ViewConfig;
  plQtOrbitCamViewWidget* m_pViewWidget = nullptr;
  plUniquePtr<plCameraMoveContext> m_pCameraFlyContext;
  int m_iCameraMode = 0;

  QSlider* m_pExplodeSlider = nullptr;
  float m_fExplodeFactor = 0.0f;
};
