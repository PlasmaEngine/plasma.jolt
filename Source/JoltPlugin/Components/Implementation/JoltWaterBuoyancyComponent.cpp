#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Profiling/Profiling.h>
#include <GameEngine/Interfaces/WaterWorldModule.h>
#include <JoltPlugin/Actors/JoltDynamicActorComponent.h>
#include <JoltPlugin/Components/JoltWaterBuoyancyComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltWaterBuoyancyComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("UseShapeDerivedPontoons", m_bUseShapeDerivedPontoons)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ARRAY_MEMBER_PROPERTY("Pontoons", m_Pontoons)->AddAttributes(new plMaxArraySizeAttribute(16)),
    PL_MEMBER_PROPERTY("PontoonRadius", m_fPontoonRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.05f, 20.0f)),
    PL_MEMBER_PROPERTY("BuoyancyFactor", m_fBuoyancyFactor)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("DensityScale", m_fDensityScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("DragScale", m_fDragScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_ACCESSOR_PROPERTY_READ_ONLY("Immersion", GetImmersion)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY_READ_ONLY("WaterFlow", GetWaterFlow)->AddAttributes(new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Effects"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plJoltWaterBuoyancyComponentManager::plJoltWaterBuoyancyComponentManager(plWorld* pWorld)
  : SUPER(pWorld)
{
}

void plJoltWaterBuoyancyComponentManager::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltWaterBuoyancyComponentManager::Update, this);
  desc.m_Phase = plWorldUpdatePhase::PreAsync;
  desc.m_bOnlyUpdateWhenSimulating = true;
  desc.m_fPriority = 100.0f;
  RegisterUpdateFunction(desc);
}

void plJoltWaterBuoyancyComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("JoltWaterBuoyancy");
  PL_IGNORE_UNUSED(context);

  const plWaterWorldModuleInterface* pWaterModule = GetWorld()->GetModuleReadOnly<plWaterWorldModuleInterface>();
  plJoltWorldModule* pJoltModule = GetWorld()->GetModule<plJoltWorldModule>();
  if (pWaterModule == nullptr || pJoltModule == nullptr || pJoltModule->GetJoltSystem() == nullptr)
    return;

  for (auto it = GetComponents(0); it.IsValid(); it.Next())
  {
    if (it->IsActiveAndInitialized())
      it->ApplyBuoyancy(*pJoltModule->GetJoltSystem(), *pWaterModule, GetWorld()->GetClock().GetTimeDiff());
  }
}

plJoltWaterBuoyancyComponent::plJoltWaterBuoyancyComponent() = default;

void plJoltWaterBuoyancyComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& stream = inout_stream.GetStream();
  stream << m_bUseShapeDerivedPontoons;
  stream.WriteArray(m_Pontoons).AssertSuccess();
  stream << m_fPontoonRadius;
  stream << m_fBuoyancyFactor;
  stream << m_fDensityScale;
  stream << m_fDragScale;
}

void plJoltWaterBuoyancyComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& stream = inout_stream.GetStream();
  stream >> m_bUseShapeDerivedPontoons;
  stream.ReadArray(m_Pontoons).AssertSuccess();
  stream >> m_fPontoonRadius;
  stream >> m_fBuoyancyFactor;
  stream >> m_fDensityScale;
  stream >> m_fDragScale;

  m_fPontoonRadius = plMath::Max(m_fPontoonRadius, 0.05f);
  m_fBuoyancyFactor = plMath::Max(m_fBuoyancyFactor, 0.0f);
  m_fDensityScale = plMath::Max(m_fDensityScale, 0.0f);
  m_fDragScale = plMath::Max(m_fDragScale, 0.0f);
}

void plJoltWaterBuoyancyComponent::BuildSamplePoints(plHybridArray<plVec3, 8>& out_points) const
{
  out_points.Clear();

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  if (!m_Pontoons.IsEmpty())
  {
    const plUInt32 uiCount = plMath::Min<plUInt32>(m_Pontoons.GetCount(), 16);
    for (plUInt32 i = 0; i < uiCount; ++i)
      out_points.PushBack(ownerTransform.TransformPosition(m_Pontoons[i]));
    return;
  }

  if (m_bUseShapeDerivedPontoons)
  {
    const plBoundingBox box = GetOwner()->GetGlobalBounds().GetBox();
    if (box.IsValid())
    {
      const plVec3 center = box.GetCenter();
      const plVec3 halfExtents = box.GetHalfExtents();
      const float z = center.z - halfExtents.z * 0.65f;
      out_points.PushBack(plVec3(center.x - halfExtents.x * 0.75f, center.y - halfExtents.y * 0.75f, z));
      out_points.PushBack(plVec3(center.x + halfExtents.x * 0.75f, center.y - halfExtents.y * 0.75f, z));
      out_points.PushBack(plVec3(center.x + halfExtents.x * 0.75f, center.y + halfExtents.y * 0.75f, z));
      out_points.PushBack(plVec3(center.x - halfExtents.x * 0.75f, center.y + halfExtents.y * 0.75f, z));
      out_points.PushBack(plVec3(center.x, center.y, center.z));
      return;
    }
  }

  out_points.PushBack(GetOwner()->GetGlobalPosition());
}

