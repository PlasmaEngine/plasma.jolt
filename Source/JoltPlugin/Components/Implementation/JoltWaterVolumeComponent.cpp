#include <JoltPlugin/JoltPluginPCH.h>

#include <JoltPlugin/Components/JoltWaterVolumeComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Profiling/Profiling.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltWaterVolumeComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(10.0f)), new plClampValueAttribute(plVec3(0.0f), plVariant())),
    PL_MEMBER_PROPERTY("LocalFlow", m_vLocalFlow),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Effects"),
    new plBoxManipulatorAttribute("Extents", 1.0f, true),
    new plBoxVisualizerAttribute("Extents", 1.0f, plColorScheme::GetCategoryColor("Physics", plColorScheme::CategoryColorUsage::ViewportIcon)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

//////////////////////////////////////////////////////////////////////////

plJoltWaterVolumeComponentManager::plJoltWaterVolumeComponentManager(plWorld* pWorld)
  : plComponentManager<plJoltWaterVolumeComponent, plBlockStorageType::FreeList>(pWorld)
{
}

plJoltWaterVolumeComponentManager::~plJoltWaterVolumeComponentManager() = default;

void plJoltWaterVolumeComponentManager::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltWaterVolumeComponentManager::UpdateWaterVolumes, this);
  desc.m_Phase = plWorldUpdatePhase::PreAsync;
  desc.m_bOnlyUpdateWhenSimulating = true;
  RegisterUpdateFunction(desc);
}

void plJoltWaterVolumeComponentManager::UpdateWaterVolumes(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("UpdateWaterVolumes");
  PL_IGNORE_UNUSED(context);

  plJoltWorldModule* pJoltModule = GetWorld()->GetModule<plJoltWorldModule>();
  if (pJoltModule == nullptr || pJoltModule->GetJoltSystem() == nullptr)
    return;

  auto pJoltSystem = pJoltModule->GetJoltSystem();

  for (auto it = GetComponents(0); it.IsValid(); it.Next())
  {
    if (it->IsActiveAndInitialized())
    {
      it->Update(*pJoltSystem, GetWorld()->GetClock().GetTimeDiff());
    }
  }
}

//////////////////////////////////////////////////////////////////////////

plJoltWaterVolumeComponent::plJoltWaterVolumeComponent() = default;
plJoltWaterVolumeComponent::~plJoltWaterVolumeComponent() = default;

void plJoltWaterVolumeComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_vExtents;
  s << m_vLocalFlow;
}

void plJoltWaterVolumeComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  /*const plUInt32 uiVersion =*/inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  auto& s = inout_stream.GetStream();

  s >> m_vExtents;
  s >> m_vLocalFlow;
}

void plJoltWaterVolumeComponent::Update(JPH::PhysicsSystem& joltSystem, plTime deltaTime)
{
  class MyCollector : public JPH::CollideShapeBodyCollector
  {
  public:
    MyCollector(JPH::PhysicsSystem& inSystem, const plVec3& inSurfacePosition, const plVec3& inSurfaceNormal, const plVec3& inFlowVelocity, float inDeltaTime)
      : m_system(inSystem)
      , mSurfacePosition(plJoltConversionUtils::ToVec3(inSurfacePosition))
      , mSurfaceNormal(plJoltConversionUtils::ToVec3(inSurfaceNormal))
      , mFlowVelocity(plJoltConversionUtils::ToVec3(inFlowVelocity))
      , mDeltaTime(inDeltaTime)
    {
    }

    virtual void AddHit(const JPH::BodyID& inBodyID) override
    {
      JPH::BodyLockWrite lock(m_system.GetBodyLockInterface(), inBodyID);
      JPH::Body& body = lock.GetBody();
      if (body.IsActive() && body.IsDynamic())
        body.ApplyBuoyancyImpulse(mSurfacePosition, mSurfaceNormal, 1.1f, 0.3f, 0.05f, mFlowVelocity, m_system.GetGravity(), mDeltaTime);
    }

  private:
    JPH::PhysicsSystem& m_system;
    JPH::RVec3 mSurfacePosition;
    JPH::Vec3 mSurfaceNormal;
    JPH::Vec3 mFlowVelocity;
    float mDeltaTime;
  };

  const plVec3 halfExtents = m_vExtents * 0.5f;
  const plVec3 globalPos = GetOwner()->GetGlobalPosition();

  JPH::Vec3 joltHalfExtents = plJoltConversionUtils::ToVec3(halfExtents);
  JPH::AABox waterBox(-joltHalfExtents, joltHalfExtents);
  waterBox.Translate(plJoltConversionUtils::ToVec3(GetOwner()->GetGlobalPosition()));

  const plVec3 globalFlow = GetOwner()->GetGlobalRotation() * plVec3(m_vLocalFlow.x, m_vLocalFlow.y, 0.0f);
  MyCollector collector(joltSystem, globalPos + plVec3(0, 0, halfExtents.z), plVec3::MakeAxisZ(), globalFlow, deltaTime.AsFloatInSeconds());
  plJoltBroadPhaseLayerFilter broadphaseFilter(plPhysicsShapeType::Dynamic);
  // TODO: collision layer

  joltSystem.GetBroadPhaseQuery().CollideAABox(waterBox, collector, broadphaseFilter);
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltWaterVolumeComponent);
