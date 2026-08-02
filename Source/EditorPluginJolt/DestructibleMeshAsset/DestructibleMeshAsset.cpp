#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAsset.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/Progress.h>
#include <GameEngine/Destruction/VoronoiFracture.h>
#include <JoltCooking/JoltCooking.h>
#include <ModelImporter2/ModelImporter.h>
#include <ModelImporter2/ModelPartConventions.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
#  include <Foundation/IO/CompressedStreamZstd.h>
#endif

// clang-format off
// NOTE: bump the version with EVERY change that affects the baked output, or existing assets keep their stale (cached) transform results
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plDestructibleMeshAssetDocument, 11, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

static plMat3 CalculateTransformationMatrix(const plDestructibleMeshAssetProperties* pProp)
{
  const float us = plMath::Clamp(pProp->m_fUniformScaling, 0.0001f, 10000.0f);

  auto rightDir = plMeshImportTransform::GetRightDir(pProp->m_ImportTransform, pProp->m_RightDir);
  auto upDir = plMeshImportTransform::GetUpDir(pProp->m_ImportTransform, pProp->m_UpDir);
  auto flipFwd = plMeshImportTransform::GetFlipForward(pProp->m_ImportTransform, pProp->m_bFlipForwardDir);

  const plBasisAxis::Enum forwardDir = plBasisAxis::GetOrthogonalAxis(rightDir, upDir, !flipFwd);

  return plBasisAxis::CalculateTransformationMatrix(forwardDir, rightDir, upDir, us);
}

plDestructibleMeshAssetDocument::plDestructibleMeshAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plDestructibleMeshAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
}

plTransformStatus plDestructibleMeshAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& thumbnailInfo)
{
  return plAssetDocument::RemoteCreateThumbnail(thumbnailInfo);
}

