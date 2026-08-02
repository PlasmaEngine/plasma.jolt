#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginJolt/EnginePluginJoltDLL.h>
#include <JoltPlugin/Resources/JoltDestructibleMeshResource.h>

class plObjectSelectionMsgToEngine;
class plRenderContext;

/// \brief Engine-process document context for the Destructible Mesh asset preview.
///
/// Creates one game object per chunk (each rendering its slice of the baked render mesh),
/// so the fracture can be inspected. The editor window controls an 'exploded view' factor
/// that pushes each chunk away from the mesh center along its centroid direction.
class PL_ENGINEPLUGINJOLT_DLL plDestructibleMeshContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plDestructibleMeshContext, plEngineProcessDocumentContext);

public:
  plDestructibleMeshContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

  const plJoltDestructibleMeshResourceHandle& GetResource() const { return m_hDestructible; }

  /// \brief Called every view redraw; (re)builds the chunk preview objects once the resource is loaded or was re-transformed.
  void UpdatePreview();

  bool m_bDisplayGrid = true;

protected:
  virtual void OnInitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override;
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override;
  virtual bool UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext) override;

private:
  void QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg);
  void SetExplodeFactor(float fExplode);
  void ApplyExplode();

  plGameObject* m_pRootObject = nullptr;
  plJoltDestructibleMeshResourceHandle m_hDestructible;

  plUInt32 m_uiRebuiltChangeCounter = 0xFFFFFFFF;
  float m_fExplode = 0.0f;

  plDynamicArray<plGameObjectHandle> m_ChunkObjects;
  plDynamicArray<plVec3> m_ChunkOffsets; ///< Explode direction per chunk (centroid relative to the mesh center).
};
