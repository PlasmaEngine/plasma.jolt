#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginJolt/ClothAsset/JoltClothAsset.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/Progress.h>
#include <JoltPlugin/CharacterCloth/JoltClothResource.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// The cooked file of an asset, given by GUID or path. False when the project has no such asset.
  bool FindOutputFile(plStringView sAsset, const plPlatformProfile* pAssetProfile, plStringBuilder& out_sFile)
  {
    auto asset = plAssetCurator::GetSingleton()->FindSubAsset(sAsset);
    if (!asset.isValid())
      return false;

    out_sFile = asset->m_pAssetInfo->GetManager()->GetAbsoluteOutputFileName(asset->m_pAssetInfo->m_pDocumentTypeDescriptor, asset->m_pAssetInfo->m_Path, "", pAssetProfile);
    return true;
  }

  /// The skeleton asset an animated mesh asset is skinned to, or an empty string.
  plString FindSkeletonOfMesh(plStringView sMesh)
  {
    plHybridArray<plString, 8> candidates;

    {
      auto mesh = plAssetCurator::GetSingleton()->FindSubAsset(sMesh);
      if (!mesh.isValid() || mesh->m_pAssetInfo->m_Info == nullptr)
        return plString();

      for (const plString& sDependency : mesh->m_pAssetInfo->m_Info->m_PackageDependencies)
      {
        candidates.PushBack(sDependency);
      }
    }

    for (const plString& sCandidate : candidates)
    {
      auto asset = plAssetCurator::GetSingleton()->FindSubAsset(sCandidate);

      if (asset.isValid() && asset->m_pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName == "Skeleton")
        return sCandidate;
    }

    return plString();
  }
} // namespace

plJoltClothAssetDocument::plJoltClothAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plJoltClothAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
  // the preview starts with its grid on
  m_uiCommonAssetStateFlags |= plCommonAssetUiState::Grid;
}

void plJoltClothAssetDocument::SetCommonAssetUiState(plCommonAssetUiState::Enum state, double value)
{
  if (state == plCommonAssetUiState::SimulationSpeed)
    m_fSimulationSpeed = value;

  // handles the on/off states and broadcasts the event
  SUPER::SetCommonAssetUiState(state, value);
}

double plJoltClothAssetDocument::GetCommonAssetUiState(plCommonAssetUiState::Enum state) const
{
  if (state == plCommonAssetUiState::SimulationSpeed)
    return m_fSimulationSpeed;

  return SUPER::GetCommonAssetUiState(state);
}

plStatus plJoltClothAssetDocument::LoadSource(const plPlatformProfile* pAssetProfile)
{
  const plJoltClothAssetProperties* pProp = GetProperties();

  if (pProp->m_sMesh.IsEmpty())
    return plStatus("The cloth has no mesh. Set Mesh to the animated mesh that draws the garments.");

  if (pAssetProfile == nullptr)
    pAssetProfile = plAssetCurator::GetSingleton()->GetActiveAssetProfile();

  plStringBuilder sMeshFile;
  if (!FindOutputFile(pProp->m_sMesh, pAssetProfile, sMeshFile))
    return plStatus(plFmt("The mesh '{}' is not an asset of this project.", pProp->m_sMesh));

  plFileStats meshStats;
  if (plOSFile::GetFileStats(sMeshFile, meshStats).Failed())
    return plStatus(plFmt("The mesh has not been transformed yet: '{}' does not exist. Transform the mesh, then the cloth.", sMeshFile));

  if (m_pSource == nullptr || m_pSource->m_sMeshFile != sMeshFile || !m_pSource->m_MeshTime.Compare(meshStats.m_LastModificationTime, plTimestamp::CompareMode::Identical))
  {
    m_pSource = PL_DEFAULT_NEW(Source);

    if (m_pSource->m_Mesh.Load(sMeshFile).Failed())
    {
      m_pSource.Clear();
      return plStatus(plFmt("The transformed mesh '{}' could not be read. Transform the mesh again.", sMeshFile));
    }

    m_pSource->m_sMeshFile = sMeshFile;
    m_pSource->m_MeshTime = meshStats.m_LastModificationTime;
  }

  // The skeleton only supplies collider shapes: a cloth without one still cooks.
  plStringBuilder sSkeletonFile;
  plFileStats skeletonStats;

  const plSkeletonResourceHandle& hSkeleton = m_pSource->m_Mesh.m_hDefaultSkeleton;

  if (!hSkeleton.IsValid() || !FindOutputFile(hSkeleton.GetResourceID(), pAssetProfile, sSkeletonFile) || plOSFile::GetFileStats(sSkeletonFile, skeletonStats).Failed())
  {
    m_pSource->m_bHasSkeleton = false;
    m_pSource->m_sSkeletonFile.Clear();
    return plStatus(PL_SUCCESS);
  }

  if (!m_pSource->m_bHasSkeleton || m_pSource->m_sSkeletonFile != sSkeletonFile || !m_pSource->m_SkeletonTime.Compare(skeletonStats.m_LastModificationTime, plTimestamp::CompareMode::Identical))
  {
    m_pSource->m_bHasSkeleton = false;
    m_pSource->m_Skeleton = plSkeletonResourceDescriptor();

    plFileReader file;
    plAssetFileHeader header;

    if (file.Open(sSkeletonFile).Succeeded() && header.Read(file).Succeeded() && m_pSource->m_Skeleton.Deserialize(file).Succeeded())
    {
      m_pSource->m_bHasSkeleton = true;
      m_pSource->m_sSkeletonFile = sSkeletonFile;
      m_pSource->m_SkeletonTime = skeletonStats.m_LastModificationTime;
    }
    else
    {
      plLog::Warning("The skeleton '{}' of the cloth's mesh could not be read. The cloth is built without its shapes.", sSkeletonFile);
    }
  }

  return plStatus(PL_SUCCESS);
}

