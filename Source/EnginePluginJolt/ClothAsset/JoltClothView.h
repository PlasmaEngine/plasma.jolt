#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessViewContext.h>

class plJoltClothContext;

class plJoltClothViewContext : public plEngineProcessViewContext
{
public:
  plJoltClothViewContext(plJoltClothContext* pContext);
  ~plJoltClothViewContext();

  bool UpdateThumbnailCamera(const plBoundingBoxSphere& bounds);

protected:
  virtual plViewHandle CreateView() override;
  virtual void SetCamera(const plViewRedrawMsgToEngine* pMsg) override;

  plJoltClothContext* m_pContext = nullptr;
};
