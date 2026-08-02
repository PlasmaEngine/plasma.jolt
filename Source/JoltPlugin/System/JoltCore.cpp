#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Factory.h>
#include <Core/Physics/SurfaceResource.h>
#include <Foundation/Configuration/CVar.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>
#include <JoltPlugin/Declarations.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/Shapes/Implementation/JoltCustomShapeInfo.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltDebugRenderer.h>
#include <JoltPlugin/System/JoltJobSystem.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <stdarg.h>

#ifdef JPH_DEBUG_RENDERER
std::unique_ptr<plJoltDebugRenderer> plJoltCore::s_pDebugRenderer;
#endif

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltSteppingMode, 1)
  PL_ENUM_CONSTANTS(plJoltSteppingMode::Variable, plJoltSteppingMode::Fixed, plJoltSteppingMode::SemiFixed)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_BITFLAGS(plOnJoltContact, 1)
  // PL_BITFLAGS_CONSTANT(plOnJoltContact::SendContactMsg), // do not expose in the UI
  PL_BITFLAGS_CONSTANT(plOnJoltContact::ImpactReactions),
  PL_BITFLAGS_CONSTANT(plOnJoltContact::SlideReactions),
  PL_BITFLAGS_CONSTANT(plOnJoltContact::RollXReactions),
  PL_BITFLAGS_CONSTANT(plOnJoltContact::RollYReactions),
  PL_BITFLAGS_CONSTANT(plOnJoltContact::RollZReactions),
PL_END_STATIC_REFLECTED_BITFLAGS;
// clang-format on

plJoltMaterial* plJoltCore::s_pDefaultMaterial = nullptr;
plUniquePtr<JPH::JobSystem> plJoltCore::s_pJobSystemPL;
std::unique_ptr<JPH::JobSystem> plJoltCore::s_pJobSystemJolt;
plUniquePtr<plProxyAllocator> plJoltCore::s_pAllocator;
plUniquePtr<plProxyAllocator> plJoltCore::s_pAllocatorAligned;
plCollisionFilterConfig plJoltCore::s_CollisionFilterConfig;
plWeightCategoryConfig plJoltCore::s_WeightCategoryConfig;
plImpulseTypeConfig plJoltCore::s_ImpulseTypeConfig;

plJoltMaterial::plJoltMaterial() = default;
plJoltMaterial::~plJoltMaterial() = default;

static void JoltTraceFunc(const char* szText, ...)
{
  plStringBuilder tmp;

  va_list args;
  va_start(args, szText);
  tmp.SetPrintfArgs(szText, args);
  va_end(args);

  plLog::Dev("Jolt: {}", tmp);
}

#ifdef JPH_ENABLE_ASSERTS

static bool JoltAssertFailed(const char* szInExpression, const char* szInMessage, const char* szInFile, uint32_t inLine)
{
  return plFailedCheck(szInFile, inLine, "Jolt", szInExpression, szInMessage);
};

#endif // JPH_ENABLE_ASSERTS

plCVarBool cvar_JoltUsePlTaskSystem("Jolt.UsePlasmaTaskSystem", true, plCVarFlags::Save, "Use the Plasma TaskSystem for Jolt updates");

JPH::JobSystem* plJoltCore::GetJoltJobSystem()
{
  if (cvar_JoltUsePlTaskSystem)
  {
    if (s_pJobSystemPL == nullptr)
    {
      s_pJobSystemPL = PL_NEW(plFoundation::GetAlignedAllocator(), plJoltJobSystem, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    }

    return s_pJobSystemPL.Borrow();
  }
  else
  {
    if (s_pJobSystemJolt == nullptr)
    {
      s_pJobSystemJolt = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);
    }

    return s_pJobSystemJolt.get();
  }
}

void plJoltCore::DebugDraw(plWorld* pWorld)
{
#ifdef JPH_DEBUG_RENDERER
  if (s_pDebugRenderer == nullptr)
    return;

  plDebugRenderer::DrawSolidTriangles(pWorld, s_pDebugRenderer->m_Triangles, plColor::White);
  plDebugRenderer::DrawLines(pWorld, s_pDebugRenderer->m_Lines, plColor::White);

  s_pDebugRenderer->m_Triangles.Clear();
  s_pDebugRenderer->m_Lines.Clear();
#endif
}

void* plJoltCore::JoltMalloc(size_t inSize)
{
  return plJoltCore::s_pAllocator->Allocate(inSize, 8);
}

void plJoltCore::JoltFree(void* inBlock)
{
  if (inBlock)
  {
    plJoltCore::s_pAllocator->Deallocate(inBlock);
  }
}

void* plJoltCore::JoltReallocate(void* inBlock, size_t inOldSize, size_t inNewSize)
{
  if (inBlock == nullptr)
  {
    return JoltMalloc(inNewSize);
  }
  else
  {
    return plJoltCore::s_pAllocator->Reallocate(inBlock, inOldSize, inNewSize, 8);
  }
}

void* plJoltCore::JoltAlignedMalloc(size_t inSize, size_t inAlignment)
{
  return plJoltCore::s_pAllocatorAligned->Allocate(inSize, inAlignment);
}

void plJoltCore::JoltAlignedFree(void* inBlock)
{
  plJoltCore::s_pAllocatorAligned->Deallocate(inBlock);
}

