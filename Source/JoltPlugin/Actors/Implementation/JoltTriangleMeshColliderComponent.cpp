#include <JoltPlugin/JoltPluginPCH.h>

#include <Jolt/Physics/PhysicsSystem.h>
#include <JoltPlugin/Actors/JoltTriangleMeshColliderComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plJoltTriangleMeshColliderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Physics/Jolt/Actors"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plJoltTriangleMeshColliderComponent::plJoltTriangleMeshColliderComponent() = default;
plJoltTriangleMeshColliderComponent::~plJoltTriangleMeshColliderComponent() = default;

void plJoltTriangleMeshColliderComponent::OnDeactivated()
{
  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();

  if (pModule != nullptr)
  {
    JPH::BodyID bodyId(m_uiJoltBodyID);

    if (!bodyId.IsInvalid())
    {
      auto* pSystem = pModule->GetJoltSystem();
      auto* pBodies = &pSystem->GetBodyInterface();

      if (pBodies->IsAdded(bodyId))
      {
        pBodies->RemoveBody(bodyId);
      }
      else
      {
        pModule->RemoveBodyFromQueue(bodyId);
      }

      pBodies->DestroyBody(bodyId);
      m_uiJoltBodyID = JPH::BodyID::cInvalidBodyID;
    }

    pModule->DeallocateUserData(m_uiUserDataIndex);
    pModule->DeleteObjectFilterID(m_uiObjectFilterID);
  }

  m_Surfaces.Clear();
  SUPER::OnDeactivated();
}


PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Actors_Implementation_JoltTriangleMeshColliderComponent);
