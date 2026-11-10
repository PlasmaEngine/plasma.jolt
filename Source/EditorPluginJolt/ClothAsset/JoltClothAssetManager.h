#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <Foundation/Types/Status.h>

struct plAssetBrowserContextMenuEvent;

class plJoltClothAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothAssetDocumentManager, plAssetDocumentManager);

public:
  plJoltClothAssetDocumentManager();
  ~plJoltClothAssetDocumentManager();

  /// \brief Makes the cloth asset of an animated mesh next to it and opens it. Opens the one that is there already.
  static plStatus CreateClothForMesh(plStringView sMeshAssetAbsPath);

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);
  void OnAssetBrowserContextMenu(const plAssetBrowserContextMenuEvent& e);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
