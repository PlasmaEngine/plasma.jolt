#pragma once

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <JoltPlugin/JoltPluginDLL.h>

using plJoltTriangleMeshColliderComponentManager = plComponentManager<class plJoltTriangleMeshColliderComponent, plBlockStorageType::FreeList>;

/// Manages a Jolt static body with a triangle mesh shape, attached programmatically via plPhysicsWorldModuleInterface.
///
/// Not added through the editor. Use plPhysicsWorldModuleInterface::CreateTriangleMeshCollider() to create and
/// attach it to a game object. Owns the associated Jolt body and removes it from the physics system on deactivation.
class PL_JOLTPLUGIN_DLL plJoltTriangleMeshColliderComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltTriangleMeshColliderComponent, plComponent, plJoltTriangleMeshColliderComponentManager);

protected:
  virtual void OnDeactivated() override;

public:
  plJoltTriangleMeshColliderComponent();
  ~plJoltTriangleMeshColliderComponent();

private:
  friend class plJoltWorldModule;

  plUInt32 m_uiJoltBodyID = plInvalidIndex;
  plUInt32 m_uiUserDataIndex = plInvalidIndex;
  plUInt32 m_uiObjectFilterID = plInvalidIndex;
};