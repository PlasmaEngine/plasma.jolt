#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Assets/AssetBrowserContextMenu.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAsset.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetManager.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetWindow.h>
#include <Foundation/IO/OSFile.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothAssetDocumentManager, 1, plRTTIDefaultAllocator<plJoltClothAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plJoltClothAssetDocumentManager::plJoltClothAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plJoltClothAssetDocumentManager::OnDocumentManagerEvent, this));
  plAssetBrowserContextMenu::s_Events.AddEventHandler(plMakeDelegate(&plJoltClothAssetDocumentManager::OnAssetBrowserContextMenu, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Jolt_Cloth";
  m_DocTypeDesc.m_sFileExtension = "plJoltClothAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/Jolt_Cloth.svg";
  m_DocTypeDesc.m_sAssetCategory = "Physics";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plJoltClothAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Jolt_Cloth");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinJoltCloth";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::SupportsThumbnail;
}

plJoltClothAssetDocumentManager::~plJoltClothAssetDocumentManager()
{
  plAssetBrowserContextMenu::s_Events.RemoveEventHandler(plMakeDelegate(&plJoltClothAssetDocumentManager::OnAssetBrowserContextMenu, this));
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plJoltClothAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plJoltClothAssetDocumentManager::OnAssetBrowserContextMenu(const plAssetBrowserContextMenuEvent& e)
{
  // an animated mesh on its own: offer to make its cloth
  if (e.m_SelectedFiles.GetCount() != 1 || !plPathUtils::HasExtension(e.m_SelectedFiles[0], "plAnimatedMeshAsset"))
    return;

  const plString sMesh = e.m_SelectedFiles[0];

  e.m_AddAction("Create Cloth", ":/AssetIcons/Jolt_Cloth.svg", [sMesh]()
    {
      const plStatus status = CreateClothForMesh(sMesh);

      if (status.Failed())
        plLog::Error("Create Cloth: {}", status.m_sMessage); });
}

plStatus plJoltClothAssetDocumentManager::CreateClothForMesh(plStringView sMeshAssetAbsPath)
{
  plStringBuilder sMeshGuid;

  {
    auto mesh = plAssetCurator::GetSingleton()->FindSubAsset(sMeshAssetAbsPath);
    if (!mesh.isValid())
      return plStatus(plFmt("'{}' is not an asset of this project.", sMeshAssetAbsPath));

    plConversionUtils::ToString(mesh->m_Data.m_Guid, sMeshGuid);
  }

  plStringBuilder sClothFile = sMeshAssetAbsPath;
  sClothFile.ChangeFileExtension("plJoltClothAsset");

  plEditorApp* pApp = plEditorApp::GetSingleton();

  if (!plOSFile::ExistsFile(sClothFile))
  {
    plDocument* pDocument = pApp->CreateDocument(sClothFile, plDocumentFlags::None);

    plJoltClothAssetDocument* pCloth = plDynamicCast<plJoltClothAssetDocument*>(pDocument);
    if (pCloth == nullptr)
      return plStatus(plFmt("The cloth asset '{}' could not be created.", sClothFile));

    pCloth->GetPropertyObject()->GetTypeAccessor().SetValue("Mesh", sMeshGuid.GetView());

    const plStatus saved = pCloth->SaveDocument();
    if (saved.Failed())
      return saved;
  }

  // the window opens once the menu that asked for it is gone
  pApp->OpenDocumentQueued(sClothFile);

  return plStatus(PL_SUCCESS);
}

void plJoltClothAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plJoltClothAssetDocument>())
      {
        new plJoltClothAssetDocumentWindow(static_cast<plJoltClothAssetDocument*>(e.m_pDocument)); // NOLINT: Not a memory leak
      }
    }
    break;

    default:
      break;
  }
}

void plJoltClothAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plJoltClothAssetDocument(sPath);
}

void plJoltClothAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
