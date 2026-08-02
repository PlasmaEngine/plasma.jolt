#pragma once

#include <Core/ResourceManager/Resource.h>
#include <GameEngine/Destruction/DestructibleMeshDescriptor.h>
#include <JoltPlugin/JoltPluginDLL.h>

using plJoltDestructibleMeshResourceHandle = plTypedResourceHandle<class plJoltDestructibleMeshResource>;
using plMeshResourceHandle = plTypedResourceHandle<class plMeshResource>;

class plJoltMaterial;

namespace JPH
{
  class Shape;
}

struct PL_JOLTPLUGIN_DLL plJoltDestructibleMeshResourceDescriptor
{
  // empty, these types of resources must be loaded from file
};

/// \brief Runtime resource for a baked destructible mesh (see plDestructibleMeshAssetDocument).
///
/// Holds the chunk tree + bond graph (plDestructibleMeshDescriptor), a programmatically created
/// render plMeshResource with one sub-mesh range per chunk, and lazily instantiated Jolt convex
/// hull shapes per chunk.
class PL_JOLTPLUGIN_DLL plJoltDestructibleMeshResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltDestructibleMeshResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plJoltDestructibleMeshResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plJoltDestructibleMeshResource, plJoltDestructibleMeshResourceDescriptor);

public:
  plJoltDestructibleMeshResource();
  ~plJoltDestructibleMeshResource();

  /// \brief The baked chunk tree, bonds and hull data. The embedded render mesh descriptor is
  /// emptied after loading (its data moves into the render mesh resource).
  const plDestructibleMeshDescriptor& GetDescriptor() const { return m_Descriptor; }

  /// \brief The render mesh created from the baked geometry. Sub-mesh ranges per chunk come from the descriptor.
  const plMeshResourceHandle& GetRenderMesh() const { return m_hRenderMesh; }

  /// \brief Creates a new shape instance of a chunk's convex hull, decorated with user data, material and density.
  ///
  /// The returned shape has one reference that the caller must release (or hand to a body/compound).
  /// Returns nullptr for chunks without hull data.
  JPH::Shape* InstantiateChunkHull(plUInt32 uiChunk, plUInt64 uiUserData, const plJoltMaterial* pMaterial, float fDensity) const;

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plDestructibleMeshDescriptor m_Descriptor;
  plMeshResourceHandle m_hRenderMesh;
  mutable plDynamicArray<JPH::Shape*> m_HullInstances; ///< Lazily created base shapes, one per chunk.
};
