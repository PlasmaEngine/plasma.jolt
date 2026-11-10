#pragma once

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <Foundation/Types/UniquePtr.h>
#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

struct plJoltClothSolverSetup;

using plJoltClothResourceHandle = plTypedResourceHandle<class plJoltClothResource>;

/// \brief The cloth data of one version of a cloth resource, with everything that is built from it on demand.
///
/// A reload gives the resource a new object. Components keep the one they were built from until they rebuild, so
/// nothing they point into goes away under them.
class PL_JOLTPLUGIN_DLL plJoltClothData : public plRefCounted
{
  PL_DISALLOW_COPY_AND_ASSIGN(plJoltClothData);

public:
  plJoltClothData();
  ~plJoltClothData();

  const plJoltClothDescriptor& GetDescriptor() const { return m_Desc; }

  /// \brief The solver data for a character whose bind space maps to simulation space by mBindToSim.
  ///
  /// Built on first use and shared by every character with the same scale and orientation. Returns nullptr when the
  /// build fails. The pointer is valid for as long as this object.
  const plJoltClothSolverSetup* GetSolverSetup(const plMat4& mBindToSim);

  /// \brief The bindings on the GPU. Created on first use; nullptr without a render device or when the cloth is not
  /// bound to a render mesh. Call from the main thread.
  plSharedPtr<plClothDeformation> GetDeformation(plStringView sDebugName);

  plUInt64 GetHeapMemoryUsage() const;

private:
  friend class plJoltClothResource;

  plJoltClothDescriptor m_Desc;

  plMutex m_Mutex;
  plDynamicArray<plUniquePtr<plJoltClothSolverSetup>> m_Setups;
  plSharedPtr<plClothDeformation> m_pDeformation;
  bool m_bDeformationFailed = false;
};

struct PL_JOLTPLUGIN_DLL plJoltClothResourceDescriptor
{
  plJoltClothDescriptor m_Cloth;
};

/// \brief A cooked cloth asset: the sim mesh of a character's garments, its maps, fabric and colliders.
class PL_JOLTPLUGIN_DLL plJoltClothResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plJoltClothResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plJoltClothResource, plJoltClothResourceDescriptor);

public:
  /// Version of the file that the cloth asset transform writes.
  static constexpr plUInt8 FileVersion = 1;

  plJoltClothResource();
  ~plJoltClothResource();

  /// \brief The current data, or nullptr while nothing is loaded.
  plSharedPtr<plJoltClothData> GetData() const { return m_pData; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plSharedPtr<plJoltClothData> m_pData;
};
