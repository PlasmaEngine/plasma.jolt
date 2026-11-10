#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Utilities/AssetFileHeader.h>
#include <JoltPlugin/CharacterCloth/JoltClothInstance.h>
#include <JoltPlugin/CharacterCloth/JoltClothResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothResource, 1, plRTTIDefaultAllocator<plJoltClothResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plJoltClothResource);
// clang-format on

//////////////////////////////////////////////////////////////////////////

plJoltClothData::plJoltClothData() = default;
plJoltClothData::~plJoltClothData() = default;

const plJoltClothSolverSetup* plJoltClothData::GetSolverSetup(const plMat4& mBindToSim)
{
  PL_LOCK(m_Mutex);

  for (const auto& pSetup : m_Setups)
  {
    if (pSetup->m_mBindToSim.IsEqual(mBindToSim, 1.0e-4f))
      return pSetup.Borrow();
  }

  plUniquePtr<plJoltClothSolverSetup> pSetup = PL_DEFAULT_NEW(plJoltClothSolverSetup);
  if (pSetup->Build(m_Desc, mBindToSim).Failed())
    return nullptr;

  m_Setups.PushBack(std::move(pSetup));
  return m_Setups.PeekBack().Borrow();
}

plSharedPtr<plClothDeformation> plJoltClothData::GetDeformation(plStringView sDebugName)
{
  PL_LOCK(m_Mutex);

  if (m_pDeformation == nullptr && !m_bDeformationFailed)
  {
    m_pDeformation = plClothDeformation::Create(m_Desc.m_Bindings, m_Desc.GetParticleCount(), sDebugName);
    m_bDeformationFailed = m_pDeformation == nullptr;
  }

  return m_pDeformation;
}

plUInt64 plJoltClothData::GetHeapMemoryUsage() const
{
  // each solver setup holds roughly the particles and constraints again
  return m_Desc.GetHeapMemoryUsage() * (1 + m_Setups.GetCount());
}

//////////////////////////////////////////////////////////////////////////

plJoltClothResource::plJoltClothResource()
  : plResource(DoUpdate::OnMainThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plJoltClothResource);
}

plJoltClothResource::~plJoltClothResource() = default;

plResourceLoadDesc plJoltClothResource::UnloadData(Unload WhatToUnload)
{
  m_pData = nullptr;

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plJoltClothResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plJoltClothResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  m_pData = nullptr;

  if (Stream == nullptr)
    return res;

  // the standard file reader writes the absolute file path into the stream
  plStringBuilder sAbsFilePath;
  (*Stream) >> sAbsFilePath;

  plAssetFileHeader assetHash;
  assetHash.Read(*Stream).IgnoreResult();

  plUInt8 uiVersion = 0;
  (*Stream) >> uiVersion;

  if (uiVersion != FileVersion)
  {
    plLog::Error("Cloth file version {} is not supported, this build reads version {}. Transform the cloth asset again.", uiVersion, FileVersion);
    return res;
  }

  plSharedPtr<plJoltClothData> pData = PL_DEFAULT_NEW(plJoltClothData);
  if (pData->m_Desc.Deserialize(*Stream).Failed())
    return res;

  m_pData = pData;
  res.m_State = plResourceState::Loaded;

  return res;
}

void plJoltClothResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plJoltClothResource) + (m_pData != nullptr ? m_pData->GetHeapMemoryUsage() : 0);
  out_NewMemoryUsage.m_uiMemoryGPU = m_pData != nullptr && m_pData->m_pDeformation != nullptr ? m_pData->m_pDeformation->GetGpuMemoryUsage() : 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plJoltClothResource, plJoltClothResourceDescriptor)
{
  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  m_pData = nullptr;

  if (descriptor.m_Cloth.Validate().Failed())
    return res;

  plSharedPtr<plJoltClothData> pData = PL_DEFAULT_NEW(plJoltClothData);
  pData->m_Desc = std::move(descriptor.m_Cloth);

  m_pData = pData;
  res.m_State = plResourceState::Loaded;

  return res;
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothResource);
