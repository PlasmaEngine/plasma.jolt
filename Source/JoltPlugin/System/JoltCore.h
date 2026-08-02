#pragma once

#include <Foundation/Containers/HashTable.h>
#include <Foundation/Memory/CommonAllocators.h>
#include <Foundation/Types/UniquePtr.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <GameEngine/Physics/ImpulseType.h>
#include <GameEngine/Physics/WeightCategory.h>
#include <JoltPlugin/JoltPluginDLL.h>

class plJoltMaterial;
struct plSurfaceResourceEvent;
class plJoltDebugRenderer;
class plWorld;

namespace JPH
{
  class JobSystem;
}

class PL_JOLTPLUGIN_DLL plJoltCore
{
public:
  static JPH::JobSystem* GetJoltJobSystem();
  static const plJoltMaterial* GetDefaultMaterial() { return s_pDefaultMaterial; }

  static void DebugDraw(plWorld* pWorld);

#ifdef JPH_DEBUG_RENDERER
  static std::unique_ptr<plJoltDebugRenderer> s_pDebugRenderer;
#endif

  static const plCollisionFilterConfig& GetCollisionFilterConfig();
  static const plWeightCategoryConfig& GetWeightCategoryConfig();
  static const plImpulseTypeConfig& GetImpulseTypeConfig();

  static void ReloadConfigs();

private:
  PL_MAKE_SUBSYSTEM_STARTUP_FRIEND(Jolt, JoltPlugin);

  static void Startup();
  static void Shutdown();

  static void SurfaceResourceEventHandler(const plSurfaceResourceEvent& e);

  static void* JoltMalloc(size_t inSize);
  static void JoltFree(void* inBlock);
  static void* JoltReallocate(void* inBlock, size_t inOldSize, size_t inNewSize);
  static void* JoltAlignedMalloc(size_t inSize, size_t inAlignment);
  static void JoltAlignedFree(void* inBlock);

  static void LoadCollisionFilters();
  static void LoadWeightCategories();
  static void LoadImpulseTypes();

  static plJoltMaterial* s_pDefaultMaterial;

  static plUniquePtr<JPH::JobSystem> s_pJobSystemPL;
  static std::unique_ptr<JPH::JobSystem> s_pJobSystemJolt;

  static plUniquePtr<plProxyAllocator> s_pAllocator;
  static plUniquePtr<plProxyAllocator> s_pAllocatorAligned;

  static plCollisionFilterConfig s_CollisionFilterConfig;
  static plWeightCategoryConfig s_WeightCategoryConfig;
  static plImpulseTypeConfig s_ImpulseTypeConfig;
};
