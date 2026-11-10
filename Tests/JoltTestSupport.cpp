#include "JoltTestSupport.h"
#include <Foundation/Utilities/CommandLineOptions.h>
#include <set>
#include <string>

plCommandLineOptionBool opt_JoltStrictDefects("_JoltTest", "-joltStrictDefects", "Open defect checks fail the run when they fail.", false);

namespace JoltTest
{
  namespace
  {
    /// Prints the bug ids that still had a failing open defect check when the process ends.
    struct OpenDefectList
    {
      std::set<std::string> m_Open;

      ~OpenDefectList()
      {
        if (m_Open.empty())
          return;

        std::string sList;
        for (const std::string& sId : m_Open)
          sList += (sList.empty() ? "" : ", ") + sId;

        std::printf("Open defects: %s\n", sList.c_str());
      }
    };

    OpenDefectList s_OpenDefects;
  } // namespace

  void Check(bool bValue, const char* szMessage, std::source_location location)
  {
    plTestBool(bValue, "Test failed", location.file_name(), static_cast<plInt32>(location.line()), location.function_name(), "%s", szMessage);
  }

  void CheckNear(float fActual, float fExpected, float fTolerance, const char* szMessage, std::source_location location)
  {
    const bool bOk = plMath::Abs(fActual - fExpected) <= fTolerance;

    if (!bOk)
      plTestFramework::Output(plTestOutput::Error, "%s: got %.4f, expected %.4f +- %.4f", szMessage, fActual, fExpected, fTolerance);

    Check(bOk, szMessage, location);
  }

  void OpenDefect(const char* szBugId, bool bValue, const char* szMessage, std::source_location location)
  {
    if (bValue)
    {
      plTestFramework::Output(plTestOutput::Warning, "%s now passes, turn it into a regular check: %s", szBugId, szMessage);
      return;
    }

    if (opt_JoltStrictDefects.GetOptionValue(plCommandLineOption::LogMode::Never))
    {
      Check(false, szMessage, location);
      return;
    }

    s_OpenDefects.m_Open.insert(szBugId);
    plTestFramework::Output(plTestOutput::Warning, "OPEN %s: %s (%s:%u)", szBugId, szMessage, location.file_name(), location.line());
  }

  void OpenDefectNear(const char* szBugId, float fActual, float fExpected, float fTolerance, const char* szMessage, std::source_location location)
  {
    const bool bOk = plMath::Abs(fActual - fExpected) <= fTolerance;

    if (!bOk)
      plTestFramework::Output(plTestOutput::Warning, "  %s: got %.4f, expected %.4f +- %.4f", szMessage, fActual, fExpected, fTolerance);

    OpenDefect(szBugId, bOk, szMessage, location);
  }

  namespace
  {
    plAssertHandler s_PreviousAssertHandler = nullptr;

    /// Unloading the resources that core systems start up with, which happens whenever a sub-test ends, asks the renderer for its
    /// device. These tests have none, and the Debug configuration asserts. That one message is dropped, every other assert is passed on.
    bool AssertHandler(const char* szSourceFile, plUInt32 uiLine, const char* szFunction, const char* szExpression, const char* szAssertMsg)
    {
      if (plStringUtils::IsEqual(szFunction, "plGALDevice::GetDefaultDevice"))
        return false;

      return s_PreviousAssertHandler(szSourceFile, uiLine, szFunction, szExpression, szAssertMsg);
    }

    void InstallAssertHandler()
    {
      // after the test framework has installed its own handler, so this one is only set up with the first world
      if (s_PreviousAssertHandler == nullptr)
      {
        s_PreviousAssertHandler = plGetAssertHandler();
        plSetAssertHandler(AssertHandler);
      }
    }
  } // namespace

  World::World(float fFrameRate)
    : m_fFrameRate(fFrameRate)
  {
    InstallAssertHandler();

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
} // namespace JoltTest