plStatus plJoltClothAssetDocument::BuildCloth(const plPlatformProfile* pAssetProfile, plJoltClothDescriptor& out_desc, plJoltClothCookInfo* out_pInfo)
{
  const plStatus sourceStatus = LoadSource(pAssetProfile);
  if (sourceStatus.Failed())
    return sourceStatus;

  const plJoltClothAssetProperties* pProp = GetProperties();
  const plMeshResourceDescriptor& mesh = m_pSource->m_Mesh;

  if (mesh.GetClothSubMeshes().IsEmpty())
    return plStatus("The mesh has no garments. A garment is a mesh whose name starts with CLOTH_ in the source file. Name the garments, then transform the mesh again.");

  plJoltClothBuildOptions options;
  options.m_fWeldDistance = pProp->m_fWeldDistance;
  options.m_fDefaultMaxDistance = pProp->m_fMaxDistance;
  options.m_fDefaultBackstopDistance = pProp->m_fBackstopDistance;
  options.m_fDefaultBackstopRadius = pProp->m_fBackstopRadius;
  options.m_bUseImportedMasks = pProp->m_bUseImportedMasks;

  plJoltClothCookInfo info;
  info.m_bHasSkeleton = m_pSource->m_bHasSkeleton;

  out_desc = plJoltClothDescriptor();

  if (plJoltClothBuilder::BuildFromMesh(mesh, pProp->m_Garments.GetArrayPtr(), pProp->m_fMaxDistance, options, out_desc, &info.m_Report).Failed())
    return plStatus("No garment could be built. Garments lists names that the mesh does not have, or its garments have no triangles. The log has the details.");

  // painted maps replace the imported masks and the defaults
  plJoltClothPaintedMaps painted;
  if (GetPaintedMaps(painted).Succeeded() && !painted.IsEmpty())
    plJoltClothBuilder::ApplyPaintedMaps(painted, options.m_fBlendDistance, out_desc);

  out_desc.m_sMesh = pProp->m_sMesh;
  out_desc.m_Fabric = pProp->GetFabric();

  plJoltClothSimulationSettings& simulation = out_desc.m_Simulation;
  simulation.m_fStepRate = pProp->m_fStepRate;
  simulation.m_uiIterations = pProp->m_uiIterations;
  simulation.m_bTethers = pProp->m_bTethers;
  simulation.m_fTetherSlack = pProp->m_fTetherSlack;
  simulation.m_WorldCollision = pProp->m_WorldCollision;
  simulation.m_fGravityFactor = pProp->m_fGravityFactor;
  simulation.m_fFullRateDistance = pProp->m_fFullRateDistance;
  simulation.m_fSimulateDistance = plMath::Max(pProp->m_fSimulateDistance, pProp->m_fFullRateDistance);

  if (pProp->m_ColliderSource == plJoltClothColliderSource::SkeletonShapes && m_pSource->m_bHasSkeleton)
  {
    plJoltClothSkeletonColliderOptions colliderOptions;
    colliderOptions.m_Joints = pProp->m_SkeletonShapeJoints.GetArrayPtr();
    colliderOptions.m_fInflate = pProp->m_fColliderInflate;

    info.m_uiSkeletonColliders = plJoltClothBuilder::AddSkeletonColliders(m_pSource->m_Skeleton, colliderOptions, out_desc);
  }

  for (const plJoltClothAssetCollider& source : pProp->m_Colliders)
  {
    if (source.m_sJoint.IsEmpty())
      continue;

    plJoltClothCollider& collider = out_desc.m_Colliders.ExpandAndGetRef();
    collider.m_sJoint.Assign(source.m_sJoint);
    collider.m_Transform = plTransform(source.m_vOffset, source.m_qRotation);
    collider.m_fRadius = source.m_fRadius;
    collider.m_fLength = source.m_fLength;
    collider.m_vHalfExtents = source.m_vHalfExtents;

    switch (source.m_Shape)
    {
      case plJoltClothColliderShape::Sphere:
        collider.m_Type = plJoltClothCollider::Type::Sphere;
        break;
      case plJoltClothColliderShape::Box:
        collider.m_Type = plJoltClothCollider::Type::Box;
        break;
      default:
        collider.m_Type = plJoltClothCollider::Type::Capsule;
        break;
    }

    ++info.m_uiAssetColliders;
  }

  if (m_pSource->m_bHasSkeleton)
  {
    plJoltClothBuilder::SetJointBindPoses(m_pSource->m_Skeleton, out_desc);
    out_desc.m_RootTransform = m_pSource->m_Skeleton.m_RootTransform;
  }

  if (out_desc.Validate().Failed())
    return plStatus("The built cloth is not valid. The log says what is wrong.");

  m_LastCookInfo = info;

  if (out_pInfo != nullptr)
    *out_pInfo = info;

  return plStatus(PL_SUCCESS);
}

