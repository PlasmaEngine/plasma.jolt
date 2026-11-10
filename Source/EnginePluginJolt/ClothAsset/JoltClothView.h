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
  virtual void HandleViewMessage(const plEditorEngineViewMsg* pMsg) override;

  /// Tells the editor what is under a pixel of the view. Only the collider handles can be picked here.
  void PickObjectAt(plUInt16 x, plUInt16 y);

  plJoltClothContext* m_pContext = nullptr;
};
