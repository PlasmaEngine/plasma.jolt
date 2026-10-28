#include "JoltTestSupport.h"
#include <GameEngine/Interfaces/WaterWorldModule.h>
#include <JoltPlugin/Components/JoltWaterBuoyancyComponent.h>

using namespace JoltTest;

/// \brief A flat water plane at z = 0 for tests that need a water module without a renderer.
class TestWaterModule : public plWaterWorldModuleInterface
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(TestWaterModule, plWaterWorldModuleInterface);

public:
  TestWaterModule(plWorld* pWorld)
    : plWaterWorldModuleInterface(pWorld)
  {
  }

  virtual bool TrySampleSurface(const plWaterSurfaceQuery& query, plWaterSurfaceResult& out_result) const override
  {
    const float fDepth = -query.m_vPosition.z;
    if (plMath::Abs(fDepth) > query.m_fMaxVerticalDistance)
      return false;

    out_result = plWaterSurfaceResult();
    out_result.m_bContained = true;
    out_result.m_fDepth = fDepth;
    return true;
  }

  virtual bool TrySampleSurfaceAtTime(const plWaterSurfaceQuery& query, plTime simulationTime, plWaterSurfaceResult& out_result) const override
  {
    return TrySampleSurface(query, out_result);
  }

  virtual bool TrySampleSurfaceAtTick(const plWaterSurfaceQuery& query, plUInt64 uiSimulationTick, plWaterSurfaceResult& out_result) const override
  {
    return TrySampleSurface(query, out_result);
  }

  virtual plUInt64 GetSimulationTick() const override { return 0; }
  virtual plTime GetFixedSimulationStep() const override { return plTime::MakeFromSeconds(1.0 / 60.0); }
  virtual void AddInteraction(plWaterInteraction interaction) override {}
  virtual plUInt64 CopyInteractionsSince(plUInt64 uiAfterSequence, plDynamicArray<plWaterInteraction>& out_interactions) const override { return uiAfterSequence; }
};

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(TestWaterModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(TestWaterModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

/// A heavy box with four pontoons that starts below the water surface.
class Boat
{
public:
  explicit Boat(float fFrameRate)
    : m_World(fFrameRate)
  {
    {
      PL_LOCK(m_World.GetWorld().GetWriteMarker());
      m_World.GetWorld().GetOrCreateModule<TestWaterModule>();
    }

    m_pObject = m_World.AddDynamicBox(plVec3(0, 0, -1.0f), plVec3(1.0f, 1.0f, 0.5f));

    PL_LOCK(m_World.GetWorld().GetWriteMarker());

    plJoltDynamicActorComponent* pActor = nullptr;
    m_pObject->TryGetComponentOfBaseType(pActor);
    pActor->m_bAllowSleeping = false;

    plJoltWaterBuoyancyComponent::CreateComponent(m_pObject, m_pBuoyancy);
    m_pBuoyancy->m_bUseShapeDerivedPontoons = false;
    m_pBuoyancy->m_fBuoyancyFactor = 0.0f;

    for (const plVec3& vPontoon : {plVec3(0.75f, 0.75f, -0.3f), plVec3(-0.75f, 0.75f, -0.3f), plVec3(0.75f, -0.75f, -0.3f), plVec3(-0.75f, -0.75f, -0.3f), plVec3(0, 0, 0)})
      m_pBuoyancy->m_Pontoons.PushBack(vPontoon);
  }

  /// Sizes the lift to twice the weight when all five pontoons are fully submerged, so the boat settles half out of the water.
  void ScaleLiftToMass()
  {
    m_World.Step(1);

    PL_LOCK(m_World.GetWorld().GetWriteMarker());
    plJoltDynamicActorComponent* pActor = nullptr;
    m_pObject->TryGetComponentOfBaseType(pActor);

    const float fPontoonVolume = (4.0f / 3.0f) * plMath::Pi<float>() * m_pBuoyancy->m_fPontoonRadius * m_pBuoyancy->m_fPontoonRadius * m_pBuoyancy->m_fPontoonRadius;
    m_pBuoyancy->m_fBuoyancyFactor = 2.0f * pActor->GetMass() / (5.0f * 1000.0f * fPontoonVolume);
  }

  float Height() { return m_World.GetPosition(m_pObject->GetHandle()).z; }

  World m_World;
  plGameObject* m_pObject = nullptr;
  plJoltWaterBuoyancyComponent* m_pBuoyancy = nullptr;
};

/// A box that is pushed up by the water settles near the surface instead of sinking or flying away.
static void TestBoatFloats()
{
  Boat boat(60.0f);
  boat.ScaleLiftToMass();
  boat.m_World.StepSeconds(12.0f);

  const float fHeight = boat.Height();
  Check(fHeight > -1.0f && fHeight < 1.0f, "the boat floats near the water surface");
}

/// The same boat must be at the same place after the same simulated time, whatever the frame rate is.
static void TestBuoyancyIsFrameRateIndependent()
{
  const float fRates[] = {30.0f, 60.0f, 144.0f};
  float fEarly[3];
  float fLate[3];

  for (plUInt32 i = 0; i < 3; ++i)
  {
    Boat boat(fRates[i]);
    boat.ScaleLiftToMass();
    boat.m_World.StepSeconds(1.0f);
    fEarly[i] = boat.Height();
    boat.m_World.StepSeconds(11.0f);
    fLate[i] = boat.Height();
  }

  CheckNear(fEarly[0], fEarly[1], 0.15f, "boat height after 1 s agrees at 30 and 60 fps", Category::R1Gate);
  CheckNear(fEarly[2], fEarly[1], 0.15f, "boat height after 1 s agrees at 144 and 60 fps", Category::R1Gate);
  CheckNear(fLate[0], fLate[1], 0.05f, "resting boat height agrees at 30 and 60 fps", Category::R1Gate);
  CheckNear(fLate[2], fLate[1], 0.05f, "resting boat height agrees at 144 and 60 fps", Category::R1Gate);
}

void RunBuoyancyTests()
{
  BeginGroup("Buoyancy");

  TestBoatFloats();
  TestBuoyancyIsFrameRateIndependent();
}
