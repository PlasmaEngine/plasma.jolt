#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/InputContexts/CameraMoveContext.h>
#include <EditorPluginJolt/CollisionMeshAsset/JoltCollisionMeshAsset.h>
#include <Foundation/Types/UniquePtr.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plOrbitCamViewWidget;

class plJoltCollisionMeshAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plJoltCollisionMeshAssetDocumentWindow(plAssetDocument* pDocument);

	virtual int GetCameraMode() const override { return m_iCameraMode; }
  virtual void SetCameraMode(int iMode) override;

protected:
  virtual void InternalRedraw() override;
  virtual void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg) override;

private:
  void SendRedrawMsg();
  void QueryObjectBBox(plInt32 iPurpose = 0);

  plEngineViewConfig m_ViewConfig;
  plOrbitCamViewWidget* m_pViewWidget;
	plUniquePtr<plCameraMoveContext> m_pCameraFlyContext;
  int m_iCameraMode = 0;
  plAssetDocument* m_pAssetDoc;
};