plResult plJoltClothAssetDocument::WriteCloth(plStreamWriter& inout_stream, const plJoltClothDescriptor& desc)
{
  inout_stream << plJoltClothResource::FileVersion;

  return desc.Serialize(inout_stream);
}

plResult plJoltClothAssetDocument::GetPaintedMaps(plJoltClothPaintedMaps& out_maps) const
{
  out_maps.Clear();

  const plDataBuffer& buffer = GetProperties()->m_PaintedMaps;
  if (buffer.IsEmpty())
    return PL_SUCCESS;

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
  return out_maps.Deserialize(reader);
}

void plJoltClothAssetDocument::SetPaintedMaps(const plJoltClothPaintedMaps& maps, plStringView sUndoName)
{
  plDataBuffer buffer;

  if (!maps.IsEmpty())
  {
    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    maps.Serialize(writer).AssertSuccess();

    buffer = plArrayPtr<const plUInt8>(storage.GetData(), storage.GetStorageSize32());
  }

  plObjectCommandAccessor accessor(GetCommandHistory());
  accessor.StartTransaction(sUndoName);
  accessor.SetValueByName(GetPropertyObject(), "PaintedMaps", buffer).AssertSuccess();
  accessor.FinishTransaction();
}

const plSkeletonResourceDescriptor* plJoltClothAssetDocument::GetCookedSkeleton() const
{
  return m_pSource != nullptr && m_pSource->m_bHasSkeleton ? &m_pSource->m_Skeleton : nullptr;
}

plTransformStatus plJoltClothAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  plProgressRange range("Transforming Asset", 2, false);

  range.BeginNextStep("Building Cloth");

  plJoltClothDescriptor desc;
  plJoltClothCookInfo info;

  const plStatus status = BuildCloth(pAssetProfile, desc, &info);
  if (status.Failed())
    return status;

  const plJoltClothBuildReport& report = info.m_Report;

  plLog::Info("Cloth: {} particles and {} triangles from {} vertices, {} of the mesh's vertices follow it, {} collider(s).", report.m_uiParticles, report.m_uiTriangles, report.m_uiSourceVertices, report.m_uiBoundVertices, desc.m_Colliders.GetCount());

  // the names Garments can list
  {
    plStringBuilder sGarments;
    for (const auto& garment : m_pSource->m_Mesh.GetClothSubMeshes())
    {
      sGarments.AppendWithSeparator(", ", garment.m_sName.GetView());
    }

    plLog::Info("Garments and proxies in the mesh: {}", sGarments);
  }

  if (report.m_uiLooseVertices > 0)
    plLog::Warning("{} vertices of the garments belong to no triangle. They are pinned.", report.m_uiLooseVertices);

  if (report.m_uiNonManifoldEdges > 0)
    plLog::Warning("{} edges of the garments are shared by more than two triangles. The cloth simulates, but may fold badly there.", report.m_uiNonManifoldEdges);

  if (report.m_uiDegenerateTriangles > 0)
    plLog::Info("{} triangles collapsed when vertices were welded and are left out. Lower Weld Distance when that removes detail.", report.m_uiDegenerateTriangles);

  if (GetProperties()->m_ColliderSource == plJoltClothColliderSource::SkeletonShapes && !info.m_bHasSkeleton)
    plLog::Warning("The mesh's skeleton is not transformed or has no shapes, so the cloth has no skeleton colliders.");

  range.BeginNextStep("Writing Result");

  if (WriteCloth(stream, desc).Failed())
    return plStatus("The cloth could not be written.");

  return plStatus(PL_SUCCESS);
}

plTransformStatus plJoltClothAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo)
{
  plStatus status = plAssetDocument::RemoteCreateThumbnail(ThumbnailInfo);
  return status;
}

void plJoltClothAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  // Skeleton colliders are cooked from the skeleton of the mesh, so a change to its shapes needs a new cook.
  if (GetProperties()->m_ColliderSource == plJoltClothColliderSource::SkeletonShapes && !GetProperties()->m_sMesh.IsEmpty())
  {
    const plString sSkeleton = FindSkeletonOfMesh(GetProperties()->m_sMesh);

    if (!sSkeleton.IsEmpty())
      pInfo->m_TransformDependencies.Insert(sSkeleton);
  }
}
