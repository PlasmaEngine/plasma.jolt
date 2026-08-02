#pragma once

#include <JoltPlugin/JoltPluginDLL.h>

class plComponent;
class plJoltDynamicActorComponent;
class plJoltStaticActorComponent;
class plJoltTriggerComponent;
class plJoltCharacterControllerComponent;
class plJoltShapeComponent;
class plJoltQueryShapeActorComponent;
class plJoltRagdollComponent;
class plJoltRopeComponent;
class plJoltActorComponent;
class plJoltClothSheetComponent;
class plJoltBreakableSlabComponent;
class plJoltDestructibleComponent;
class plJoltVegetationRodComponent;
class plJoltHeightfieldColliderComponent;
class plJoltTriangleMeshColliderComponent;

class plJoltUserData
{
public:
  PL_DECLARE_POD_TYPE();

  enum class Type
  {
    Invalid,
    DynamicActorComponent,
    StaticActorComponent,
    TriggerComponent,
    CharacterComponent,
    ShapeComponent,
    BreakableSlabComponent,
    QueryShapeActorComponent,
    RagdollComponent,
    RopeComponent,
    ClothSheetComponent,
    VegetationRodComponent,
    HeightfieldColliderComponent,
    TriangleMeshColliderComponent,
    DestructibleComponent,
  };

  plJoltUserData() = default;
  ~plJoltUserData() = default;

  PL_ALWAYS_INLINE void Init(plJoltDynamicActorComponent* pObject, plBitflags<plOnJoltContact> contactFlags)
  {
    m_Type = Type::DynamicActorComponent;
    m_pObject = pObject;
    m_OnContact = contactFlags;
  }

  PL_ALWAYS_INLINE void Init(plJoltStaticActorComponent* pObject)
  {
    m_Type = Type::StaticActorComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltTriggerComponent* pObject)
  {
    m_Type = Type::TriggerComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltCharacterControllerComponent* pObject)
  {
    m_Type = Type::CharacterComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltShapeComponent* pObject)
  {
    m_Type = Type::ShapeComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltQueryShapeActorComponent* pObject)
  {
    m_Type = Type::QueryShapeActorComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltRagdollComponent* pObject)
  {
    m_Type = Type::RagdollComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltRopeComponent* pObject)
  {
    m_Type = Type::RopeComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltClothSheetComponent* pObject)
  {
    m_Type = Type::ClothSheetComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltVegetationRodComponent* pObject)
  {
    m_Type = Type::VegetationRodComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltBreakableSlabComponent* pObject, plBitflags<plOnJoltContact> contactFlags)
  {
    m_Type = Type::BreakableSlabComponent;
    m_pObject = pObject;
    m_OnContact = contactFlags;
  }

  PL_ALWAYS_INLINE void Init(plJoltDestructibleComponent* pObject, plBitflags<plOnJoltContact> contactFlags)
  {
    m_Type = Type::DestructibleComponent;
    m_pObject = pObject;
    m_OnContact = contactFlags;
  }

  PL_ALWAYS_INLINE void Init(plJoltHeightfieldColliderComponent* pObject)
  {
    m_Type = Type::HeightfieldColliderComponent;
    m_pObject = pObject;
  }

  PL_ALWAYS_INLINE void Init(plJoltTriangleMeshColliderComponent* pObject)
  {
    m_Type = Type::TriangleMeshColliderComponent;
    m_pObject = pObject;
  }

  PL_FORCE_INLINE void Invalidate()
  {
    m_Type = Type::Invalid;
    m_pObject = nullptr;
  }

  PL_FORCE_INLINE static Type GetType(const void* pUserData)
  {
    const plJoltUserData* pJoltUserData = static_cast<const plJoltUserData*>(pUserData);
    if (pJoltUserData == nullptr)
      return Type::Invalid;

    return pJoltUserData->m_Type;
  }

  PL_FORCE_INLINE void* GetObject() const
  {
    return m_pObject;
  }

  PL_FORCE_INLINE static plComponent* GetComponent(const void* pUserData)
  {
    const plJoltUserData* pJoltUserData = static_cast<const plJoltUserData*>(pUserData);
    if (pJoltUserData == nullptr || pJoltUserData->m_Type == Type::Invalid)
    {
      return nullptr;
    }

    return static_cast<plComponent*>(pJoltUserData->m_pObject);
  }

  PL_FORCE_INLINE static plJoltDynamicActorComponent* GetDynamicActorComponent(const void* pUserData)
  {
    const plJoltUserData* pJoltUserData = static_cast<const plJoltUserData*>(pUserData);
    if (pJoltUserData != nullptr && pJoltUserData->m_Type == Type::DynamicActorComponent)
    {
      return static_cast<plJoltDynamicActorComponent*>(pJoltUserData->m_pObject);
    }

    return nullptr;
  }

  PL_FORCE_INLINE static plJoltTriggerComponent* GetTriggerComponent(const void* pUserData)
  {
    const plJoltUserData* pJoltUserData = static_cast<const plJoltUserData*>(pUserData);
    if (pJoltUserData != nullptr && pJoltUserData->m_Type == Type::TriggerComponent)
    {
      return static_cast<plJoltTriggerComponent*>(pJoltUserData->m_pObject);
    }

    return nullptr;
  }

  PL_FORCE_INLINE static plBitflags<plOnJoltContact> GetContactFlags(const void* pUserData)
  {
    if (const plJoltUserData* pJoltUserData = static_cast<const plJoltUserData*>(pUserData))
    {
      return pJoltUserData->m_OnContact;
    }

    return plOnJoltContact::None;
  }

  PL_FORCE_INLINE plBitflags<plOnJoltContact> GetContactFlags() const
  {
    return m_OnContact;
  }

private:
  Type m_Type = Type::Invalid;
  void* m_pObject = nullptr;
  plBitflags<plOnJoltContact> m_OnContact;
};