const plCollisionFilterConfig& plJoltCore::GetCollisionFilterConfig()
{
  return s_CollisionFilterConfig;
}

const plWeightCategoryConfig& plJoltCore::GetWeightCategoryConfig()
{
  return s_WeightCategoryConfig;
}

const plImpulseTypeConfig& plJoltCore::GetImpulseTypeConfig()
{
  return s_ImpulseTypeConfig;
}

void plJoltCore::ReloadConfigs()
{
  LoadCollisionFilters();
  LoadWeightCategories();
  LoadImpulseTypes();
}

void plJoltCore::LoadCollisionFilters()
{
  PL_LOG_BLOCK("plJoltCore::LoadCollisionFilters");

  if (s_CollisionFilterConfig.Load().Failed())
  {
    plLog::Info("Collision filter config file could not be found ('{}'). Using default values.", plCollisionFilterConfig::s_sConfigFile);

    // setup some default config

    s_CollisionFilterConfig.SetGroupName(0, "Default");
    s_CollisionFilterConfig.EnableCollision(0, 0);
  }
}

void plJoltCore::LoadWeightCategories()
{
  PL_LOG_BLOCK("plJoltCore::LoadWeightCategories");

  if (s_WeightCategoryConfig.Load().Failed())
  {
    plLog::Info("Weight category config file could not be found ('{}').", plWeightCategoryConfig::s_sConfigFile);
  }
}

void plJoltCore::LoadImpulseTypes()
{
  PL_LOG_BLOCK("plJoltCore::LoadImpulseTypes");

  if (s_ImpulseTypeConfig.Load().Failed())
  {
    plLog::Info("Impulse Types config file could not be found ('{}').", plImpulseTypeConfig::s_sConfigFile);
  }
}

void plJoltCore::Startup()
{
  s_pAllocator = PL_DEFAULT_NEW(plProxyAllocator, "Jolt-Core", plFoundation::GetDefaultAllocator());
  s_pAllocatorAligned = PL_DEFAULT_NEW(plProxyAllocator, "Jolt-Core-Aligned", plFoundation::GetAlignedAllocator());

  JPH::Trace = JoltTraceFunc;
  JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed);
  JPH::Allocate = plJoltCore::JoltMalloc;
  JPH::Free = plJoltCore::JoltFree;
  JPH::Reallocate = plJoltCore::JoltReallocate;
  JPH::AlignedAllocate = plJoltCore::JoltAlignedMalloc;
  JPH::AlignedFree = plJoltCore::JoltAlignedFree;

  JPH::Factory::sInstance = new JPH::Factory();

  JPH::RegisterTypes();

  plJoltCustomShapeInfo::sRegister();

  s_pDefaultMaterial = new plJoltMaterial;
  s_pDefaultMaterial->AddRef();
  JPH::PhysicsMaterial::sDefault = s_pDefaultMaterial;

#ifdef JPH_DEBUG_RENDERER
  s_pDebugRenderer = std::make_unique<plJoltDebugRenderer>();
#endif

  plSurfaceResource::s_Events.AddEventHandler(&plJoltCore::SurfaceResourceEventHandler);
}

void plJoltCore::Shutdown()
{
#ifdef JPH_DEBUG_RENDERER
  s_pDebugRenderer = nullptr;
#endif

  JPH::PhysicsMaterial::sDefault = nullptr;

  s_pDefaultMaterial->Release();
  s_pDefaultMaterial = nullptr;

  s_pJobSystemPL = nullptr;
  s_pJobSystemJolt = nullptr;

  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  JPH::Trace = nullptr;

  s_pAllocator.Clear();
  s_pAllocatorAligned.Clear();

  plSurfaceResource::s_Events.RemoveEventHandler(&plJoltCore::SurfaceResourceEventHandler);
}

void plJoltCore::SurfaceResourceEventHandler(const plSurfaceResourceEvent& e)
{
  if (e.m_Type == plSurfaceResourceEvent::Type::Created)
  {
    const auto& desc = e.m_pSurface->GetDescriptor();

    plJoltMaterial* pJoltMat = static_cast<plJoltMaterial*>(e.m_pSurface->m_pPhysicsMaterialJolt);

    if (pJoltMat == nullptr)
    {
      pJoltMat = new plJoltMaterial;
      pJoltMat->AddRef();
      pJoltMat->m_pSurface = e.m_pSurface;
    }
    else
    {
      PL_ASSERT_DEV(pJoltMat->m_pSurface == e.m_pSurface, "Invalid surface");
    }

    pJoltMat->m_fRestitution = desc.m_fPhysicsRestitution;
    pJoltMat->m_fFriction = plMath::Lerp(desc.m_fPhysicsFrictionStatic, desc.m_fPhysicsFrictionDynamic, 0.5f);

    e.m_pSurface->m_pPhysicsMaterialJolt = pJoltMat;
  }
  else if (e.m_Type == plSurfaceResourceEvent::Type::Destroyed)
  {
    if (e.m_pSurface->m_pPhysicsMaterialJolt != nullptr)
    {
      plJoltMaterial* pMaterial = static_cast<plJoltMaterial*>(e.m_pSurface->m_pPhysicsMaterialJolt);
      pMaterial->Release();

      e.m_pSurface->m_pPhysicsMaterialJolt = nullptr;
    }
  }
}


PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_System_JoltCore);
