#pragma once

#include <Core/Interfaces/NavmeshGeoWorldModule.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/World/Declarations.h>
#include <Core/World/WorldModule.h>
#include <Foundation/Containers/IdTable.h>
#include <Foundation/Types/UniquePtr.h>
#include <JoltPlugin/Declarations.h>
#include <JoltPlugin/JoltPluginDLL.h>
#include <JoltPlugin/System/JoltCollisionFiltering.h>
#include <JoltPlugin/Utilities/JoltUserData.h>
#include <RendererCore/Meshes/DynamicMeshBufferResource.h>

class plJoltCharacterControllerComponent;
class plJoltContactListener;
class plJoltSoftBodyContactListener;
class plJoltMaterial;
class plJoltRagdollComponent;
class plJoltRopeComponent;
class plJoltBreakableSlabComponent;
class plJoltDestructibleComponent;
class plView;

namespace JPH
{
  class Body;
  class TempAllocator;
  class PhysicsSystem;
  class GroupFilter;
  class Shape;

  class BodyInterface;
} // namespace JPH

struct plJoltImpulse
{
  plUInt32 m_uiBodyID;
  plVec3 m_vImpulse;
  plVec3 m_vGlobalPosition;

  enum class Type : plUInt8
  {
    Center,
    AtGlobalPos,
    Angular
  };

  Type m_Type;
};

struct plJoltForce
{
  plUInt32 m_uiBodyID;
  plTime m_tDisable;
  plVec3 m_vForce;
};

using plJoltForceId = plGenericId<24, 8>;

class PL_JOLTPLUGIN_DLL plJoltWorldModule : public plPhysicsWorldModuleInterface
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plJoltWorldModule, plPhysicsWorldModuleInterface);

