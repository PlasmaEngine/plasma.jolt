#include "JoltTestSupport.h"

namespace JoltTest
{
  static Totals s_Totals;
  static const char* s_szGroup = "";

  void BeginGroup(const char* szName)
  {
    s_szGroup = szName;
    std::printf("[ %s ]\n", szName);
  }

  void Check(bool bValue, const char* szMessage, Category category)
  {
    ++s_Totals.m_uiChecks;

    if (bValue)
      return;

    if (category == Category::R1Gate)
    {
      ++s_Totals.m_uiOpenR1Items;
      std::printf("OPEN R1: %s: %s\n", s_szGroup, szMessage);
    }
    else
    {
      ++s_Totals.m_uiFailures;
      std::printf("FAIL: %s: %s\n", s_szGroup, szMessage);
    }
  }

  void CheckNear(float fActual, float fExpected, float fTolerance, const char* szMessage, Category category)
  {
    const bool bOk = plMath::Abs(fActual - fExpected) <= fTolerance;

    if (!bOk)
      std::printf("  %s: got %.4f, expected %.4f +- %.4f\n", szMessage, fActual, fExpected, fTolerance);

    Check(bOk, szMessage, category);
  }

  const Totals& GetTotals()
  {
    return s_Totals;
  }

  World::World(float fFrameRate)
    : m_fFrameRate(fFrameRate)
  {
    plWorldDesc desc("JoltTests");
    m_pWorld = PL_DEFAULT_NEW(plWorld, desc);

    PL_LOCK(m_pWorld->GetWriteMarker());
    m_pWorld->SetWorldSimulationEnabled(true);
    m_pWorld->GetClock().SetFixedTimeStep(plTime::MakeFromSeconds(1.0 / fFrameRate));
    m_pJolt = m_pWorld->GetOrCreateModule<plJoltWorldModule>();
  }

  World::~World()
  {
    m_pWorld.Clear();
  }

  void World::Step(plUInt32 uiFrames)
  {
    for (plUInt32 i = 0; i < uiFrames; ++i)
    {
      PL_LOCK(m_pWorld->GetWriteMarker());
      m_pWorld->Update();
    }
  }

  void World::StepSeconds(float fSeconds)
  {
    Step(static_cast<plUInt32>(plMath::Round(fSeconds * m_fFrameRate)));
  }

  plGameObject* World::CreateObject(const plVec3& vPosition, const plQuat& qRotation)
  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    plGameObjectDesc desc;
    desc.m_LocalPosition = vPosition;
    desc.m_LocalRotation = qRotation;
    desc.m_bDynamic = true;

    plGameObject* pObject = nullptr;
    m_pWorld->CreateObject(desc, pObject);
    return pObject;
  }

  plVec3 World::GetPosition(const plGameObjectHandle& hObject)
  {
    PL_LOCK(m_pWorld->GetReadMarker());

    plGameObject* pObject = nullptr;
    if (!m_pWorld->TryGetObject(hObject, pObject))
      return plVec3(plMath::NaN<float>());

    return pObject->GetGlobalPosition();
  }

  plGameObject* World::AddStaticBox(const plVec3& vPosition, const plVec3& vHalfExtents, plUInt8 uiLayer, const plQuat& qRotation)
  {
    plGameObject* pObject = CreateObject(vPosition, qRotation);

    PL_LOCK(m_pWorld->GetWriteMarker());

    plJoltStaticActorComponent* pActor = nullptr;
    plJoltStaticActorComponent::CreateComponent(pObject, pActor);
    pActor->m_uiCollisionLayer = uiLayer;

    plJoltShapeBoxComponent* pBox = nullptr;
    plJoltShapeBoxComponent::CreateComponent(pObject, pBox);
    pBox->SetHalfExtents(vHalfExtents);
    return pObject;
  }

  plGameObject* World::AddDynamicBox(const plVec3& vPosition, const plVec3& vHalfExtents, plUInt8 uiLayer, float fGravityFactor)
  {
    plGameObject* pObject = CreateObject(vPosition);

    PL_LOCK(m_pWorld->GetWriteMarker());

    plJoltDynamicActorComponent* pActor = nullptr;
    plJoltDynamicActorComponent::CreateComponent(pObject, pActor);
    pActor->m_uiCollisionLayer = uiLayer;
    pActor->SetGravityFactor(fGravityFactor);

    plJoltShapeBoxComponent* pBox = nullptr;
    plJoltShapeBoxComponent::CreateComponent(pObject, pBox);
    pBox->SetHalfExtents(vHalfExtents);
    return pObject;
  }

  void World::SetVelocity(plGameObject* pObject, const plVec3& vVelocity)
  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    plJoltDynamicActorComponent* pActor = nullptr;
    if (!pObject->TryGetComponentOfBaseType(pActor))
      return;

    pActor->AddLinearImpulse(vVelocity * pActor->GetMass());
  }

  plGameObject* World::AddFloor(plUInt8 uiLayer)
  {
    return AddStaticBox(plVec3(0, 0, -0.5f), plVec3(100.0f, 100.0f, 0.5f), uiLayer);
  }

  plPhysicsQueryParameters AllShapes(plUInt32 uiLayer)
  {
    return plPhysicsQueryParameters(uiLayer, plPhysicsShapeType::Default);
  }

  plUInt32 CountDistinctActors(const plPhysicsCastResultArray& results)
  {
    plSet<plGameObjectHandle> actors;
    for (const plPhysicsCastResult& result : results.m_Results)
      actors.Insert(result.m_hActorObject);

    return actors.GetCount();
  }

  void Startup()
  {
    // the high level systems bring up the renderer, which needs a GPU device; Jolt starts with the core systems
    plStartup::StartupCoreSystems();
  }

  void Shutdown()
  {
    plStartup::ShutdownCoreSystems();
  }
} // namespace JoltTest
