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

  /// \brief The scenes and prefabs that draw the mesh of a cloth asset without using the cloth.
  static void FindUsersWithoutCloth(const plUuid& clothGuid, plStringView sMeshGuid, plDynamicArray<plString>& out_documents);

  /// \brief Adds a Character Cloth component with the cloth next to every Animated Mesh component that draws the
  /// mesh, in one scene or prefab, and saves it. Returns how many it added.
  static plUInt32 AddClothComponents(plStringView sDocumentAbsPath, const plUuid& clothGuid, plStringView sMeshGuid);

  /// \brief Asks whether the scenes and prefabs that draw the cloth's mesh should get the component, and adds it.
  /// A document that was asked about once in a session is not asked about again.
  static void OfferClothComponents(const plUuid& clothGuid, plStringView sMeshGuid);

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);
  void OnAssetBrowserContextMenu(const plAssetBrowserContextMenuEvent& e);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
