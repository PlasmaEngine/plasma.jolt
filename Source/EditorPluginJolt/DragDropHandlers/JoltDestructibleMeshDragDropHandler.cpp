#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/DragDrop/DragDropInfo.h>
#include <EditorPluginJolt/DragDropHandlers/JoltDestructibleMeshDragDropHandler.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltDestructibleMeshComponentDragDropHandler, 1, plRTTIDefaultAllocator<plJoltDestructibleMeshComponentDragDropHandler>)
PL_END_DYNAMIC_REFLECTED_TYPE;

float plJoltDestructibleMeshComponentDragDropHandler::CanHandle(const plDragDropInfo* pInfo) const
{
  if (plComponentDragDropHandler::CanHandle(pInfo) == 0.0f)
    return 0.0f;

  return IsSpecificAssetType(pInfo, "DestructibleMesh") ? 1.0f : 0.0f;
}

void plJoltDestructibleMeshComponentDragDropHandler::OnDragBegin(const plDragDropInfo* pInfo)
{
  plComponentDragDropHandler::OnDragBegin(pInfo);

  constexpr const char* szComponentType = "plJoltDestructibleComponent";
  constexpr const char* szPropertyName = "DestructibleMesh";

  if (pInfo->m_sTargetContext == "viewport")
  {
    CreateDropObject(pInfo->m_vDropPosition, szComponentType, szPropertyName, GetAssetGuidString(pInfo), plUuid(), -1);
  }
  else if (!pInfo->m_bCtrlKeyDown && pInfo->m_iTargetObjectInsertChildIndex == -1)
  {
    AttachComponentToObject(szComponentType, szPropertyName, GetAssetGuidString(pInfo), pInfo->m_TargetObject);
    m_DraggedObjects.PushBack(pInfo->m_TargetObject);
  }
  else
  {
    CreateDropObject(pInfo->m_vDropPosition, szComponentType, szPropertyName, GetAssetGuidString(pInfo), pInfo->m_TargetObject, pInfo->m_iTargetObjectInsertChildIndex);
  }

  BeginTemporaryCommands();
}

void plJoltDestructibleMeshComponentDragDropHandler::OnDrop(const plDragDropInfo* pInfo)
{
  plComponentDragDropHandler::OnDrop(pInfo);
  SelectCreatedObjects();
}
