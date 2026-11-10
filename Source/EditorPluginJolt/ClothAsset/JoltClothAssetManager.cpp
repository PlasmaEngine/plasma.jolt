#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Assets/AssetBrowserContextMenu.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAsset.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetManager.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetWindow.h>
#include <Foundation/IO/OSFile.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

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

    // through the command history, so that the document's properties and its preview see the mesh as well
    plObjectCommandAccessor accessor(pCloth->GetCommandHistory());
    accessor.StartTransaction("Set Cloth Mesh");
    const plStatus set = accessor.SetValueByName(pCloth->GetPropertyObject(), "Mesh", sMeshGuid.GetView());
    accessor.FinishTransaction();

    if (set.Failed())
      return set;

    const plStatus saved = pCloth->SaveDocument();
    if (saved.Failed())
      return saved;
  }

  // the window opens once the menu that asked for it is gone
  pApp->OpenDocumentQueued(sClothFile);

  return plStatus(PL_SUCCESS);
}

void plJoltClothAssetDocumentManager::FindUsersWithoutCloth(const plUuid& clothGuid, plStringView sMeshGuid, plDynamicArray<plString>& out_documents)
{
  out_documents.Clear();

  if (!plConversionUtils::IsStringUuid(sMeshGuid))
    return;

  const plUuid meshGuid = plConversionUtils::ConvertStringToUuid(sMeshGuid);
  plAssetCurator* pCurator = plAssetCurator::GetSingleton();

  plSet<plUuid> meshUsers;
  plSet<plUuid> clothUsers;
  pCurator->FindAllUses(meshGuid, meshUsers, false);
  pCurator->FindAllUses(clothGuid, clothUsers, false);

  for (const plUuid& user : meshUsers)
  {
    if (clothUsers.Contains(user))
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(user, sGuid);

    auto asset = pCurator->FindSubAsset(sGuid);
    if (!asset.isValid() || asset->m_pAssetInfo == nullptr)
      continue;

    const plString& sPath = asset->m_pAssetInfo->m_Path.GetAbsolutePath();

    if (plPathUtils::HasExtension(sPath, "plScene") || plPathUtils::HasExtension(sPath, "plPrefab"))
      out_documents.PushBack(sPath);
  }

  out_documents.Sort();
}

plUInt32 plJoltClothAssetDocumentManager::AddClothComponents(plStringView sDocumentAbsPath, const plUuid& clothGuid, plStringView sMeshGuid)
{
  const plRTTI* pMeshType = plRTTI::FindTypeByName("plAnimatedMeshComponent");
  const plRTTI* pClothType = plRTTI::FindTypeByName("plJoltCharacterClothComponent");
  if (pMeshType == nullptr || pClothType == nullptr)
    return 0;

  // a document that is open keeps its window; one that is not is opened without one and closed again
  plDocument* pDocument = nullptr;
  bool bWasOpen = false;

  for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
  {
    if (plDocument* pOpen = pManager->GetDocumentByPath(sDocumentAbsPath))
    {
      pDocument = pOpen;
      bWasOpen = true;
      break;
    }
  }

  if (pDocument == nullptr)
    pDocument = plEditorApp::GetSingleton()->OpenDocument(sDocumentAbsPath, plDocumentFlags::None);

  if (pDocument == nullptr)
    return 0;

  plStringBuilder sClothGuid;
  plConversionUtils::ToString(clothGuid, sClothGuid);

  // the game objects whose Animated Mesh draws the mesh and that have no cloth component yet
  plHybridArray<const plDocumentObject*, 16> owners;
  plHybridArray<const plDocumentObject*, 64> stack;
  stack.PushBack(pDocument->GetObjectManager()->GetRootObject());

  while (!stack.IsEmpty())
  {
    const plDocumentObject* pObject = stack.PeekBack();
    stack.PopBack();

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      stack.PushBack(pChild);
    }

    if (!pObject->GetType()->IsDerivedFrom(pMeshType) || pObject->GetParent() == nullptr)
      continue;

    if (pObject->GetTypeAccessor().GetValue("Mesh").ConvertTo<plString>() != sMeshGuid)
      continue;

    bool bHasCloth = false;
    for (const plDocumentObject* pSibling : pObject->GetParent()->GetChildren())
    {
      if (pSibling->GetType()->IsDerivedFrom(pClothType))
        bHasCloth = true;
    }

    if (!bHasCloth)
      owners.PushBack(pObject->GetParent());
  }

  if (!owners.IsEmpty())
  {
    plObjectCommandAccessor accessor(pDocument->GetCommandHistory());
    accessor.StartTransaction("Add Character Cloth");

    for (const plDocumentObject* pOwner : owners)
    {
      plUuid guid = plUuid::MakeUuid();

      if (accessor.AddObjectByName(pOwner, "Components", -1, pClothType, guid).Succeeded())
        accessor.SetValueByName(accessor.GetObject(guid), "Cloth", sClothGuid.GetView()).IgnoreResult();
    }

    accessor.FinishTransaction();

    const plStatus saved = pDocument->SaveDocument();
    if (saved.Failed())
      plLog::Error("'{}' could not be saved: {}", sDocumentAbsPath, saved.m_sMessage);
  }

  if (!bWasOpen)
    pDocument->GetDocumentManager()->CloseDocument(pDocument);

  return owners.GetCount();
}

void plJoltClothAssetDocumentManager::OfferClothComponents(const plUuid& clothGuid, plStringView sMeshGuid)
{
  static plSet<plString> s_Asked;

  plDynamicArray<plString> users;
  FindUsersWithoutCloth(clothGuid, sMeshGuid, users);

  plStringBuilder sKey;
  plStringBuilder sList;
  plDynamicArray<plString> toAsk;

  for (const plString& sUser : users)
  {
    plConversionUtils::ToString(clothGuid, sKey);
    sKey.Append("|", sUser);

    if (s_Asked.Contains(sKey))
      continue;

    s_Asked.Insert(sKey);
    toAsk.PushBack(sUser);

    if (toAsk.GetCount() <= 8)
      sList.Append("\n  ", plPathUtils::GetFileNameAndExtension(sUser));
  }

  if (toAsk.IsEmpty())
    return;

  if (toAsk.GetCount() > 8)
    sList.AppendFormat("\n  and {} more", toAsk.GetCount() - 8);

  const auto answer = plEditorUiServices::MessageBoxQuestion(plFmt("These scenes and prefabs draw the mesh without its cloth:{}\n\nAdd a Character Cloth component next to the Animated Mesh in each of them?", sList), plEditorUiMessageButton::Yes | plEditorUiMessageButton::No, plEditorUiMessageButton::Yes);

  if (answer != plEditorUiMessageButton::Yes)
    return;

  plUInt32 uiAdded = 0;
  for (const plString& sUser : toAsk)
  {
    uiAdded += AddClothComponents(sUser, clothGuid, sMeshGuid);
  }

  plLog::Success("Character Cloth added to {} object(s) in {} document(s).", uiAdded, toAsk.GetCount());
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
