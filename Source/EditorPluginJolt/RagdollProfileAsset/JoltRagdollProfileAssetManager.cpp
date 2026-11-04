#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAsset.h>
#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAssetManager.h>
#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAssetWindow.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltRagdollProfileAssetDocumentManager, 1, plRTTIDefaultAllocator<plJoltRagdollProfileAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plJoltRagdollProfileAssetDocumentManager::plJoltRagdollProfileAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Jolt_RagdollProfile";
  m_DocTypeDesc.m_sFileExtension = "plJoltRagdollProfileAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/Jolt_RagdollProfile.svg";
  m_DocTypeDesc.m_sAssetCategory = "Physics";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plJoltRagdollProfileAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Jolt_RagdollProfile");

  m_DocTypeDesc.m_sResourceFileExtension = "plJoltRagdollProfile";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plJoltRagdollProfileAssetDocumentManager::~plJoltRagdollProfileAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plJoltRagdollProfileAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  if (e.m_Type == plDocumentManager::Event::Type::DocumentWindowRequested && e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plJoltRagdollProfileAssetDocument>())
  {
    new plJoltRagdollProfileAssetDocumentWindow(e.m_pDocument); // NOLINT: not a memory leak
  }
}

void plJoltRagdollProfileAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plJoltRagdollProfileAssetDocument(sPath);
}

void plJoltRagdollProfileAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