plTransformStatus plDestructibleMeshAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  plProgressRange range("Transforming Asset", 4, false);

  plDestructibleMeshAssetProperties* pProp = GetProperties();

  // import the source mesh
  range.BeginNextStep("Importing Mesh");

  plMeshResourceDescriptor meshDesc;
  plHybridArray<plBoundingBox, 4> anchorVolumes;
  {
    plStringBuilder sAbsFilename = pProp->m_sMeshFile;
    if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsFilename))
    {
      return plStatus(plFmt("Couldn't make path absolute: '{0}'", sAbsFilename));
    }

    plUniquePtr<plModelImporter2::Importer> pImporter = plModelImporter2::RequestImporterForFileType(sAbsFilename);
    if (pImporter == nullptr)
      return plStatus("No known importer for this file type.");

    plModelImporter2::ImportOptions opt;
    opt.m_sSourceFile = sAbsFilename;
    opt.m_pMeshOutput = &meshDesc;
    opt.m_RootTransform = CalculateTransformationMatrix(pProp);

    // ANC_ helper meshes mark anchor regions - they are never part of the fractured geometry
    opt.m_MeshExcludeTags.PushBack(plModelImporter2::PartConventions::GetRolePrefix(plModelImporter2::PartRole::Anchor));

    // compound-breakable assemblies: restrict this asset to one tagged part of the file
    if (!pProp->m_sPartFilter.IsEmpty())
    {
      opt.m_MeshIncludeTags.PushBack(pProp->m_sPartFilter);
    }

    if (pImporter->Import(opt).Failed())
      return plStatus("Model importer was unable to read this asset.");

    // collect ANC_ node bounds (recorded for all nodes, even excluded ones) as anchor volumes
    for (const auto& node : pImporter->m_OutputNodes)
    {
      if (plModelImporter2::PartConventions::ClassifyName(node.m_sName) == plModelImporter2::PartRole::Anchor && node.m_MeshBounds.IsValid())
      {
        anchorVolumes.PushBack(node.m_MeshBounds);
      }
    }
  }

  // sync the material slots with the imported materials, then remap the mesh to real material assets.
  // the importer only writes source material NAMES (or empty strings) into the descriptor, which are
  // not loadable resources - unassigned slots fall back to the default material.
  {
    plUInt32 uiNumMaterials = meshDesc.GetMaterials().GetCount();
    for (const auto& subMesh : meshDesc.GetSubMeshes())
    {
      uiNumMaterials = plMath::Max(uiNumMaterials, subMesh.m_uiMaterialIndex + 1);
    }

    pProp->m_Slots.SetCount(uiNumMaterials);

    for (plUInt32 i = 0; i < uiNumMaterials; ++i)
    {
      plStringView sImportedName;
      if (i < meshDesc.GetMaterials().GetCount())
      {
        sImportedName = meshDesc.GetMaterials()[i].m_sPath;
      }

      if (sImportedName.IsEmpty())
      {
        plStringBuilder sLabel;
        sLabel.SetFormat("Material {}", i);
        pProp->m_Slots[i].m_sLabel = sLabel;
      }
      else
      {
        pProp->m_Slots[i].m_sLabel = sImportedName;
      }
    }

    ApplyNativePropertyChangesToObjectManager();
    pProp = GetProperties(); // the object model may have been reallocated

    const char* szDefaultMaterial = plMaterialResource::GetDefaultMaterialFileName(plMaterialResource::DefaultMaterialType::Lit);

    for (plUInt32 i = 0; i < uiNumMaterials; ++i)
    {
      meshDesc.SetMaterial(i, pProp->m_Slots[i].m_sResource.IsEmpty() ? plStringView(szDefaultMaterial) : pProp->m_Slots[i].m_sResource.GetView());
    }
  }

  // fracture
  range.BeginNextStep("Fracturing");

  plVoronoiFracture3D fracture;

  if (fracture.SetSourceMesh(meshDesc, pProp->m_bSealOpenSurfaces).Failed())
    return plStatus("Source mesh is unsuitable for fracturing. Enable SealOpenSurfaces for repairable boundary loops; every remaining part must enclose a non-zero volume. See the log for details.");

  {
    plVoronoiFracture3D::Params params;
    params.m_uiChunkCount = pProp->m_uiChunkCount;
    params.m_uiRandomSeed = pProp->m_uiRandomSeed;
    params.m_FracturePattern = pProp->m_FracturePattern;
    params.m_fInteriorUVScale = pProp->m_fInteriorUVScale;
    params.m_fBondAreaMin = pProp->m_fBondAreaMin;
    params.m_AnchorMode = pProp->m_AnchorMode;
    params.m_AnchorVolumes = anchorVolumes;

    for (plUInt16 uiSubCount : pProp->m_SubdivisionLevels)
    {
      params.m_SubdivisionLevels.PushBack(uiSubCount);
    }

    if (fracture.Fracture(params).Failed())
      return plStatus("Voronoi fracturing failed. Is the source mesh closed and large enough for the chunk count?");
  }

  // every chunk must be a closed mesh - holes here become visible gaps in the fracture interior
  {
    const plUInt32 uiDefectiveCells = fracture.ValidateCellsClosed();

    if (uiDefectiveCells > 0)
      return plStatus(plFmt("{0} fractured chunk(s) are not closed meshes - see the log for details.", uiDefectiveCells));
  }

  plDestructibleMeshDescriptor desc;

  const plStringView sInteriorMaterial = pProp->m_sInteriorMaterial.IsEmpty()
                                           ? plStringView(plMaterialResource::GetDefaultMaterialFileName(plMaterialResource::DefaultMaterialType::Lit))
                                           : pProp->m_sInteriorMaterial.GetView();

  // per-slot toughness scales the baked chunk toughness (area-weighted over each chunk's surface materials)
  plHybridArray<float, 8> materialToughness;
  for (const auto& slot : pProp->m_Slots)
  {
    materialToughness.PushBack(plMath::Clamp(slot.m_fToughness, 0.05f, 100.0f));
  }

  if (fracture.BuildDescriptor(desc, sInteriorMaterial, materialToughness).Failed())
    return plStatus("Failed to build the destructible mesh data.");

  desc.m_fSplitDamageThreshold = pProp->m_fSplitDamageThreshold;
  desc.m_uiSupportDepth = 1; // the runtime support graph starts at depth 1; deeper levels are exposed through chunk splitting

  // cook one convex hull per chunk (chunk 0 = whole source mesh)
  range.BeginNextStep("Cooking Collision Hulls");

  desc.m_CollisionHulls.Reserve(desc.m_Chunks.GetCount());

  for (plUInt32 uiChunk = 0; uiChunk < desc.m_Chunks.GetCount(); ++uiChunk)
  {
    const plUInt32 uiCell = (uiChunk == 0) ? plInvalidIndex : uiChunk - 1;

    plJoltCookingMesh cellMesh;
    fracture.GetCellGeometry(uiCell, cellMesh.m_Vertices, cellMesh.m_VerticesInPolygon, cellMesh.m_PolygonIndices);
    cellMesh.m_PolygonSurfaceID.SetCount(cellMesh.m_VerticesInPolygon.GetCount(), 0);

    plJoltCookingMesh hullMesh;
    if (plJoltCooking::ComputeConvexHull(cellMesh, hullMesh).Failed())
    {
      // the geometric hull simplifier rejects very thin cells (its epsilons work in normalized
      // space); Jolt's own cooking computes a hull from any point cloud, so feed it the raw cell
      // geometry instead of failing the whole bake
      plLog::Warning("Convex hull simplification failed for chunk {0} (very thin cell?) - cooking the raw cell geometry instead.", uiChunk);
      hullMesh = cellMesh;
    }

    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);

    if (plJoltCooking::CookConvexMesh(hullMesh, writer).Failed())
      return plStatus(plFmt("Convex hull cooking failed for chunk {0}.", uiChunk));

    plDataBuffer& hullData = desc.m_CollisionHulls.ExpandAndGetRef();
    hullData.SetCountUninitialized(storage.GetStorageSize32());
    plMemoryUtils::Copy(hullData.GetData(), storage.GetData(), storage.GetStorageSize32());
  }

  // update statistics shown in the property grid
  {
    pProp->m_uiGeneratedChunks = desc.m_Chunks.GetCount();
    pProp->m_uiGeneratedBonds = desc.m_Bonds.GetCount();
    ApplyNativePropertyChangesToObjectManager();
  }

  if (desc.Validate().Failed())
    return plStatus("Baked destructible mesh data failed validation.");

  // write the resource
  range.BeginNextStep("Writing Result");

  const plUInt8 uiVersion = 1; // read in plJoltDestructibleMeshResource::UpdateContent()
  stream << uiVersion;

  plUInt8 uiCompressionMode = 0;

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
  uiCompressionMode = 1;
  plCompressedStreamWriterZstd compressor(&stream, 0, plCompressedStreamWriterZstd::Compression::Average);
  plStreamWriter& dataStream = compressor;
