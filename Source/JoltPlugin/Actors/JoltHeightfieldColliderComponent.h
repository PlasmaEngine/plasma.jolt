#pragma once

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <JoltPlugin/JoltPluginDLL.h>

using plJoltHeightfieldColliderComponentManager = plComponentManager<class plJoltHeightfieldColliderComponent, plBlockStorageType::FreeList>;

/// Manages a Jolt static body with a heightfield shape, attached programmatically via plPhysicsWorldModuleInterface.
///
/// This component is not added through the editor. Use plPhysicsWorldModuleInterface::CreateHeightfieldCollider()
/// or TrySetHeightfieldCollider() to create and attach it to a game object. It owns the associated Jolt body
/// and removes it from the physics system when deactivated or deleted.
class PL_JOLTPLUGIN_DLL plJoltHeightfieldColliderComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltHeightfieldColliderComponent, plComponent, plJoltHeightfieldColliderComponentManager);

protected:
  virtual void OnDeactivated() override;

public:
  plJoltHeightfieldColliderComponent();
  ~plJoltHeightfieldColliderComponent();

private:
  friend class plJoltWorldModule;

  plUInt32 m_uiJoltBodyID = plInvalidIndex;
  plUInt32 m_uiUserDataIndex = plInvalidIndex;
  plUInt32 m_uiObjectFilterID = plInvalidIndex;

  /// Identifier of the shape cache entry this body was created from. Empty if the shape is not cached.
  plString m_sCacheIdentifier;
};