public:
  plJoltWorldModule(plWorld* pWorld);
  ~plJoltWorldModule();

  virtual void Initialize() override;
  virtual void Deinitialize() override;
  virtual void OnSimulationStarted() override;

  JPH::PhysicsSystem* GetJoltSystem() { return m_pSystem.get(); }
  const JPH::PhysicsSystem* GetJoltSystem() const { return m_pSystem.get(); }

  JPH::BodyInterface& GetBodyInterface() { return m_pSystem->GetBodyInterface(); }

  plUInt32 CreateObjectFilterID();
  void DeleteObjectFilterID(plUInt32& ref_uiObjectFilterID);

  plUInt32 AllocateUserData(plJoltUserData*& out_pUserData);
  void DeallocateUserData(plUInt32& ref_uiUserDataId);
  const plJoltUserData& GetUserData(plUInt32 uiUserDataId) const;

  void SetGravity(const plVec3& vObjectGravity, const plVec3& vCharacterGravity);
  virtual plVec3 GetGravity() const override { return plVec3(0, 0, -10); }
  plVec3 GetObjectGravity() const { return m_Settings.m_vObjectGravity; }
  plVec3 GetCharacterGravity() const { return m_Settings.m_vCharacterGravity; }

  /// \brief Queues an impulse to be applied on the given body as soon as that body is added to the Jolt scene.
  void AddImpulse(plUInt32 uiBodyID, const plVec3& vImpulse, const plVec3& vGlobalPosition);

  /// \brief Queues an impulse to be applied on the given body as soon as that body is added to the Jolt scene.
  void AddImpulse(plUInt32 uiBodyID, const plVec3& vImpulse);

  /// \brief Queues an angular impulse to be applied on the given body as soon as that body is added to the Jolt scene.
  void AddTorque(plUInt32 uiBodyID, const plVec3& vImpulse);

  /// \brief Creates a force that acts upon the given Jolt body for a limited time.
  ///
  /// The force is applied every frame. It can be updated by calling this function again with a previously returned force ID.
  /// Once the duration is elapsed without an update, the force is removed.
  ///
  /// If an invalid ID is passed in, a new force is created and a valid ID is returned.
  /// If a valid ID is passed in, the existing force gets updated, and the same ID is returned.
  plJoltForceId AddOrUpdateForce(plJoltForceId forceId, plUInt32 uiBodyID, plTime duration, const plVec3& vForce);

  /// \brief Removes a previously added force. See AddOrUpdateForce().
  void ClearForce(plJoltForceId id);

  //////////////////////////////////////////////////////////////////////////
  // plPhysicsWorldModuleInterface
  //

  virtual plUInt32 GetCollisionLayerByName(plStringView sName) const override;
  virtual plUInt8 GetWeightCategoryByName(plStringView sName) const override;
  virtual plUInt8 GetImpulseTypeByName(plStringView sName) const override;

  virtual bool Raycast(plPhysicsCastResult& out_result, const plVec3& vStart, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection = plPhysicsHitCollection::Closest) const override;

  virtual bool RaycastAll(plPhysicsCastResultArray& out_results, const plVec3& vStart, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params) const override;

  virtual bool SweepTestSphere(plPhysicsCastResult& out_result, float fSphereRadius, const plVec3& vStart, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection = plPhysicsHitCollection::Closest) const override;

  virtual bool SweepTestBox(plPhysicsCastResult& out_result, const plVec3& vBoxExtents, const plTransform& transform, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection = plPhysicsHitCollection::Closest) const override;

  virtual bool SweepTestCapsule(plPhysicsCastResult& out_result, float fCapsuleRadius, float fCapsuleHeight, const plTransform& transform, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection = plPhysicsHitCollection::Closest) const override;

  virtual bool SweepTestCylinder(plPhysicsCastResult& out_result, float fCylinderRadius, float fCylinderHeight, const plTransform& transform, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection = plPhysicsHitCollection::Closest) const override;

  virtual bool OverlapTestSphere(float fSphereRadius, const plVec3& vPosition, const plPhysicsQueryParameters& params) const override;

  virtual bool OverlapTestBox(const plVec3& vBoxExtents, const plVec3& vPosition, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual bool OverlapTestCapsule(float fCapsuleRadius, float fCapsuleHeight, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual bool OverlapTestCylinder(float fCylinderRadius, float fCylinderHeight, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual void QueryShapesInSphere(plPhysicsOverlapResultArray& out_results, float fSphereRadius, const plVec3& vPosition, const plPhysicsQueryParameters& params) const override;

  virtual void QueryShapesInBox(plPhysicsOverlapResultArray& out_results, const plVec3& vBoxExtents, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual void QueryShapesInCapsule(plPhysicsOverlapResultArray& out_results, float fCapsuleRadius, float fCapsuleHeight, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual void QueryShapesInCylinder(plPhysicsOverlapResultArray& out_results, float fCylinderRadius, float fCylinderHeight, const plTransform& transform, const plPhysicsQueryParameters& params) const override;

  virtual void AddStaticCollisionBox(plGameObject* pObject, plVec3 vBoxSize) override;

  virtual void AddFixedJointComponent(plGameObject* pOwner, const plPhysicsWorldModuleInterface::FixedJointConfig& cfg) override;

  virtual plBoundingBoxSphere GetWorldSpaceBounds(plGameObject* pOwner, plUInt32 uiCollisionLayer, plBitflags<plPhysicsShapeType> shapeTypes, bool bIncludeChildObjects) const override;

  virtual plResult TrySetHeightfieldCollider(plGameObject* pOwner, plStringView sIdentifier, plUInt8 uiCollisionLayer) override;

  virtual void CreateHeightfieldCollider(plGameObject* pOwner, plStringView sIdentifier, const HeightfieldColliderData& data) override;

  virtual void RemoveHeightfieldCollider(plGameObject* pOwner) override;

  virtual void CreateTriangleMeshCollider(plGameObject* pOwner, const TriangleMeshColliderData& data) override;

  virtual void RemoveTriangleMeshCollider(plGameObject* pOwner) override;

  plDeque<plComponentHandle> m_RequireUpdate;

  const plSet<plJoltDynamicActorComponent*>& GetActiveActors() const { return m_ActiveActors; }
  const plMap<plJoltRopeComponent*, plInt32>& GetActiveRopes() const { return m_ActiveRopes; }
  const plMap<plJoltRagdollComponent*, plInt32>& GetActiveRagdolls() const { return m_ActiveRagdolls; }
  plArrayPtr<plJoltRagdollComponent*> GetRagdollsPutToSleep() { return m_RagdollsPutToSleep.GetArrayPtr(); }
  const plMap<plJoltBreakableSlabComponent*, plInt32>& GetActiveSlabs() const { return m_ActiveSlabs; }
  plArrayPtr<plJoltBreakableSlabComponent*> GetSlabsPutToSleep() { return m_SlabsPutToSleep.GetArrayPtr(); }
  const plMap<plJoltDestructibleComponent*, plInt32>& GetActiveDestructibles() const { return m_ActiveDestructibles; }
  plArrayPtr<plJoltDestructibleComponent*> GetDestructiblesPutToSleep() { return m_DestructiblesPutToSleep.GetArrayPtr(); }

  void QueueBodyToAdd(JPH::Body* pBody, bool bAwake);
  void RemoveBodyFromQueue(JPH::BodyID bodyId);

  JPH::GroupFilter* GetGroupFilter() const { return m_pGroupFilter; }
  JPH::GroupFilter* GetGroupFilterIgnoreSame() const { return m_pGroupFilterIgnoreSame; }

  void EnableJoinedBodiesCollisions(plUInt32 uiObjectFilterID1, plUInt32 uiObjectFilterID2, bool bEnable);

  JPH::TempAllocator* GetTempAllocator() const { return m_pTempAllocator.get(); }

  void ActivateCharacterController(plJoltCharacterControllerComponent* pCharacter, bool bActivate);

  plJoltContactListener* GetContactListener()
  {
    return reinterpret_cast<plJoltContactListener*>(m_pContactListener);
  }

  plJoltSoftBodyContactListener* GetSoftBodyContactListener()
  {
    return reinterpret_cast<plJoltSoftBodyContactListener*>(m_pSoftBodyContactListener);
  }

  void CheckBreakableConstraints();

  /// \brief Debugging aid: logs errors for bodies with corrupted state (unknown group filters, NaN transforms/bounds).
  void ValidateBodies(const char* szWhen);

  plSet<plComponentHandle> m_BreakableConstraints;

  void QueryGeometryInBox(const plPhysicsQueryParameters& params, plBoundingBox box, plDynamicArray<plNavmeshTriangle>& out_triangles) const;

  /// \brief Returns the counter of the last Jolt update.
  /// Can be used to detect when no physics update was done (at high frame rates) to skip duplicate physics modifications.
  plUInt64 GetJoltUpdateCounter() const { return m_uiJoltUpdateCounter; }

  void ReleaseHeightfieldCacheEntry(plStringView sIdentifier);

private:
  struct HeightfieldCacheEntry
  {
    JPH::Ref<JPH::Shape> m_pShape;
    plDynamicArray<plSurfaceResourceHandle> m_Surfaces;
    plDynamicArray<const plJoltMaterial*> m_MaterialPtrs;
    plUInt32 m_uiRefCount = 0;
  };

  bool SweepTest(plPhysicsCastResult& out_Result, const JPH::Shape& shape, const JPH::Mat44& transform, const plVec3& vDir, float fDistance, const plPhysicsQueryParameters& params, plPhysicsHitCollection collection) const;
  static plResult BuildJoltHeightfieldShape(const plPhysicsWorldModuleInterface::HeightfieldColliderData& data, HeightfieldCacheEntry& out_Data);
  static void AttachHeightfieldBody(plJoltWorldModule* pModule, plGameObject* pOwner, HeightfieldCacheEntry& cacheEntry, plUInt8 uiCollisionLayer, plStringView sCacheIdentifier);
  static plResult BuildJoltTriangleMeshShape(const plPhysicsWorldModuleInterface::TriangleMeshColliderData& data, JPH::Ref<JPH::Shape>& out_pShape, const plJoltMaterial*& out_pFirstMaterial);
  static void AttachTriangleMeshBody(plJoltWorldModule* pModule, plGameObject* pOwner, JPH::Ref<JPH::Shape> pShape, const plJoltMaterial* pMaterial, plUInt8 uiCollisionLayer);
  bool OverlapTest(const JPH::Shape& shape, const JPH::Mat44& transform, const plPhysicsQueryParameters& params) const;
  void QueryShapes(plPhysicsOverlapResultArray& out_results, const JPH::Shape& shape, const JPH::Mat44& transform, const plPhysicsQueryParameters& params) const;

  void FreeUserDataAfterSimulationStep();

  void StartSimulation(const plWorldModule::UpdateContext& context);
  void FetchResults(const plWorldModule::UpdateContext& context);

  void Simulate();

  void UpdateSettingsCfg();
  void ApplySettingsCfg();

  void ApplyImpulses();
  void UpdateForces();

  void UpdateConstraints();

  plTime CalculateUpdateSteps();

  void DebugDrawGeometry();
  void DebugDrawGeometry(const plVec3& vCenter, float fRadius, plPhysicsShapeType::Enum shapeType, const plTag& tag);

  struct DebugGeo
  {
    plGameObjectHandle m_hObject;
    plUInt32 m_uiLastSeenCounter = 0;
    bool m_bMutableGeometry = false;
  };

  struct DebugGeoShape
  {
    plDynamicMeshBufferResourceHandle m_hMesh;
    plBoundingBox m_Bounds;
    plUInt32 m_uiLastSeenCounter = 0;
  };

  struct DebugBodyShapeKey
  {
    plUInt32 m_uiBodyID;
    const void* m_pShapePtr;

    bool operator<(const DebugBodyShapeKey& rhs) const
    {
      if (m_uiBodyID == rhs.m_uiBodyID)
        return m_pShapePtr < rhs.m_pShapePtr;

      return m_uiBodyID < rhs.m_uiBodyID;
    }

    bool operator==(const DebugBodyShapeKey& rhs) const
    {
      return (m_uiBodyID == rhs.m_uiBodyID) && (m_pShapePtr == rhs.m_pShapePtr);
    }
  };

  plUInt64 m_uiJoltUpdateCounter = 0;

  plUInt32 m_uiDebugGeoLastSeenCounter = 0;
  plMap<DebugBodyShapeKey, DebugGeo> m_DebugDrawComponents;
  plMap<const void*, DebugGeoShape> m_DebugDrawShapeGeo;

  plUInt32 m_uiNextObjectFilterID = 1;
  plDynamicArray<plUInt32> m_FreeObjectFilterIDs;

  plDeque<plJoltUserData> m_AllocatedUserData;
  plDynamicArray<plUInt32> m_FreeUserData;
  plDynamicArray<plUInt32> m_FreeUserDataAfterSimulationStep;

  plTime m_AccumulatedTimeSinceUpdate;

  plJoltSettings m_Settings;

  plSharedPtr<plTask> m_pSimulateTask;
  plTaskGroupID m_SimulateTaskGroupId;
  bool m_bSimulateTaskStarted = false; ///< Whether StartSimulation started a simulate task THIS frame (m_SimulateTaskGroupId is stale otherwise)
  plTime m_SimulatedTimeStep;

  std::unique_ptr<JPH::PhysicsSystem> m_pSystem;
  std::unique_ptr<JPH::TempAllocator> m_pTempAllocator;

  plJoltObjectToBroadphaseLayer m_ObjectToBroadphase;
  plJoltObjectVsBroadPhaseLayerFilter m_ObjectVsBroadphaseFilter;
  plJoltObjectLayerPairFilter m_ObjectLayerPairFilter;

  void* m_pContactListener = nullptr;
  void* m_pSoftBodyContactListener = nullptr;
  void* m_pActivationListener = nullptr;
  plSet<plJoltDynamicActorComponent*> m_ActiveActors;
  plMap<plJoltRopeComponent*, plInt32> m_ActiveRopes;
  plMap<plJoltRagdollComponent*, plInt32> m_ActiveRagdolls;
  plDynamicArray<plJoltRagdollComponent*> m_RagdollsPutToSleep;
  plMap<plJoltBreakableSlabComponent*, plInt32> m_ActiveSlabs;
  plDynamicArray<plJoltBreakableSlabComponent*> m_SlabsPutToSleep;
  plMap<plJoltDestructibleComponent*, plInt32> m_ActiveDestructibles;
  plDynamicArray<plJoltDestructibleComponent*> m_DestructiblesPutToSleep;

  JPH::GroupFilter* m_pGroupFilter = nullptr;
  JPH::GroupFilter* m_pGroupFilterIgnoreSame = nullptr;

  plUInt32 m_uiBodiesAddedSinceOptimize = 100;
  plDeque<plUInt32> m_BodiesToAdd;
  plDeque<plUInt32> m_BodiesToAddAndActivate;

  plHybridArray<plTime, 4> m_UpdateSteps;
  plHybridArray<plJoltCharacterControllerComponent*, 4> m_ActiveCharacters;

  // Maps identifier strings to cached Jolt heightfield shapes. Shapes are ref-counted; the cache holds one ref each.
  plMap<plString, HeightfieldCacheEntry> m_HeightfieldShapeCache;

  plMutex m_ImpulsesMutex;
  plDeque<plJoltImpulse> m_Impulses;

  plMutex m_ForcesMutex;
  plIdTable<plJoltForceId, plJoltForce> m_Forces;
};

/// \brief Implementation of the plNavmeshGeoWorldModuleInterface that uses Jolt physics to retrieve the geometry
/// from which to generate a navmesh.
class PL_JOLTPLUGIN_DLL plJoltNavmeshGeoWorldModule : public plNavmeshGeoWorldModuleInterface
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plJoltNavmeshGeoWorldModule, plNavmeshGeoWorldModuleInterface);

public:
  plJoltNavmeshGeoWorldModule(plWorld* pWorld);

  virtual void RetrieveGeometryInArea(plUInt32 uiCollisionLayer, const plBoundingBox& box, plDynamicArray<plNavmeshTriangle>& out_triangles) const override;

private:
  plJoltWorldModule* m_pJoltModule = nullptr;
};
