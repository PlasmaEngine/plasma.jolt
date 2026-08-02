#pragma once

#include <EditorFramework/DragDrop/ComponentDragDropHandler.h>

/// \brief Creates a plJoltDestructibleComponent when a Destructible Mesh asset is dropped into a scene.
class plJoltDestructibleMeshComponentDragDropHandler : public plComponentDragDropHandler
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltDestructibleMeshComponentDragDropHandler, plComponentDragDropHandler);

public:
  virtual float CanHandle(const plDragDropInfo* pInfo) const override;
  virtual void OnDragBegin(const plDragDropInfo* pInfo) override;
  virtual void OnDrop(const plDragDropInfo* pInfo) override;
};
