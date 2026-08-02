#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessViewContext.h>
#include <EnginePluginJolt/EnginePluginJoltDLL.h>

class plDestructibleMeshContext;

class plDestructibleMeshViewContext : public plEngineProcessViewContext
{
public:
  plDestructibleMeshViewContext(plDestructibleMeshContext* pContext);
  ~plDestructibleMeshViewContext();

  bool UpdateThumbnailCamera(const plBoundingBoxSphere& bounds);

protected:
  virtual plViewHandle CreateView() override;
  virtual void SetCamera(const plViewRedrawMsgToEngine* pMsg) override;

  plDestructibleMeshContext* m_pContext = nullptr;
};