#else
  plStreamWriter& dataStream = stream;
#endif

  stream << uiCompressionMode;

  if (desc.Save(dataStream).Failed())
    return plStatus("Failed to write the destructible mesh data.");

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
  PL_SUCCEED_OR_RETURN(compressor.FinishCompressedStream());

  plLog::Dev("Compressed destructible mesh data from {0} KB to {1} KB", plArgF((float)compressor.GetUncompressedSize() / 1024.0f, 1), plArgF((float)compressor.GetCompressedSize() / 1024.0f, 1));
#endif

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plDestructibleMeshAssetDocumentGenerator, 1, plRTTIDefaultAllocator<plDestructibleMeshAssetDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plDestructibleMeshAssetDocumentGenerator::plDestructibleMeshAssetDocumentGenerator()
{
  AddSupportedFileType("obj");
  AddSupportedFileType("fbx");
  AddSupportedFileType("gltf");
  AddSupportedFileType("glb");
}

plDestructibleMeshAssetDocumentGenerator::~plDestructibleMeshAssetDocumentGenerator() = default;

void plDestructibleMeshAssetDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
  info.m_Priority = plAssetDocGeneratorPriority::LowPriority;
  info.m_sName = "DestructibleMesh";
  info.m_sIcon = ":/AssetIcons/DestructibleMesh.svg";
}

plStatus plDestructibleMeshAssetDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  plStringBuilder sOutFile = sInputFileAbs;
  sOutFile.ChangeFileExtension(GetDocumentExtension());

  if (plOSFile::ExistsFile(sOutFile))
  {
    plLog::Info("Skipping destructible mesh import, file has been imported before: '{}'", sOutFile);
    return plStatus(PL_SUCCESS);
  }

  plStringBuilder sInputFileRel = sInputFileAbs;
  plQtEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sInputFileRel);

  plDocument* pDoc = plQtEditorApp::GetSingleton()->CreateDocument(sOutFile, plDocumentFlags::None);
  if (pDoc == nullptr)
    return plStatus("Could not create target destructible mesh document.");

  out_generatedDocuments.PushBack(pDoc);

  plDestructibleMeshAssetDocument* pAssetDoc = plDynamicCast<plDestructibleMeshAssetDocument*>(pDoc);
  if (pAssetDoc == nullptr)
    return plStatus("Target document is not a valid plDestructibleMeshAssetDocument.");

  auto& accessor = pAssetDoc->GetPropertyObject()->GetTypeAccessor();
  if (!accessor.SetValue("MeshFile", sInputFileRel.GetView()))
    return plStatus("Could not assign the source mesh to the destructible mesh document.");

  plLog::Success("Imported destructible mesh: '{}'", sOutFile);
  return plStatus(PL_SUCCESS);
}
