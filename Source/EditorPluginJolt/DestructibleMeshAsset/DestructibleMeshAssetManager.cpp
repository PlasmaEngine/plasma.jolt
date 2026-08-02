#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAsset.h>
#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetManager.h>
#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetWindow.moc.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plDestructibleMeshAssetDocumentManager, 1, plRTTIDefaultAllocator<plDestructibleMeshAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plDestructibleMeshAssetDocumentManager::plDestructibleMeshAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plDestructibleMeshAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "DestructibleMesh";
  m_DocTypeDesc.m_sFileExtension = "plDestructibleMeshAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/DestructibleMesh.svg";
  m_DocTypeDesc.m_sAssetCategory = "Physics";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plDestructibleMeshAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_DestructibleMesh");

  m_DocTypeDesc.m_sResourceFileExtension = "plDestructibleMesh";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave | plAssetDocumentFlags::SupportsThumbnail;

  plQtImageCache::GetSingleton()->RegisterTypeImage("DestructibleMesh", QPixmap(":/AssetIcons/DestructibleMesh.svg"));
}

plDestructibleMeshAssetDocumentManager::~plDestructibleMeshAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plDestructibleMeshAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plDestructibleMeshAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plDestructibleMeshAssetDocument>())
      {
        new plQtDestructibleMeshAssetDocumentWindow(static_cast<plAssetDocument*>(e.m_pDocument)); // NOLINT: not a memory leak
      }
    }
    break;

    default:
      break;
  }
}

void plDestructibleMeshAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plDestructibleMeshAssetDocument(sPath);
}

void plDestructibleMeshAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
