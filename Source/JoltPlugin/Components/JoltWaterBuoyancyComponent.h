#pragma once

#include <Core/World/Component.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec3.h>
#include <JoltPlugin/JoltPluginDLL.h>

namespace JPH
{
  class PhysicsSystem;
}

class PL_JOLTPLUGIN_DLL plJoltWaterBuoyancyComponentManager : public plComponentManager<class plJoltWaterBuoyancyComponent, plBlockStorageType::FreeList>
{
public:
  using SUPER = plComponentManager<plJoltWaterBuoyancyComponent, plBlockStorageType::FreeList>;

  plJoltWaterBuoyancyComponentManager(plWorld* pWorld);

  virtual void Initialize() override;

private:
  void Update(const plWorldModule::UpdateContext& context);
};

/// \brief Applies profile-driven water buoyancy to a sibling plJoltDynamicActorComponent.
///
/// Custom pontoons are authored in owner-local space. With shape-derived pontoons enabled and no
/// custom points, the component samples the lower corners and center of the owner's world bounds.
class PL_JOLTPLUGIN_DLL plJoltWaterBuoyancyComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltWaterBuoyancyComponent, plComponent, plJoltWaterBuoyancyComponentManager);

public:
  plJoltWaterBuoyancyComponent();

  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  float GetImmersion() const { return m_fImmersion; } // [ property ]
  plVec3 GetWaterFlow() const { return m_vWaterFlow; } // [ property ]

  bool m_bUseShapeDerivedPontoons = true; // [ property ]
  plDynamicArray<plVec3> m_Pontoons;       // [ property ]
  float m_fPontoonRadius = 0.5f;           // [ property ]
  float m_fBuoyancyFactor = 1.0f;          // [ property ]
  float m_fDensityScale = 1.0f;            // [ property ]
  float m_fDragScale = 1.0f;               // [ property ]

private:
  friend class plJoltWaterBuoyancyComponentManager;

  void ApplyBuoyancy(JPH::PhysicsSystem& joltSystem, const class plWaterWorldModuleInterface& waterModule, plTime deltaTime);
  void BuildSamplePoints(plHybridArray<plVec3, 8>& out_points) const;

  float m_fImmersion = 0.0f;
  plVec3 m_vWaterFlow = plVec3::MakeZero();
};
