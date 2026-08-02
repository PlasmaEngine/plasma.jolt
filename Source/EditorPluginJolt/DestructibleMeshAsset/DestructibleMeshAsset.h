#pragma once

#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetObjects.h>

/// \brief Asset document that pre-fractures a source mesh into a destructible chunk hierarchy.
///
/// The transform step imports the source mesh, runs the in-engine Voronoi fracture
/// (plVoronoiFracture3D), cooks a Jolt convex hull per chunk and writes the combined
/// plDestructibleMeshDescriptor as the runtime resource.
class plDestructibleMeshAssetDocument : public plSimpleAssetDocument<plDestructibleMeshAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plDestructibleMeshAssetDocument, plSimpleAssetDocument<plDestructibleMeshAssetProperties>);

public:
  plDestructibleMeshAssetDocument(plStringView sDocumentPath);

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& thumbnailInfo) override;
};

//////////////////////////////////////////////////////////////////////////

/// \brief Offers supported model files as directly importable Destructible Mesh assets.
class plDestructibleMeshAssetDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plDestructibleMeshAssetDocumentGenerator, plAssetDocumentGenerator);

public:
  plDestructibleMeshAssetDocumentGenerator();
  ~plDestructibleMeshAssetDocumentGenerator();

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plDestructibleMeshAsset"; }
  virtual plStringView GetGeneratorGroup() const override { return "Meshes"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};
