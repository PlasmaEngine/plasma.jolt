#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAssetObjects.h>
#include <Foundation/Time/Timestamp.h>
#include <JoltPlugin/CharacterCloth/JoltClothBuilder.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

/// \brief What the cloth window shows about the last cook.
struct plJoltClothCookInfo
{
  plJoltClothBuildReport m_Report;
  plUInt32 m_uiSkeletonColliders = 0;
  plUInt32 m_uiAssetColliders = 0;
  bool m_bHasSkeleton = false;
};

/// \brief The cloth of a character: which garments of an animated mesh simulate, their maps, fabric and colliders.
class plJoltClothAssetDocument : public plSimpleAssetDocument<plJoltClothAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothAssetDocument, plSimpleAssetDocument<plJoltClothAssetProperties>);

public:
  plJoltClothAssetDocument(plStringView sDocumentPath);

  /// \brief Cooks the cloth from the current properties. The transform and the live preview both use it.
  plStatus BuildCloth(const plPlatformProfile* pAssetProfile, plJoltClothDescriptor& out_desc, plJoltClothCookInfo* out_pInfo = nullptr);

  /// \brief Writes what plJoltClothResource reads after the asset header.
  static plResult WriteCloth(plStreamWriter& inout_stream, const plJoltClothDescriptor& desc);

  /// \brief The maps painted so far. Empty while nothing was painted.
  plResult GetPaintedMaps(plJoltClothPaintedMaps& out_maps) const;

  /// \brief Stores painted maps as one undoable step. Empty maps go back to the imported and default values.
  void SetPaintedMaps(const plJoltClothPaintedMaps& maps, plStringView sUndoName);

  /// \brief The skeleton the mesh is skinned to, when it was found at the last cook. nullptr otherwise.
  const plSkeletonResourceDescriptor* GetCookedSkeleton() const;

  const plJoltClothCookInfo& GetLastCookInfo() const { return m_LastCookInfo; }

  virtual void SetCommonAssetUiState(plCommonAssetUiState::Enum state, double value) override;
  virtual double GetCommonAssetUiState(plCommonAssetUiState::Enum state) const override;

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;

private:
  /// The cooked mesh and skeleton, kept between cooks: the live preview cooks on every change.
  struct Source
  {
    plString m_sMeshFile;
    plTimestamp m_MeshTime;
    plMeshResourceDescriptor m_Mesh;

    plString m_sSkeletonFile;
    plTimestamp m_SkeletonTime;
    plSkeletonResourceDescriptor m_Skeleton;
    bool m_bHasSkeleton = false;
  };

  plStatus LoadSource(const plPlatformProfile* pAssetProfile);

  plUniquePtr<Source> m_pSource;
  plJoltClothCookInfo m_LastCookInfo;
  double m_fSimulationSpeed = 1.0;
};