void plJoltWaterBuoyancyComponent::ApplyBuoyancy(JPH::PhysicsSystem& joltSystem, const plWaterWorldModuleInterface& waterModule, plTime deltaTime)
{
  PL_IGNORE_UNUSED(deltaTime);

  m_fImmersion = 0.0f;
  m_vWaterFlow.SetZero();

  plJoltDynamicActorComponent* pActor = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType(pActor) || pActor->GetJoltBodyID() == plInvalidIndex)
    return;

  plHybridArray<plVec3, 8> samplePoints;
  BuildSamplePoints(samplePoints);
  if (samplePoints.IsEmpty())
    return;

  struct Sample
  {
    plVec3 m_vPoint;
    plWaterSurfaceResult m_Result;
    float m_fSubmergedFraction = 0.0f;
  };

  plHybridArray<Sample, 8> samples;
  samples.Reserve(samplePoints.GetCount());

  const float fRadius = plMath::Max(m_fPontoonRadius, 0.05f);
  float fImmersionSum = 0.0f;
  for (const plVec3& point : samplePoints)
  {
    plWaterSurfaceQuery query;
    query.m_vPosition = point;
    query.m_fMaxVerticalDistance = fRadius * 4.0f;

    Sample sample;
    sample.m_vPoint = point;
    if (!waterModule.TrySampleSurface(query, sample.m_Result))
      continue;
    if (sample.m_Result.m_fDepth > 0.0f && !sample.m_Result.m_bContained)
      continue;

    sample.m_fSubmergedFraction = plMath::Saturate((sample.m_Result.m_fDepth + fRadius) / (2.0f * fRadius));
    if (sample.m_fSubmergedFraction <= 0.0f)
      continue;

    fImmersionSum += sample.m_fSubmergedFraction;
    m_vWaterFlow += sample.m_Result.m_vFlowVelocity * sample.m_fSubmergedFraction;
    samples.PushBack(sample);
  }

  if (samples.IsEmpty())
    return;

  m_fImmersion = plMath::Saturate(fImmersionSum / static_cast<float>(samplePoints.GetCount()));
  m_vWaterFlow /= plMath::Max(fImmersionSum, 0.0001f);

  const JPH::BodyID bodyId(pActor->GetJoltBodyID());
  JPH::BodyLockWrite bodyLock(joltSystem.GetBodyLockInterface(), bodyId);
  if (!bodyLock.Succeeded())
    return;

  JPH::Body& body = bodyLock.GetBody();
  if (!body.IsDynamic() || !body.IsActive())
    return;

  const JPH::MotionProperties* pMotion = body.GetMotionProperties();
  const float fInverseMass = pMotion->GetInverseMass();
  const float fMass = fInverseMass > 0.0f ? 1.0f / fInverseMass : 0.0f;
  const float fPontoonVolume = (4.0f / 3.0f) * plMath::Pi<float>() * fRadius * fRadius * fRadius;
  const plVec3 vGravity = plJoltConversionUtils::ToVec3(joltSystem.GetGravity());

  float fAngularDrag = 0.0f;
  for (const Sample& sample : samples)
  {
    const float fDensity = sample.m_Result.m_fDensity * m_fDensityScale;
    const plVec3 vBuoyancy = -vGravity * (fDensity * fPontoonVolume * sample.m_fSubmergedFraction * m_fBuoyancyFactor);

    const JPH::RVec3 joltPoint = plJoltConversionUtils::ToVec3(sample.m_vPoint);
    const plVec3 vPointVelocity = plJoltConversionUtils::ToVec3(body.GetPointVelocity(joltPoint));
    const plVec3 vRelativeVelocity = vPointVelocity - sample.m_Result.m_vFlowVelocity;
    const float fLinearDrag = sample.m_Result.m_fLinearDrag * m_fDragScale;
    const plVec3 vDrag = -vRelativeVelocity * (fLinearDrag * fMass * sample.m_fSubmergedFraction);

    body.AddForce(plJoltConversionUtils::ToVec3(vBuoyancy + vDrag), joltPoint);
    fAngularDrag += sample.m_Result.m_fAngularDrag * sample.m_fSubmergedFraction;
  }

  fAngularDrag = (fAngularDrag / static_cast<float>(samples.GetCount())) * m_fDragScale;
  const plVec3 vAngularVelocity = plJoltConversionUtils::ToVec3(body.GetAngularVelocity());
  body.AddTorque(plJoltConversionUtils::ToVec3(-vAngularVelocity * (fAngularDrag * fMass)));
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltWaterBuoyancyComponent);
