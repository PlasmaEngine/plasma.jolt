#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <JoltPlugin/Resources/JoltDestructibleMeshResource.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/Shapes/Implementation/JoltCustomShapeInfo.h>
#include <RendererCore/Meshes/MeshResource.h>

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
#  include <Foundation/IO/CompressedStreamZstd.h>
#endif

namespace
{
  class plJoltDestructibleStreamIn : public JPH::StreamIn
  {
  public:
    plStreamReader* m_pStream = nullptr;
    bool m_bEOF = false;

    virtual void ReadBytes(void* pData, size_t uiInNumBytes) override
    {
      if (m_pStream->ReadBytes(pData, uiInNumBytes) < uiInNumBytes)
        m_bEOF = true;
    }

    virtual bool IsEOF() const override { return m_bEOF; }
    virtual bool IsFailed() const override { return false; }
  };
} // namespace

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltDestructibleMeshResource, 1, plRTTIDefaultAllocator<plJoltDestructibleMeshResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plJoltDestructibleMeshResource);
// clang-format on

plJoltDestructibleMeshResource::plJoltDestructibleMeshResource()
  : plResource(DoUpdate::OnMainThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plJoltDestructibleMeshResource);
}

plJoltDestructibleMeshResource::~plJoltDestructibleMeshResource() = default;

plResourceLoadDesc plJoltDestructibleMeshResource::UnloadData(Unload WhatToUnload)
{
  for (auto pShape : m_HullInstances)
  {
    if (pShape != nullptr)
    {
      pShape->Release();
    }
  }

  m_HullInstances.Clear();
  m_Descriptor.Clear();
  m_hRenderMesh.Invalidate();

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plJoltDestructibleMeshResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plJoltDestructibleMeshResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plJoltDestructibleMeshResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (Stream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  // the standard file reader writes the absolute file path into the stream
  plStringBuilder sAbsFilePath;
  (*Stream) >> sAbsFilePath;

  plAssetFileHeader AssetHash;
  AssetHash.Read(*Stream).IgnoreResult();

  // version specified in plDestructibleMeshAssetDocument::InternalTransformAsset()
  plUInt8 uiVersion = 0;
  plUInt8 uiCompressionMode = 0;

  *Stream >> uiVersion;
  *Stream >> uiCompressionMode;

  if (uiVersion != 1)
  {
    plLog::Error("Unsupported destructible mesh version {} in '{}'", uiVersion, GetResourceIdOrDescription());
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  plStreamReader* pDecompressor = Stream;

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
  plCompressedStreamReaderZstd decompressorZstd;
#endif

  switch (uiCompressionMode)
  {
    case 0:
      break;

    case 1:
#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
      decompressorZstd.SetInputStream(Stream);
      pDecompressor = &decompressorZstd;
      break;
#else
      plLog::Error("Destructible mesh is compressed with zstandard, but support for this compressor is not compiled in.");
      res.m_State = plResourceState::LoadedResourceMissing;
      return res;
#endif

    default:
      plLog::Error("Destructible mesh is compressed with an unknown algorithm.");
      res.m_State = plResourceState::LoadedResourceMissing;
      return res;
  }

  if (m_Descriptor.Load(*pDecompressor).Failed())
  {
    plLog::Error("Failed to load destructible mesh data from '{}'", GetResourceIdOrDescription());
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  m_HullInstances.SetCount(m_Descriptor.m_CollisionHulls.GetCount(), nullptr);

  // create the render mesh from the embedded descriptor
  {
    plStringBuilder sRenderMeshId;
    sRenderMeshId.SetFormat("{}-RenderMesh-{}", GetResourceID(), GetCurrentResourceChangeCounter());

    m_hRenderMesh = plResourceManager::GetExistingResource<plMeshResource>(sRenderMeshId);

    if (!m_hRenderMesh.IsValid())
    {
      plMeshResourceDescriptor renderDesc = std::move(m_Descriptor.m_RenderMesh);
      m_hRenderMesh = plResourceManager::CreateResource<plMeshResource>(sRenderMeshId, std::move(renderDesc), "Destructible mesh render geometry");
    }

    m_Descriptor.m_RenderMesh.Clear();
  }

  res.m_State = plResourceState::Loaded;
  return res;
}

void plJoltDestructibleMeshResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plJoltDestructibleMeshResource);
  out_NewMemoryUsage.m_uiMemoryGPU = 0;

  out_NewMemoryUsage.m_uiMemoryCPU += m_Descriptor.m_Chunks.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Descriptor.m_Bonds.GetHeapMemoryUsage();

  for (const auto& hull : m_Descriptor.m_CollisionHulls)
  {
    out_NewMemoryUsage.m_uiMemoryCPU += hull.GetHeapMemoryUsage();
  }
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plJoltDestructibleMeshResource, plJoltDestructibleMeshResourceDescriptor)
{
  // creates just an empty resource

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

JPH::Shape* plJoltDestructibleMeshResource::InstantiateChunkHull(plUInt32 uiChunk, plUInt64 uiUserData, const plJoltMaterial* pMaterial, float fDensity) const
{
  if (uiChunk >= m_Descriptor.m_Chunks.GetCount())
    return nullptr;

  const plUInt32 uiHullIdx = m_Descriptor.m_Chunks[uiChunk].m_uiCollisionHullIndex;

  if (uiHullIdx == plInvalidIndex || uiHullIdx >= m_HullInstances.GetCount())
    return nullptr;

  if (m_HullInstances[uiHullIdx] == nullptr)
  {
    plRawMemoryStreamReader memReader(m_Descriptor.m_CollisionHulls[uiHullIdx]);

    // plJoltCooking::CookConvexMesh writes [size][shape binary state][num vertices][num triangles];
    // sRestoreFromBinaryState must only see the shape state itself
    plUInt32 uiShapeDataSize = 0;
    memReader >> uiShapeDataSize;

    if (uiShapeDataSize == 0 || uiShapeDataSize > m_Descriptor.m_CollisionHulls[uiHullIdx].GetCount() - sizeof(plUInt32))
    {
      plLog::Error("Destructible chunk hull {} of '{}' has invalid data.", uiChunk, GetResourceIdOrDescription());
      return nullptr;
    }

    plJoltDestructibleStreamIn jStream;
    jStream.m_pStream = &memReader;

    auto shapeRes = JPH::Shape::sRestoreFromBinaryState(jStream);

    if (shapeRes.HasError() || jStream.IsFailed())
    {
      plLog::Error("Failed to instantiate destructible chunk hull {} of '{}'", uiChunk, GetResourceIdOrDescription());
      return nullptr;
    }

    JPH::ConvexShape* pConvexShape = static_cast<JPH::ConvexShape*>(shapeRes.Get().GetPtr());
    pConvexShape->SetDensity(1.0f); // the decoration shape multiplies the density, so keep the base at 1

    m_HullInstances[uiHullIdx] = shapeRes.Get();
    m_HullInstances[uiHullIdx]->AddRef();
  }

  plJoltCustomShapeInfo* pShapeDeco = new plJoltCustomShapeInfo(m_HullInstances[uiHullIdx]);
  pShapeDeco->SetUserData(uiUserData);
  pShapeDeco->m_fDensity = fDensity;

  if (pMaterial && pMaterial->m_pSurface != nullptr)
  {
    pShapeDeco->m_CustomMaterials.SetCount(1);
    pShapeDeco->m_CustomMaterials[0] = pMaterial;
  }

  pShapeDeco->AddRef();
  return pShapeDeco;
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Resources_JoltDestructibleMeshResource);
