#pragma once

#include <JoltPlugin/JoltPluginDLL.h>

#include <Core/World/World.h>

namespace JPH
{
  class PhysicsSystem;
} // namespace JPH

class PL_JOLTPLUGIN_DLL plJoltWaterVolumeComponentManager : public plComponentManager<class plJoltWaterVolumeComponent, plBlockStorageType::FreeList>
{
public:
  plJoltWaterVolumeComponentManager(plWorld* pWorld);
  ~plJoltWaterVolumeComponentManager();
  virtual void Initialize() override;

  void UpdateWaterVolumes(const plWorldModule::UpdateContext& context);
};

//////////////////////////////////////////////////////////////////////////

/// \brief Legacy box-only buoyancy volume. Prefer plJoltWaterBuoyancyComponent together with a WaterPlugin body.
class PL_JOLTPLUGIN_DLL plJoltWaterVolumeComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltWaterVolumeComponent, plComponent, plJoltWaterVolumeComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plJoltWaterVolumeComponent

public:
  plJoltWaterVolumeComponent();
  ~plJoltWaterVolumeComponent();

  plVec3 m_vExtents = plVec3(10.0f);        // [ property ]
  plVec2 m_vLocalFlow = plVec2::MakeZero(); // [ property ]

private:
  void Update(JPH::PhysicsSystem& joltSystem, plTime deltaTime);
};
