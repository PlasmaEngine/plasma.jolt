#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Physics/SurfaceResource.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Types/TagRegistry.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <JoltPlugin/Actors/JoltDynamicActorComponent.h>
#include <JoltPlugin/Actors/JoltHeightfieldColliderComponent.h>
#include <JoltPlugin/Actors/JoltTriangleMeshColliderComponent.h>
#include <JoltPlugin/Actors/JoltQueryShapeActorComponent.h>
#include <JoltPlugin/Actors/JoltStaticActorComponent.h>
#include <JoltPlugin/Actors/JoltTriggerComponent.h>
#include <JoltPlugin/Character/JoltCharacterControllerComponent.h>
#include <JoltPlugin/Components/JoltBreakableSlabComponent.h>
#include <JoltPlugin/Components/JoltDestructibleComponent.h>
#include <JoltPlugin/Components/JoltRagdollComponent.h>
#include <JoltPlugin/Components/JoltSettingsComponent.h>
#include <JoltPlugin/Constraints/JoltConstraintComponent.h>
#include <JoltPlugin/Constraints/JoltFixedConstraintComponent.h>
#include <JoltPlugin/Resources/JoltMaterial.h>
#include <JoltPlugin/Shapes/JoltShapeBoxComponent.h>
#include <JoltPlugin/System/JoltCollisionFiltering.h>
#include <JoltPlugin/System/JoltContacts.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltDebugRenderer.h>
#include <JoltPlugin/System/JoltTempAlloc.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>
#include <JoltPlugin/Utilities/JoltUserData.h>
#include <Physics/Collision/CollisionCollectorImpl.h>
#include <Physics/Collision/Shape/Shape.h>
#include <RendererCore/Meshes/CustomMeshComponent.h>
#include <RendererCore/Meshes/DynamicMeshBufferResource.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plJoltWorldModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE
// clang-format on

plCVarBool cvar_JoltSimulationPause("Jolt.Simulation.Pause", false, plCVarFlags::None, "Pauses the physics simulation.");
plCVarBool cvar_JoltValidateBodies("Jolt.Simulation.ValidateBodies", false, plCVarFlags::None, "Debugging aid: checks all bodies for corrupted state (unknown group filters, NaN transforms) around each simulation step. Note: bodies with custom group filters (e.g. ragdolls) are reported as unknown.");
plCVarInt cvar_JoltMaxUpdateSteps("Jolt.MaxUpdateSteps", 0, plCVarFlags::Save, "If > 0, caps the number of physics solves per frame and drops the un-simulated catch-up time. Prevents the low-FPS death spiral where a slow frame queues multiple full solves (very costly with soft bodies) and makes itself slower. Physics runs slightly slow instead of spiraling. 0 = use stepping settings.");

#ifdef JPH_DEBUG_RENDERER
plCVarBool cvar_JoltDebugDrawConstraints("Jolt.DebugDraw.Constraints", false, plCVarFlags::None, "Visualize physics constraints.");
plCVarBool cvar_JoltDebugDrawConstraintLimits("Jolt.DebugDraw.ConstraintLimits", false, plCVarFlags::None, "Visualize physics constraint limits.");
plCVarBool cvar_JoltDebugDrawConstraintFrames("Jolt.DebugDraw.ConstraintFrames", false, plCVarFlags::None, "Visualize physics constraint frames.");
plCVarBool cvar_JoltDebugDrawBodies("Jolt.DebugDraw.Bodies", false, plCVarFlags::None, "Visualize physics bodies.");
#endif

plCVarBool cvar_JoltVisualizeGeometry("Jolt.Visualize.Geometry", false, plCVarFlags::None, "Renders collision geometry.");
plCVarBool cvar_JoltVisualizeGeometryExclusive("Jolt.Visualize.Exclusive", false, plCVarFlags::Save, "Hides regularly rendered geometry.");
plCVarFloat cvar_JoltVisualizeDistance("Jolt.Visualize.Distance", 30.0f, plCVarFlags::Save, "How far away objects to visualize.");

plJoltWorldModule::plJoltWorldModule(plWorld* pWorld)
  : plPhysicsWorldModuleInterface(pWorld)
//, m_FreeObjectFilterIDs(plJolt::GetSingleton()->GetAllocator()) // could use a proxy allocator to bin those
{
  m_pSimulateTask = PL_DEFAULT_NEW(plDelegateTask<void>, "Jolt::Simulate", plTaskNesting::Never, plMakeDelegate(&plJoltWorldModule::Simulate, this));
  m_pSimulateTask->ConfigureTask("Jolt Simulate", plTaskNesting::Maybe);
}

plJoltWorldModule::~plJoltWorldModule() = default;

class plJoltBodyActivationListener : public JPH::BodyActivationListener
{
public:
  virtual void OnBodyActivated(const JPH::BodyID& bodyID, JPH::uint64 inBodyUserData) override
  {
    // Jolt calls activation callbacks from ANY of its solver job threads, potentially several at the same time
    // (see JPH::BodyActivationListener docs: "make sure your code is thread safe"). The containers below are trees /
    // arrays with no internal synchronization - concurrent mutation corrupts them and crashes later when the world
    // update iterates them (e.g. plJoltBreakableSlabComponentManager::PostAsyncUpdate over m_ActiveSlabs).
    // Readers are safe without this lock: they only run after FetchResults joined the simulate task.
    PL_LOCK(m_Mutex);

    const plJoltUserData* pUserData = reinterpret_cast<const plJoltUserData*>(inBodyUserData);

    switch (plJoltUserData::GetType(pUserData))
    {
      case plJoltUserData::Type::DynamicActorComponent:
        m_pActiveActors->Insert(static_cast<plJoltDynamicActorComponent*>(pUserData->GetObject()));
        return;

      case plJoltUserData::Type::RagdollComponent:
        (*m_pActiveRagdolls)[static_cast<plJoltRagdollComponent*>(pUserData->GetObject())]++;
        return;

      case plJoltUserData::Type::RopeComponent:
        (*m_pActiveRopes)[static_cast<plJoltRopeComponent*>(pUserData->GetObject())]++;
        return;

      case plJoltUserData::Type::BreakableSlabComponent:
        (*m_pActiveSlabs)[static_cast<plJoltBreakableSlabComponent*>(pUserData->GetObject())]++;
        return;

      case plJoltUserData::Type::DestructibleComponent:
        (*m_pActiveDestructibles)[static_cast<plJoltDestructibleComponent*>(pUserData->GetObject())]++;
        return;

      default:
        return;
    }
  }

  virtual void OnBodyDeactivated(const JPH::BodyID& bodyID, JPH::uint64 inBodyUserData) override
  {
    // See OnBodyActivated: concurrent invocation from Jolt job threads requires synchronization.
    PL_LOCK(m_Mutex);

    const plJoltUserData* pUserData = reinterpret_cast<const plJoltUserData*>(inBodyUserData);

    switch (plJoltUserData::GetType(pUserData))
    {
      case plJoltUserData::Type::DynamicActorComponent:
      {
        m_pActiveActors->Remove(static_cast<plJoltDynamicActorComponent*>(pUserData->GetObject()));

        return;
      }

      case plJoltUserData::Type::RagdollComponent:
      {
        plJoltRagdollComponent* pActor = static_cast<plJoltRagdollComponent*>(pUserData->GetObject());
        if (--(*m_pActiveRagdolls)[pActor] == 0)
        {
          m_pActiveRagdolls->Remove(pActor);
          m_pRagdollsPutToSleep->PushBack(pActor->GetHandle());
        }

        return;
      }

      case plJoltUserData::Type::RopeComponent:
      {
        plJoltRopeComponent* pActor = static_cast<plJoltRopeComponent*>(pUserData->GetObject());
        if (--(*m_pActiveRopes)[pActor] == 0)
        {
          m_pActiveRopes->Remove(pActor);
        }

        return;
      }

      case plJoltUserData::Type::BreakableSlabComponent:
      {
        plJoltBreakableSlabComponent* pActor = static_cast<plJoltBreakableSlabComponent*>(pUserData->GetObject());
        if (--(*m_pActiveSlabs)[pActor] == 0)
        {
          m_pActiveSlabs->Remove(pActor);
          m_pSlabsPutToSleep->PushBack(pActor->GetHandle());
        }

        return;
      }

      case plJoltUserData::Type::DestructibleComponent:
      {
        plJoltDestructibleComponent* pActor = static_cast<plJoltDestructibleComponent*>(pUserData->GetObject());
        if (--(*m_pActiveDestructibles)[pActor] == 0)
        {
          m_pActiveDestructibles->Remove(pActor);
          m_pDestructiblesPutToSleep->PushBack(pActor->GetHandle());
        }

        return;
      }

      default:
        return;
    }
  }

  plMutex m_Mutex; ///< Serializes concurrent activation callbacks from Jolt's job threads.
  plSet<plJoltDynamicActorComponent*>* m_pActiveActors = nullptr;
  plMap<plJoltRopeComponent*, plInt32>* m_pActiveRopes = nullptr;          // value is a ref-count
  plMap<plJoltRagdollComponent*, plInt32>* m_pActiveRagdolls = nullptr;    // value is a ref-count
  plDynamicArray<plComponentHandle>* m_pRagdollsPutToSleep = nullptr;
  plMap<plJoltBreakableSlabComponent*, plInt32>* m_pActiveSlabs = nullptr; // value is a ref-count
  plDynamicArray<plComponentHandle>* m_pSlabsPutToSleep = nullptr;
  plMap<plJoltDestructibleComponent*, plInt32>* m_pActiveDestructibles = nullptr; // value is a ref-count
  plDynamicArray<plComponentHandle>* m_pDestructiblesPutToSleep = nullptr;
};

class plJoltGroupFilter : public JPH::GroupFilter
{
public:
  virtual bool CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const override
  {
    const plUInt64 id = static_cast<plUInt64>(group1.GetGroupID()) << 32 | group2.GetGroupID();

    return !m_IgnoreCollisions.Contains(id);
  }

  plHashSet<plUInt64> m_IgnoreCollisions;
};

class plJoltGroupFilterIgnoreSame : public JPH::GroupFilter
{
public:
  virtual bool CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const override
  {
    return group1.GetGroupID() != group2.GetGroupID();
  }
};

void plJoltWorldModule::Deinitialize()
{
  m_HeightfieldShapeCache.Clear(); // JPH::Ref in each entry releases the shape automatically

  m_pSystem = nullptr;
  m_pTempAllocator = nullptr;

  plJoltBodyActivationListener* pActivationListener = reinterpret_cast<plJoltBodyActivationListener*>(m_pActivationListener);
  PL_DEFAULT_DELETE(pActivationListener);
  m_pActivationListener = nullptr;

  plJoltContactListener* pContactListener = reinterpret_cast<plJoltContactListener*>(m_pContactListener);
  PL_DEFAULT_DELETE(pContactListener);
  m_pContactListener = nullptr;

  plJoltSoftBodyContactListener* pSoftBodyContactListener = reinterpret_cast<plJoltSoftBodyContactListener*>(m_pSoftBodyContactListener);
  PL_DEFAULT_DELETE(pSoftBodyContactListener);
  m_pSoftBodyContactListener = nullptr;

  m_pGroupFilter->Release();
  m_pGroupFilter = nullptr;

  m_pGroupFilterIgnoreSame->Release();
  m_pGroupFilterIgnoreSame = nullptr;
}


void plJoltWorldModule::Initialize()
{
  // TODO: it would be better if this were in OnSimulationStarted() to guarantee that the system is always initialized with the latest values
  // however, that doesn't work because plJoltWorldModule is only created by calls to GetOrCreateWorldModule, where Initialize is called, but OnSimulationStarted
  // is queued and executed later

  // ensure the first element is reserved for 'invalid' objects
  m_AllocatedUserData.SetCount(1);
  m_UserDataRefCounts.SetCount(1, 0);

  UpdateSettingsCfg();

  plStringBuilder tmp("Jolt-", GetWorld()->GetName());
  m_pTempAllocator = std::make_unique<plJoltTempAlloc>(tmp);

  const uint32_t cMaxBodies = m_Settings.m_uiMaxBodies;
  const uint32_t cMaxContactConstraints = m_Settings.m_uiMaxBodies * 4;
  const uint32_t cMaxBodyPairs = cMaxContactConstraints * 10;
  const uint32_t cNumBodyMutexes = 0;

  m_pSystem = std::make_unique<JPH::PhysicsSystem>();
  m_pSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, m_ObjectToBroadphase, m_ObjectVsBroadphaseFilter, m_ObjectLayerPairFilter);

  {
    plJoltBodyActivationListener* pListener = PL_DEFAULT_NEW(plJoltBodyActivationListener);
    m_pActivationListener = pListener;
    pListener->m_pActiveActors = &m_ActiveActors;
    pListener->m_pActiveRopes = &m_ActiveRopes;
    pListener->m_pActiveRagdolls = &m_ActiveRagdolls;
    pListener->m_pRagdollsPutToSleep = &m_RagdollsPutToSleep;
    pListener->m_pActiveSlabs = &m_ActiveSlabs;
    pListener->m_pSlabsPutToSleep = &m_SlabsPutToSleep;
    pListener->m_pActiveDestructibles = &m_ActiveDestructibles;
    pListener->m_pDestructiblesPutToSleep = &m_DestructiblesPutToSleep;
    m_pSystem->SetBodyActivationListener(pListener);
  }

  {
    plJoltContactListener* pListener = PL_DEFAULT_NEW(plJoltContactListener);
    pListener->m_pWorld = GetWorld();
    m_pContactListener = pListener;
    m_pSystem->SetContactListener(pListener);
  }

  {
    plJoltSoftBodyContactListener* pListener = PL_DEFAULT_NEW(plJoltSoftBodyContactListener);
    m_pSoftBodyContactListener = pListener;
    m_pSystem->SetSoftBodyContactListener(pListener);
  }

  {
    m_pGroupFilter = new plJoltGroupFilter();
    m_pGroupFilter->AddRef();
  }

  {
    m_pGroupFilterIgnoreSame = new plJoltGroupFilterIgnoreSame();
    m_pGroupFilterIgnoreSame->AddRef();
  }
}

void plJoltWorldModule::OnSimulationStarted()
{
  {
    auto startSimDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltWorldModule::StartSimulation, this);
    startSimDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PreAsync;
    startSimDesc.m_bOnlyUpdateWhenSimulating = true;
    // Start physics simulation as late as possible in the first synchronous phase
    // so all kinematic objects have a chance to update their transform before.
    startSimDesc.m_fPriority = -100000.0f;

    RegisterUpdateFunction(startSimDesc);
  }

  {
    auto fetchResultsDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltWorldModule::FetchResults, this);
    fetchResultsDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostAsync;
    fetchResultsDesc.m_bOnlyUpdateWhenSimulating = true;
    // Fetch results as early as possible after async phase.
    fetchResultsDesc.m_fPriority = 100000.0f;

    RegisterUpdateFunction(fetchResultsDesc);
  }

  plJoltCore::ReloadConfigs();

  UpdateSettingsCfg();
  ApplySettingsCfg();

  m_AccumulatedTimeSinceUpdate = plTime::MakeZero();
}

plUInt32 plJoltWorldModule::CreateObjectFilterID()
{
  if (!m_FreeObjectFilterIDs.IsEmpty())
  {
    plUInt32 uiObjectFilterID = m_FreeObjectFilterIDs.PeekBack();
    m_FreeObjectFilterIDs.PopBack();

    return uiObjectFilterID;
  }

  return m_uiNextObjectFilterID++;
}

void plJoltWorldModule::DeleteObjectFilterID(plUInt32& ref_uiObjectFilterID)
{
  if (ref_uiObjectFilterID == plInvalidIndex)
    return;

  m_FreeObjectFilterIDs.PushBack(ref_uiObjectFilterID);

  ref_uiObjectFilterID = plInvalidIndex;
}

plUInt32 plJoltWorldModule::AllocateUserData(plJoltUserData*& out_pUserData)
{
  if (!m_FreeUserData.IsEmpty())
  {
    plUInt32 uiIndex = m_FreeUserData.PeekBack();
    m_FreeUserData.PopBack();

    m_UserDataRefCounts[uiIndex] = 1;
    out_pUserData = &m_AllocatedUserData[uiIndex];
    return uiIndex;
  }

  out_pUserData = &m_AllocatedUserData.ExpandAndGetRef();
  m_UserDataRefCounts.PushBack(1);
  return m_AllocatedUserData.GetCount() - 1;
}

void plJoltWorldModule::DeallocateUserData(plUInt32& ref_uiUserDataId)
{
  if (ref_uiUserDataId == plInvalidIndex)
    return;

  m_AllocatedUserData[ref_uiUserDataId].Invalidate();

  ReleaseUserData(ref_uiUserDataId);

  ref_uiUserDataId = plInvalidIndex;
}

void plJoltWorldModule::RetainUserData(plUInt32 uiUserDataId)
{
  PL_ASSERT_DEV(m_UserDataRefCounts[uiUserDataId] > 0, "Cannot retain retired physics user data.");
  ++m_UserDataRefCounts[uiUserDataId];
}

void plJoltWorldModule::ReleaseUserData(plUInt32 uiUserDataId)
{
  PL_ASSERT_DEV(m_UserDataRefCounts[uiUserDataId] > 0, "Physics user data reference underflow.");
  if (--m_UserDataRefCounts[uiUserDataId] == 0)
    m_FreeUserDataAfterSimulationStep.PushBack(uiUserDataId);
}

const plJoltUserData& plJoltWorldModule::GetUserData(plUInt32 uiUserDataId) const
{
  PL_ASSERT_DEBUG(uiUserDataId != plInvalidIndex, "Invalid plJoltUserData ID");

  return m_AllocatedUserData[uiUserDataId];
}

void plJoltWorldModule::SetGravity(const plVec3& vObjectGravity, const plVec3& vCharacterGravity)
{
  m_Settings.m_vObjectGravity = vObjectGravity;
  m_Settings.m_vCharacterGravity = vCharacterGravity;

  if (m_pSystem)
  {
    m_pSystem->SetGravity(plJoltConversionUtils::ToVec3(m_Settings.m_vObjectGravity));
  }
}

plJoltForceId plJoltWorldModule::AddOrUpdateForce(plJoltForceId forceId, plUInt32 uiBodyID, plTime duration, const plVec3& vForce)
{
  PL_LOCK(m_ForcesMutex);

  plJoltForce* pForce;
  if (m_Forces.TryGetValue(forceId, pForce))
  {
    pForce->m_tDisable = GetWorld()->GetClock().GetAccumulatedTime() + duration;
    pForce->m_vForce = vForce;
    return forceId;
  }
  else
  {
    plJoltForce force;
    force.m_uiBodyID = uiBodyID;
    force.m_tDisable = GetWorld()->GetClock().GetAccumulatedTime() + duration;
    force.m_vForce = vForce;

    return m_Forces.Insert(force);
  }
}

void plJoltWorldModule::ClearForce(plJoltForceId id)
{
  PL_LOCK(m_ForcesMutex);
  m_Forces.Remove(id);
}

void plJoltWorldModule::UpdateForces()
{
  if (m_Forces.IsEmpty())
    return;

  PL_LOCK(m_ForcesMutex);

  plHybridArray<plJoltForceId, 32> forcesToRemove;

  auto* pBodies = &m_pSystem->GetBodyInterface();
  const plTime tNow = GetWorld()->GetClock().GetAccumulatedTime();

  for (auto it = m_Forces.GetIterator(); it.IsValid(); ++it)
  {
    plJoltForce& force = it.Value();

    if (force.m_tDisable < tNow)
    {
      forcesToRemove.PushBack(it.Id());
      continue;
    }

    const JPH::BodyID bodyId(force.m_uiBodyID);

    if (!pBodies->IsAdded(bodyId))
      continue;

    pBodies->AddForce(bodyId, plJoltConversionUtils::ToVec3(force.m_vForce));
  }

  for (plJoltForceId id : forcesToRemove)
  {
    m_Forces.Remove(id);
  }
}

plUInt32 plJoltWorldModule::GetCollisionLayerByName(plStringView sName) const
{
  return plJoltCore::GetCollisionFilterConfig().GetFilterGroupByName(sName);
}

plUInt8 plJoltWorldModule::GetWeightCategoryByName(plStringView sName) const
{
  if (sName.IsEmpty())
  {
    return 0;
  }
  return plJoltCore::GetWeightCategoryConfig().FindByName(plTempHashedString(sName));
}

plUInt8 plJoltWorldModule::GetImpulseTypeByName(plStringView sName) const
{
  return plJoltCore::GetImpulseTypeConfig().FindByName(plTempHashedString(sName));
}

void plJoltWorldModule::AddStaticCollisionBox(plGameObject* pObject, plVec3 vBoxSize)
{
  plJoltStaticActorComponent* pActor = nullptr;
  plJoltStaticActorComponent::CreateComponent(pObject, pActor);

  plJoltShapeBoxComponent* pBox;
  plJoltShapeBoxComponent::CreateComponent(pObject, pBox);
  pBox->SetHalfExtents(vBoxSize * 0.5f);
}

void plJoltWorldModule::AddFixedJointComponent(plGameObject* pOwner, const plPhysicsWorldModuleInterface::FixedJointConfig& cfg)
{
  plJoltFixedConstraintComponent* pConstraint = nullptr;
  m_pWorld->GetOrCreateComponentManager<plJoltFixedConstraintComponentManager>()->CreateComponent(pOwner, pConstraint);
  pConstraint->SetActors(cfg.m_hActorA, cfg.m_LocalFrameA, cfg.m_hActorB, cfg.m_LocalFrameB);
}

plBoundingBoxSphere plJoltWorldModule::GetWorldSpaceBounds(plGameObject* pOwner, plUInt32 uiCollisionLayer, plBitflags<plPhysicsShapeType> shapeTypes, bool bIncludeChildObjects) const
{
  plBoundingBoxSphere result = plBoundingBoxSphere::MakeInvalid();

  plJoltActorComponent* pActor = nullptr;
  if (pOwner->TryGetComponentOfBaseType(pActor))
  {
    plUInt32 uiBodyID = pActor->GetJoltBodyID();
    auto& lockInterface = m_pSystem->GetBodyLockInterfaceNoLock();
    JPH::BodyLockRead bodyLock(lockInterface, JPH::BodyID(uiBodyID));
    if (bodyLock.Succeeded())
    {
      const auto& body = bodyLock.GetBody();

      if ((shapeTypes.GetValue() & PL_BIT(body.GetBroadPhaseLayer().GetValue())) != 0 && plJoltObjectLayerFilter(uiCollisionLayer).ShouldCollide(body.GetObjectLayer()))
      {
        const auto& aabb = body.GetWorldSpaceBounds();
        result = plBoundingBoxSphere::MakeFromBox(plBoundingBox::MakeFromMinMax(plJoltConversionUtils::ToVec3(aabb.mMin), plJoltConversionUtils::ToVec3(aabb.mMax)));
      }
    }
  }

  if (bIncludeChildObjects)
  {
    for (auto it = pOwner->GetChildren(); it.IsValid(); it.Next())
    {
      plBoundingBoxSphere childBounds = GetWorldSpaceBounds(it, uiCollisionLayer, shapeTypes, bIncludeChildObjects);
      if (!childBounds.IsValid())
        continue;

      if (result.IsValid())
      {
        result.ExpandToInclude(childBounds);
      }
      else
      {
        result = childBounds;
      }
    }
  }

  return result;
}

void plJoltWorldModule::QueueBodyToAdd(JPH::Body* pBody, bool bAwake)
{
  if (bAwake)
    m_BodiesToAddAndActivate.PushBack(pBody->GetID().GetIndexAndSequenceNumber());
  else
    m_BodiesToAdd.PushBack(pBody->GetID().GetIndexAndSequenceNumber());
}

void plJoltWorldModule::RemoveBodyFromQueue(JPH::BodyID bodyId)
{
  const plUInt32 uiBodyID = bodyId.GetIndexAndSequenceNumber();
  bool bRemoved = m_BodiesToAdd.RemoveAndSwap(uiBodyID);
  bRemoved |= m_BodiesToAddAndActivate.RemoveAndSwap(uiBodyID);
  PL_ASSERT_DEV(bRemoved, "Body was not in add queue");
}

void plJoltWorldModule::EnableJoinedBodiesCollisions(plUInt32 uiObjectFilterID1, plUInt32 uiObjectFilterID2, bool bEnable)
{
  plJoltGroupFilter* pFilter = static_cast<plJoltGroupFilter*>(m_pGroupFilter);

  const plUInt64 uiMask1 = static_cast<plUInt64>(uiObjectFilterID1) << 32 | uiObjectFilterID2;
  const plUInt64 uiMask2 = static_cast<plUInt64>(uiObjectFilterID2) << 32 | uiObjectFilterID1;

  if (bEnable)
  {
    pFilter->m_IgnoreCollisions.Remove(uiMask1);
    pFilter->m_IgnoreCollisions.Remove(uiMask2);
  }
  else
  {
    pFilter->m_IgnoreCollisions.Insert(uiMask1);
    pFilter->m_IgnoreCollisions.Insert(uiMask2);
  }
}

void plJoltWorldModule::ActivateCharacterController(plJoltCharacterControllerComponent* pCharacter, bool bActivate)
{
  if (bActivate)
  {
    PL_ASSERT_DEBUG(!m_ActiveCharacters.Contains(pCharacter), "plJoltCharacterControllerComponent was activated more than once.");

    m_ActiveCharacters.PushBack(pCharacter);
  }
  else
  {
    if (!m_ActiveCharacters.RemoveAndSwap(pCharacter))
    {
      PL_ASSERT_DEBUG(false, "plJoltCharacterControllerComponent was deactivated more than once.");
    }
  }
}

void plJoltWorldModule::CheckBreakableConstraints()
{
  plWorld* pWorld = GetWorld();

  // Notifications can delete this or another constraint. Never retain a set iterator
  // or component pointer across gameplay callbacks.
  plHybridArray<plComponentHandle, 32> constraints;
  for (const auto& hConstraint : m_BreakableConstraints)
    constraints.PushBack(hConstraint);

  for (const auto& hConstraint : constraints)
  {
    plJoltConstraintComponent* pConstraint;
    if (pWorld->TryGetComponent(hConstraint, pConstraint) && pConstraint->IsActive() && pConstraint->m_pConstraint != nullptr)
    {
      if (pConstraint->ExceededBreakingPoint())
      {
        // notify interested parties, that this constraint is now broken
        plMsgPhysicsJointBroke msg;
        msg.m_hJointObject = pConstraint->GetOwner()->GetHandle();
        pConstraint->GetOwner()->SendEventMessage(msg, pConstraint);

        // currently we don't track the broken state separately, we just remove the component
        if (pWorld->TryGetComponent(hConstraint, pConstraint))
          pConstraint->DeleteComponent();
        m_BreakableConstraints.Remove(hConstraint);
      }
    }
    else
    {
      m_BreakableConstraints.Remove(hConstraint);
    }
  }
}

void plJoltWorldModule::FreeUserDataAfterSimulationStep()
{
  m_FreeUserData.PushBackRange(m_FreeUserDataAfterSimulationStep);
  m_FreeUserDataAfterSimulationStep.Clear();
}

void plJoltWorldModule::ValidateBodies(const char* szWhen)
{
  JPH::BodyIDVector bodyIds;
  m_pSystem->GetBodies(bodyIds);

  for (const JPH::BodyID id : bodyIds)
  {
    JPH::BodyLockRead lock(m_pSystem->GetBodyLockInterface(), id);

    if (!lock.Succeeded())
      continue;

    const JPH::Body& body = lock.GetBody();

    const JPH::GroupFilter* pFilter = body.GetCollisionGroup().GetGroupFilter();
    const plJoltUserData::Type userType = plJoltUserData::GetType(reinterpret_cast<const void*>(body.GetUserData()));

    plStringBuilder sError;

    if (pFilter != nullptr && pFilter != m_pGroupFilter && pFilter != m_pGroupFilterIgnoreSame)
    {
      sError.SetFormat("[{}] Jolt body {} has an unknown group filter {} (user data type {})", szWhen, id.GetIndexAndSequenceNumber(), plArgP(pFilter), (int)userType);
    }
    else if (body.GetPosition().IsNaN() || body.GetRotation().IsNaN())
    {
      sError.SetFormat("[{}] Jolt body {} has a NaN transform (user data type {})", szWhen, id.GetIndexAndSequenceNumber(), (int)userType);
    }
    else
    {
      const JPH::AABox& bounds = body.GetWorldSpaceBounds();
      if (bounds.mMin.IsNaN() || bounds.mMax.IsNaN())
      {
        sError.SetFormat("[{}] Jolt body {} has NaN bounds (user data type {})", szWhen, id.GetIndexAndSequenceNumber(), (int)userType);
      }
    }

    if (!sError.IsEmpty())
    {
      plLog::Error("{}", sError);
    }
  }
}

void plJoltWorldModule::StartSimulation(const plWorldModule::UpdateContext& context)
{
  m_RagdollsPutToSleep.Clear();
  m_SlabsPutToSleep.Clear();
  m_DestructiblesPutToSleep.Clear();
  m_UpdateSteps.Clear();

  if (cvar_JoltSimulationPause)
    return;

  if (cvar_JoltValidateBodies)
  {
    ValidateBodies("StartSimulation");
  }

  if (!m_BodiesToAdd.IsEmpty())
  {
    m_uiBodiesAddedSinceOptimize += m_BodiesToAdd.GetCount();

    static_assert(sizeof(JPH::BodyID) == sizeof(plUInt32));

    plUInt32 uiStartIdx = 0;

    while (uiStartIdx < m_BodiesToAdd.GetCount())
    {
      const plUInt32 uiCount = m_BodiesToAdd.GetContiguousRange(uiStartIdx);

      JPH::BodyID* pIDs = reinterpret_cast<JPH::BodyID*>(&m_BodiesToAdd[uiStartIdx]);

      void* pHandle = m_pSystem->GetBodyInterface().AddBodiesPrepare(pIDs, uiCount);
      m_pSystem->GetBodyInterface().AddBodiesFinalize(pIDs, uiCount, pHandle, JPH::EActivation::DontActivate);

      uiStartIdx += uiCount;
    }

    m_BodiesToAdd.Clear();
  }

  if (!m_BodiesToAddAndActivate.IsEmpty())
  {
    m_uiBodiesAddedSinceOptimize += m_BodiesToAddAndActivate.GetCount();

    static_assert(sizeof(JPH::BodyID) == sizeof(plUInt32));

    plUInt32 uiStartIdx = 0;

    while (uiStartIdx < m_BodiesToAddAndActivate.GetCount())
    {
      const plUInt32 uiCount = m_BodiesToAddAndActivate.GetContiguousRange(uiStartIdx);

      JPH::BodyID* pIDs = reinterpret_cast<JPH::BodyID*>(&m_BodiesToAddAndActivate[uiStartIdx]);

      void* pHandle = m_pSystem->GetBodyInterface().AddBodiesPrepare(pIDs, uiCount);
      m_pSystem->GetBodyInterface().AddBodiesFinalize(pIDs, uiCount, pHandle, JPH::EActivation::Activate);

      uiStartIdx += uiCount;
    }

    m_BodiesToAddAndActivate.Clear();
  }

  if (m_uiBodiesAddedSinceOptimize > 128)
  {
    // TODO: not clear whether this could be multi-threaded or done more efficiently somehow
    m_pSystem->OptimizeBroadPhase();
    m_uiBodiesAddedSinceOptimize = 0;
  }

  UpdateSettingsCfg();

  m_SimulatedTimeStep = CalculateUpdateSteps();

  if (m_UpdateSteps.IsEmpty())
    return;

  if (plJoltDynamicActorComponentManager* pDynamicActorManager = GetWorld()->GetComponentManager<plJoltDynamicActorComponentManager>())
  {
    pDynamicActorManager->UpdateKinematicActors(m_SimulatedTimeStep);
  }

  if (plJoltQueryShapeActorComponentManager* pQueryShapesManager = GetWorld()->GetComponentManager<plJoltQueryShapeActorComponentManager>())
  {
    pQueryShapesManager->UpdateMovingQueryShapes();
  }

  if (plJoltDestructibleComponentManager* pDestructibleManager = GetWorld()->GetComponentManager<plJoltDestructibleComponentManager>())
  {
    pDestructibleManager->UpdateMovingDestructibles(m_SimulatedTimeStep);
  }

  if (plJoltTriggerComponentManager* pTriggerManager = GetWorld()->GetComponentManager<plJoltTriggerComponentManager>())
  {
    pTriggerManager->UpdateMovingTriggers();
  }

  if (plJoltRagdollComponentManager* pRagdollManager = GetWorld()->GetComponentManager<plJoltRagdollComponentManager>())
  {
    pRagdollManager->DriveAnimatedRagdolls(m_SimulatedTimeStep);
  }

  ApplyImpulses();
  UpdateForces();
  UpdateConstraints();

  m_SimulateTaskGroupId = plTaskSystem::StartSingleTask(m_pSimulateTask, plTaskPriority::EarlyThisFrame);
  m_bSimulateTaskStarted = true;
}

void plJoltWorldModule::FetchResults(const plWorldModule::UpdateContext& context)
{
  PL_PROFILE_SCOPE("plJoltWorldModule::FetchResults");

  // Only wait when StartSimulation actually started a simulate task THIS frame. With fixed-step physics at high
  // framerates, many frames have no update step - the group id would be stale and WaitForGroup would still pay
  // task-system overhead and may 'help execute' unrelated tasks while at it.
  if (m_bSimulateTaskStarted)
  {
    PL_PROFILE_SCOPE("Wait for Simulate Task");
    plTaskSystem::WaitForGroup(m_SimulateTaskGroupId);

    if (cvar_JoltValidateBodies)
    {
      ValidateBodies("FetchResults");
    }
    m_bSimulateTaskStarted = false;
  }

#ifdef JPH_DEBUG_RENDERER
  if (cvar_JoltDebugDrawConstraints)
    m_pSystem->DrawConstraints(plJoltCore::s_pDebugRenderer.get());

  if (cvar_JoltDebugDrawConstraintLimits)
    m_pSystem->DrawConstraintLimits(plJoltCore::s_pDebugRenderer.get());

  if (cvar_JoltDebugDrawConstraintFrames)
    m_pSystem->DrawConstraintReferenceFrame(plJoltCore::s_pDebugRenderer.get());

  if (cvar_JoltDebugDrawBodies)
  {
    JPH::BodyManager::DrawSettings opt;
    opt.mDrawShape = true;
    opt.mDrawShapeWireframe = true;
    m_pSystem->DrawBodies(opt, plJoltCore::s_pDebugRenderer.get());
  }

  plJoltCore::DebugDraw(GetWorld());
#endif

  // Nothing to fetch if no simulation step was executed
  if (m_UpdateSteps.IsEmpty())
    return;

  ++m_uiJoltUpdateCounter;

  if (plJoltDynamicActorComponentManager* pDynamicActorManager = GetWorld()->GetComponentManager<plJoltDynamicActorComponentManager>())
  {
    pDynamicActorManager->UpdateDynamicActors();
  }

  for (auto pCharacter : m_ActiveCharacters)
  {
    pCharacter->Update(m_SimulatedTimeStep);
  }

  if (plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
  {
    reinterpret_cast<plJoltContactListener*>(m_pContactListener)->m_ContactEvents.m_vMainCameraPosition = pView->GetCamera()->GetPosition();
  }

  reinterpret_cast<plJoltContactListener*>(m_pContactListener)->m_ContactEvents.SpawnPhysicsImpactReactions();
  reinterpret_cast<plJoltContactListener*>(m_pContactListener)->m_ContactEvents.UpdatePhysicsSlideReactions();
  reinterpret_cast<plJoltContactListener*>(m_pContactListener)->m_ContactEvents.UpdatePhysicsRollReactions();

  reinterpret_cast<plJoltContactListener*>(m_pContactListener)->SendContactMessages();

  CheckBreakableConstraints();

  FreeUserDataAfterSimulationStep();

  DebugDrawGeometry();
}

plTime plJoltWorldModule::CalculateUpdateSteps()
{
  plTime tSimulatedTimeStep = plTime::MakeZero();
  m_AccumulatedTimeSinceUpdate += GetWorld()->GetClock().GetTimeDiff();
  m_UpdateSteps.Clear();

  if (m_Settings.m_SteppingMode == plJoltSteppingMode::Variable)
  {
    // always do a single step with the entire time
    m_UpdateSteps.PushBack(m_AccumulatedTimeSinceUpdate);

    tSimulatedTimeStep = m_AccumulatedTimeSinceUpdate;
    m_AccumulatedTimeSinceUpdate = plTime::MakeZero();
  }
  else if (m_Settings.m_SteppingMode == plJoltSteppingMode::Fixed)
  {
    const plTime tFixedStep = plTime::MakeFromSeconds(1.0 / m_Settings.m_fFixedFrameRate);

    plUInt32 uiNumSubSteps = 0;

    while (m_AccumulatedTimeSinceUpdate >= tFixedStep && uiNumSubSteps < m_Settings.m_uiMaxSubSteps)
    {
      m_UpdateSteps.PushBack(tFixedStep);
      ++uiNumSubSteps;

      tSimulatedTimeStep += tFixedStep;
      m_AccumulatedTimeSinceUpdate -= tFixedStep;
    }
  }
  else if (m_Settings.m_SteppingMode == plJoltSteppingMode::SemiFixed)
  {
    plTime tFixedStep = plTime::MakeFromSeconds(1.0 / m_Settings.m_fFixedFrameRate);
    const plTime tMinStep = tFixedStep * 0.25;

    if (tFixedStep * m_Settings.m_uiMaxSubSteps < m_AccumulatedTimeSinceUpdate) // in case too much time has passed
    {
      // if taking N steps isn't sufficient to catch up to the passed time, increase the fixed time step accordingly
      tFixedStep = m_AccumulatedTimeSinceUpdate / (double)m_Settings.m_uiMaxSubSteps;
    }

    while (m_AccumulatedTimeSinceUpdate > tMinStep)
    {
      // prefer fixed time steps
      // but if at the end there is still more than tMinStep time left, do another step with the remaining time
      const plTime tDeltaTime = plMath::Min(tFixedStep, m_AccumulatedTimeSinceUpdate);

      m_UpdateSteps.PushBack(tDeltaTime);

      tSimulatedTimeStep += tDeltaTime;
      m_AccumulatedTimeSinceUpdate -= tDeltaTime;
    }
  }

  // Anti-death-spiral cap: at low frame rates the Fixed/SemiFixed stepping queues multiple full solves per frame, which
  // makes a slow frame even slower (soft bodies are especially expensive). When capped, keep only the first N solves and
  // drop both the extra steps and the leftover catch-up time (physics runs slightly slow instead of spiraling).
  const plInt32 iMaxSteps = cvar_JoltMaxUpdateSteps.GetValue();
  if (iMaxSteps > 0 && m_UpdateSteps.GetCount() > static_cast<plUInt32>(iMaxSteps))
  {
    m_UpdateSteps.SetCount(static_cast<plUInt32>(iMaxSteps));

    tSimulatedTimeStep = plTime::MakeZero();
    for (plTime tStep : m_UpdateSteps)
      tSimulatedTimeStep += tStep;

    // Drop the un-simulated debt so it does not accumulate into even more steps next frame (that is the spiral).
    m_AccumulatedTimeSinceUpdate = plTime::MakeZero();
  }

  return tSimulatedTimeStep;
}

void plJoltWorldModule::Simulate()
{
  if (m_UpdateSteps.IsEmpty())
    return;

  PL_PROFILE_SCOPE("plJoltWorldModule::Simulate");

  plTime tDelta = m_UpdateSteps[0];
  plUInt32 uiSteps = 1;

  for (plUInt32 i = 1; i < m_UpdateSteps.GetCount(); ++i)
  {
    PL_PROFILE_SCOPE("Physics Sim Step");

    if (m_UpdateSteps[i] == tDelta)
    {
      ++uiSteps;
    }
    else
    {
      // do a single Update call with multiple sub-steps, if possible
      // this saves a bit of time compared to just doing multiple Update calls

      m_pSystem->Update((uiSteps * tDelta).AsFloatInSeconds(), uiSteps, m_pTempAllocator.get(), plJoltCore::GetJoltJobSystem());

      tDelta = m_UpdateSteps[i];
      uiSteps = 1;
    }
  }

  m_pSystem->Update((uiSteps * tDelta).AsFloatInSeconds(), uiSteps, m_pTempAllocator.get(), plJoltCore::GetJoltJobSystem());
}

void plJoltWorldModule::UpdateSettingsCfg()
{
  if (plJoltSettingsComponentManager* pSettingsManager = GetWorld()->GetComponentManager<plJoltSettingsComponentManager>())
  {
    plJoltSettingsComponent* pSettings = pSettingsManager->GetSingletonComponent();

    if (pSettings != nullptr && pSettings->IsModified())
    {
      m_Settings = pSettings->GetSettings();
      pSettings->ResetModified();

      ApplySettingsCfg();
    }
  }
}

void plJoltWorldModule::ApplySettingsCfg()
{
  SetGravity(m_Settings.m_vObjectGravity, m_Settings.m_vCharacterGravity);

  if (m_pSystem)
  {
    auto physicsSettings = m_pSystem->GetPhysicsSettings();
    physicsSettings.mPointVelocitySleepThreshold = m_Settings.m_fSleepVelocityThreshold;

    m_pSystem->SetPhysicsSettings(physicsSettings);
  }
}

void plJoltWorldModule::UpdateConstraints()
{
  if (m_RequireUpdate.IsEmpty())
    return;

  plJoltConstraintComponent* pComponent;
  for (auto& hComponent : m_RequireUpdate)
  {
    if (this->m_pWorld->TryGetComponent(hComponent, pComponent) && pComponent->IsActiveAndSimulating() && pComponent->m_pConstraint != nullptr)
    {
      pComponent->ApplySettings();
    }
  }

  m_RequireUpdate.Clear();
}

plAtomicInteger32 s_iColMeshVisGeoCounter;

struct DebugVis
{
  const char* m_szMaterial = nullptr;
  plColor m_Color;
};

const char* szMatSolid = "{ e6367876-ddb5-4149-ba80-180af553d463 }";       // Data/Base/Materials/Common/PhysicsColliders.plMaterialAsset
const char* szMatTransparent = "{ ca43dda3-a28c-41fe-ae20-419182e56f87 }"; // Data/Base/Materials/Common/PhysicsCollidersTransparent.plMaterialAsset
const char* szMatTwoSided = "{ b03df0e4-98b7-49ba-8413-d981014a77be }";    // Data/Base/Materials/Common/PhysicsCollidersSoft.plMaterialAsset

static const DebugVis s_Vis[plPhysicsShapeType::Count][2] =
  {
    // Static
    {
      {szMatSolid, plColor::LightSkyBlue}, // non-kinematic
      {szMatSolid, plColor::Red}           // kinematic
    },

    // Dynamic
    {
      {szMatSolid, plColor::Gold},      // non-kinematic
      {szMatSolid, plColor::DodgerBlue} // kinematic
    },

    // Query
    {
      {szMatTransparent, plColor::GreenYellow.WithAlpha(0.5f)}, // non-kinematic
      {szMatTransparent, plColor::GreenYellow.WithAlpha(0.5f)}  // kinematic
    },

    // Trigger
    {
      {szMatTransparent, plColor::Purple.WithAlpha(0.3f)}, // non-kinematic
      {szMatTransparent, plColor::Purple.WithAlpha(0.3f)}  // kinematic
    },

    // Character
    {
      {szMatTransparent, plColor::DarkTurquoise.WithAlpha(0.5f)}, // non-kinematic
      {szMatTransparent, plColor::DarkTurquoise.WithAlpha(0.5f)}  // kinematic
    },

    // Ragdoll
    {
      {szMatSolid, plColor::DeepPink}, // non-kinematic
      {szMatSolid, plColor::DeepPink}  // kinematic
    },

    // Rope
    {
      {szMatSolid, plColor::MediumVioletRed}, // non-kinematic
      {szMatSolid, plColor::MediumVioletRed}  // kinematic
    },

    // Cloth
    {
      {szMatTwoSided, plColor::Crimson}, // non-kinematic
      {szMatTwoSided, plColor::Red}      // kinematic
    },

    // Debris
    {
      {szMatSolid, plColor::Crimson}, // non-kinematic
      {szMatSolid, plColor::Crimson}  // kinematic
    },
};

void plJoltWorldModule::DebugDrawGeometry()
{
  plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld());

  if (pView == nullptr)
    return;

  ++m_uiDebugGeoLastSeenCounter;

  const plTag& tag = plTagRegistry::GetGlobalRegistry().RegisterTag("PhysicsCollider");

  if (cvar_JoltVisualizeGeometry && cvar_JoltVisualizeGeometryExclusive)
  {
    // deactivate other geometry rendering
    pView->m_IncludeTags.Set(tag);
  }
  else
  {
    pView->m_IncludeTags.Remove(tag);
  }

  if (cvar_JoltVisualizeGeometry)
  {
    const plVec3 vCenterPos = pView->GetCamera()->GetCenterPosition();

    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Static, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Dynamic, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Query, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Ragdoll, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Trigger, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Rope, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Cloth, tag);
    DebugDrawGeometry(vCenterPos, cvar_JoltVisualizeDistance, plPhysicsShapeType::Debris, tag);
  }

  for (auto it = m_DebugDrawComponents.GetIterator(); it.IsValid();)
  {
    if (it.Value().m_uiLastSeenCounter < m_uiDebugGeoLastSeenCounter)
    {
      GetWorld()->DeleteObjectDelayed(it.Value().m_hObject);
      it = m_DebugDrawComponents.Remove(it);
    }
    else
    {
      ++it;
    }
  }

  for (auto it = m_DebugDrawShapeGeo.GetIterator(); it.IsValid();)
  {
    if (it.Value().m_uiLastSeenCounter < m_uiDebugGeoLastSeenCounter)
    {
      it = m_DebugDrawShapeGeo.Remove(it);
    }
    else
    {
      ++it;
    }
  }
}

void plJoltWorldModule::DebugDrawGeometry(const plVec3& vCenter, float fRadius, plPhysicsShapeType::Enum shapeType, const plTag& tag)
{
  const plVec3 vAabbMin = vCenter - plVec3(fRadius);
  const plVec3 vAabbMax = vCenter + plVec3(fRadius);

  JPH::AABox aabb;
  aabb.mMin = plJoltConversionUtils::ToVec3(vAabbMin);
  aabb.mMax = plJoltConversionUtils::ToVec3(vAabbMax);

  JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> collector;

  plJoltBroadPhaseLayerFilter broadphaseFilter(shapeType);
  JPH::ObjectLayerFilter objectFilterAll;
  JPH::BodyFilter bodyFilterAll;

  m_pSystem->GetNarrowPhaseQuery().CollectTransformedShapes(aabb, collector, broadphaseFilter, objectFilterAll, bodyFilterAll);

  auto* pBodies = &m_pSystem->GetBodyInterface();

  const int cMaxTriangles = 128;


  plStaticArray<plVec3, cMaxTriangles * 3> positionsTmp;
  positionsTmp.SetCountUninitialized(cMaxTriangles * 3);

  plStaticArray<const JPH::PhysicsMaterial*, cMaxTriangles> materialsTmp;
  materialsTmp.SetCountUninitialized(cMaxTriangles);

  plHybridArray<plVec3, cMaxTriangles * 3> positionsTmp2;
  plHybridArray<const JPH::PhysicsMaterial*, cMaxTriangles> materialsTmp2;

  for (const JPH::TransformedShape& ts : collector.mHits)
  {
    DebugBodyShapeKey key;
    key.m_uiBodyID = ts.mBodyID.GetIndexAndSequenceNumber();
    key.m_pShapePtr = ts.mShape.GetPtr();

    bool bExisted = false;
    auto& geo = m_DebugDrawComponents.FindOrAdd(key, &bExisted).Value();
    geo.m_uiLastSeenCounter = m_uiDebugGeoLastSeenCounter;

    DebugGeoShape& shapeGeo = m_DebugDrawShapeGeo[key.m_pShapePtr];
    shapeGeo.m_uiLastSeenCounter = m_uiDebugGeoLastSeenCounter;

    plTransform objTrans;
    objTrans.m_vPosition = plJoltConversionUtils::ToVec3(ts.mShapePositionCOM);
    objTrans.m_qRotation = plJoltConversionUtils::ToQuat(ts.mShapeRotation);
    objTrans.m_vScale = plJoltConversionUtils::ToVec3(ts.mShapeScale);

    if (bExisted)
    {
      plGameObject* pObj;
      if (GetWorld()->TryGetObject(geo.m_hObject, pObj))
      {
        pObj->SetGlobalTransform(objTrans);
      }

      if (!geo.m_bMutableGeometry)
        continue;
    }

    JPH::BodyLockRead lock(m_pSystem->GetBodyLockInterface(), ts.mBodyID);
    if (!lock.Succeeded())
      continue;

    positionsTmp2.Clear();
    materialsTmp2.Clear();

    if (!shapeGeo.m_hMesh.IsValid() || (geo.m_bMutableGeometry && lock.GetBody().IsActive()))
    {
      shapeGeo.m_Bounds = plBoundingBox::MakeInvalid();

      JPH::Shape::GetTrianglesContext ctx;
      ts.mShape->GetTrianglesStart(ctx, JPH::AABox::sBiggest(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::Vec3::sReplicate(1.0f));

      while (true)
      {
        const int triCount = ts.mShape->GetTrianglesNext(ctx, cMaxTriangles, reinterpret_cast<JPH::Float3*>(positionsTmp.GetData()), materialsTmp.GetData());

        if (triCount == 0)
          break;

        positionsTmp2.PushBackRange(positionsTmp.GetArrayPtr().GetSubArray(0, triCount * 3));
        materialsTmp2.PushBackRange(materialsTmp.GetArrayPtr().GetSubArray(0, triCount));
      }

      if (positionsTmp2.GetCount() >= 3)
      {
        plDynamicMeshBufferResourceDescriptor desc;
        desc.m_Topology = plGALPrimitiveTopology::Triangles;
        desc.m_uiMaxVertices = positionsTmp2.GetCount();
        desc.m_uiMaxPrimitives = positionsTmp2.GetCount() / 3;
        desc.m_IndexType = plGALIndexType::None;
        desc.m_bColorStream = false;

        plStringBuilder sGuid;
        sGuid.SetFormat("ColMeshVisGeo_{}", s_iColMeshVisGeoCounter.Increment());

        if (!shapeGeo.m_hMesh.IsValid())
        {
          shapeGeo.m_hMesh = plResourceManager::CreateResource<plDynamicMeshBufferResource>(sGuid, std::move(desc));
        }

        plResourceLock<plDynamicMeshBufferResource> pMeshBuf(shapeGeo.m_hMesh, plResourceAcquireMode::BlockTillLoaded);

        auto positionData = pMeshBuf->AccessPositionData();
        auto nttData = pMeshBuf->AccessNormalTangentTexCoord0Data();
        for (plUInt32 vtxIdx = 0; vtxIdx < positionData.GetCount(); ++vtxIdx)
        {
          const auto& pos = positionsTmp2[vtxIdx];
          positionData[vtxIdx] = pos;

          auto& vtx = nttData[vtxIdx];
          vtx.m_vTexCoord.SetZero();
          vtx.m_vEncodedNormal.SetZero();
          vtx.m_vEncodedTangent.SetZero();

          shapeGeo.m_Bounds.ExpandToInclude(pos);
        }
      }
    }

    if (bExisted)
      continue;

    plGameObjectDesc gd;
    gd.m_bDynamic = true;
    gd.m_LocalPosition = objTrans.m_vPosition;
    gd.m_LocalRotation = objTrans.m_qRotation;
    gd.m_LocalScaling = objTrans.m_vScale;

    gd.m_Tags.Set(tag);
    plGameObject* pObj;
    geo.m_hObject = GetWorld()->CreateObject(gd, pObj);
    geo.m_bMutableGeometry = lock.GetBody().IsSoftBody();

    plCustomMeshComponent* pMesh;
    plCustomMeshComponent::CreateComponent(pObj, pMesh);

    const bool bKinematic = (lock.GetBody().GetMotionType() == JPH::EMotionType::Kinematic);
    const auto& vis = s_Vis[plMath::FirstBitLow((plUInt32)shapeType)][bKinematic ? 1 : 0];

    pMesh->SetMeshResource(shapeGeo.m_hMesh);
    pMesh->SetBounds(shapeGeo.m_Bounds);
    pMesh->SetMaterialFile(vis.m_szMaterial);
    pMesh->SetColor(vis.m_Color);
  }
}

void plJoltWorldModule::AddImpulse(plUInt32 uiBodyID, const plVec3& vImpulse)
{
  PL_LOCK(m_ImpulsesMutex);
  auto& imp = m_Impulses.ExpandAndGetRef();
  imp.m_uiBodyID = uiBodyID;
  imp.m_vImpulse = vImpulse;
  imp.m_Type = plJoltImpulse::Type::Center;
}

void plJoltWorldModule::AddTorque(plUInt32 uiBodyID, const plVec3& vImpulse)
{
  PL_LOCK(m_ImpulsesMutex);
  auto& imp = m_Impulses.ExpandAndGetRef();
  imp.m_uiBodyID = uiBodyID;
  imp.m_vImpulse = vImpulse;
  imp.m_Type = plJoltImpulse::Type::Angular;
}

void plJoltWorldModule::AddImpulse(plUInt32 uiBodyID, const plVec3& vImpulse, const plVec3& vGlobalPosition)
{
  PL_LOCK(m_ImpulsesMutex);
  auto& imp = m_Impulses.ExpandAndGetRef();
  imp.m_uiBodyID = uiBodyID;
  imp.m_vImpulse = vImpulse;
  imp.m_vGlobalPosition = vGlobalPosition;
  imp.m_Type = plJoltImpulse::Type::AtGlobalPos;
}

void plJoltWorldModule::ApplyImpulses()
{
  PL_LOCK(m_ImpulsesMutex);

  if (m_Impulses.IsEmpty())
    return;

  auto* pBodies = &m_pSystem->GetBodyInterface();
  plHybridArray<plJoltImpulse, 64> retain;

  for (plUInt32 i = 0; i < m_Impulses.GetCount(); ++i)
  {
    auto& imp = m_Impulses[i];

    const JPH::BodyID bodyId(imp.m_uiBodyID);

    if (bodyId.IsInvalid())
      continue;

    if (!pBodies->IsAdded(bodyId))
    {
      // A full index + sequence must still be queued for addition. Deleted bodies
      // and failed creations must not leave undeliverable impulses behind forever.
      if (m_BodiesToAdd.Contains(imp.m_uiBodyID) || m_BodiesToAddAndActivate.Contains(imp.m_uiBodyID))
        retain.PushBack(imp);
      continue;
    }

    switch (imp.m_Type)
    {
      case plJoltImpulse::Type::AtGlobalPos:
        pBodies->AddImpulse(bodyId, plJoltConversionUtils::ToVec3(imp.m_vImpulse), plJoltConversionUtils::ToVec3(imp.m_vGlobalPosition));
        break;
      case plJoltImpulse::Type::Center:
        pBodies->AddImpulse(bodyId, plJoltConversionUtils::ToVec3(imp.m_vImpulse));
        break;
      case plJoltImpulse::Type::Angular:
        pBodies->AddTorque(bodyId, plJoltConversionUtils::ToVec3(imp.m_vImpulse));
        break;
    }
  }

  m_Impulses.Clear();

  for (auto& imp : retain)
  {
    m_Impulses.PushBack(imp);
  }
}

//////////////////////////////////////////////////////////////////

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plJoltNavmeshGeoWorldModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltNavmeshGeoWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE
// clang-format on

plJoltNavmeshGeoWorldModule::plJoltNavmeshGeoWorldModule(plWorld* pWorld)
  : plNavmeshGeoWorldModuleInterface(pWorld)
{
  m_pJoltModule = pWorld->GetOrCreateModule<plJoltWorldModule>();
}

void plJoltNavmeshGeoWorldModule::RetrieveGeometryInArea(plUInt32 uiCollisionLayer, const plBoundingBox& box, plDynamicArray<plNavmeshTriangle>& out_triangles) const
{
  const plPhysicsQueryParameters params(uiCollisionLayer, plPhysicsShapeType::Static);
  m_pJoltModule->QueryGeometryInBox(params, box, out_triangles);
}

plResult plJoltWorldModule::BuildJoltHeightfieldShape(const plPhysicsWorldModuleInterface::HeightfieldColliderData& data, HeightfieldCacheEntry& out_Data)
{
  const plUInt32 N = data.m_uiResolution;
  const float halfX = data.m_vHalfExtents.x;
  const float halfY = data.m_vHalfExtents.y;

  if (N < 4 || (N % 2) != 0 || data.m_Heights.GetCount() != N * N)
  {
    plLog::Error("BuildJoltHeightfieldShape: invalid height data (N={}, count={}).", N, data.m_Heights.GetCount());
    return PL_FAILURE;
  }

  const plUInt32 uiCellCount = (N - 1) * (N - 1);
  const bool bHasMaterials = !data.m_Surfaces.IsEmpty() && !data.m_MaterialIndices.IsEmpty();

  if (bHasMaterials && data.m_MaterialIndices.GetCount() != uiCellCount)
  {
    plLog::Error("BuildJoltHeightfieldShape: material indices count ({}) must be (N-1)^2 = {}.", data.m_MaterialIndices.GetCount(), uiCellCount);
    return PL_FAILURE;
  }

  // Resolve surface handles to Jolt material pointers. The handles are stored to keep the resources alive.
  out_Data.m_Surfaces = data.m_Surfaces;
  out_Data.m_MaterialPtrs.SetCount(data.m_Surfaces.GetCount(), nullptr);
  for (plUInt32 i = 0; i < data.m_Surfaces.GetCount(); ++i)
  {
    if (data.m_Surfaces[i].IsValid())
    {
      plResourceLock<plSurfaceResource> pSurface(data.m_Surfaces[i], plResourceAcquireMode::BlockTillLoaded_NeverFail);
      if (pSurface.GetAcquireResult() == plResourceAcquireResult::Final && pSurface->m_pPhysicsMaterialJolt != nullptr)
      {
        out_Data.m_MaterialPtrs[i] = reinterpret_cast<const plJoltMaterial*>(pSurface->m_pPhysicsMaterialJolt);
      }
    }
  }

  // Row order must be flipped so that Jolt's row axis maps to +Y in Engine space after the +90° X rotation.
  plDynamicArray<float> flippedSamples;
  flippedSamples.SetCountUninitialized(N * N);
  for (plUInt32 row = 0; row < N; ++row)
  {
    const plUInt32 srcRow = N - 1 - row;
    for (plUInt32 col = 0; col < N; ++col)
    {
      flippedSamples[row * N + col] = data.m_Heights[srcRow * N + col];
    }
  }

  // Jolt requires EITHER both a non-null index array and a non-empty material list, OR neither.
  JPH::PhysicsMaterialList joltMaterials;
  if (bHasMaterials)
  {
    for (const plJoltMaterial* pMat : out_Data.m_MaterialPtrs)
    {
      joltMaterials.push_back(pMat != nullptr ? pMat : plJoltCore::GetDefaultMaterial());
    }
  }

  JPH::HeightFieldShapeSettings settings(
    flippedSamples.GetData(),
    JPH::Vec3(-halfX, 0.0f, -halfY),
    JPH::Vec3(2.0f * halfX / static_cast<float>(N - 1), 1.0f, 2.0f * halfY / static_cast<float>(N - 1)),
    N,
    bHasMaterials ? data.m_MaterialIndices.GetData() : nullptr,
    joltMaterials);

  JPH::ShapeSettings::ShapeResult result = settings.Create();
  if (result.HasError())
  {
    plLog::Error("BuildJoltHeightfieldShape: failed to create JPH::HeightFieldShape: {}", result.GetError().c_str());
    return PL_FAILURE;
  }

  out_Data.m_pShape = result.Get(); // JPH::Ref takes a reference
  return PL_SUCCESS;
}

plResult plJoltWorldModule::AttachHeightfieldBody(plJoltWorldModule* pModule, plGameObject* pOwner, plSharedPtr<HeightfieldCacheEntry> pEntry, plUInt8 uiCollisionLayer, plStringView sCacheIdentifier)
{
  // Retain replacement ownership before deleting the old component, including when
  // reattaching the same entry or replacing a named cache entry with a rebake.
  const auto& cacheEntry = *pEntry;
  PL_ASSERT_DEV(cacheEntry.m_pShape != nullptr, "Invalid shape");

  // Remove any existing heightfield collider (its OnDeactivated decrements the old cache entry ref count).
  plJoltHeightfieldColliderComponent* pExisting = nullptr;
  if (pOwner->TryGetComponentOfBaseType(pExisting))
  {
    pExisting->DeleteComponent();
    pExisting = nullptr;
  }

  // Wrap in a RotatedTranslatedShape to apply the +90° X rotation that maps Jolt's Y-up to Engine's Z-up.
  const JPH::Quat qRotX90 = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI);
  JPH::RotatedTranslatedShapeSettings rtsSettings(JPH::Vec3::sZero(), qRotX90, cacheEntry.m_pShape);
  JPH::ShapeSettings::ShapeResult rtsResult = rtsSettings.Create();
  if (rtsResult.HasError())
  {
    plLog::Error("AttachHeightfieldBody: failed to wrap shape: {}", rtsResult.GetError().c_str());
    return PL_FAILURE;
  }

  plJoltHeightfieldColliderComponent* pComp = nullptr;
  plJoltHeightfieldColliderComponent::CreateComponent(pOwner, pComp);

  plJoltUserData* pUserData = nullptr;
  pComp->m_uiUserDataIndex = pModule->AllocateUserData(pUserData);
  pUserData->Init(pComp);

  pComp->m_uiObjectFilterID = pModule->CreateObjectFilterID();
  pComp->m_sCacheIdentifier = sCacheIdentifier;
  pComp->m_pCacheEntry = pEntry;

  // Use the first resolved material for body-level friction/restitution, or the default.
  const plJoltMaterial* pMaterial = (!cacheEntry.m_MaterialPtrs.IsEmpty() && cacheEntry.m_MaterialPtrs[0] != nullptr)
                                      ? cacheEntry.m_MaterialPtrs[0]
                                      : plJoltCore::GetDefaultMaterial();

  const plSimdTransform trans = pOwner->GetGlobalTransformSimd();

  JPH::BodyCreationSettings bodyCfg;
  bodyCfg.SetShape(rtsResult.Get());
  bodyCfg.mPosition = plJoltConversionUtils::ToVec3(trans.m_Position);
  bodyCfg.mRotation = plJoltConversionUtils::ToQuat(trans.m_Rotation).Normalized();
  bodyCfg.mMotionType = JPH::EMotionType::Static;
  bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(uiCollisionLayer, plJoltBroadphaseLayer::Static);
  bodyCfg.mRestitution = pMaterial->m_fRestitution;
  bodyCfg.mFriction = pMaterial->m_fFriction;
  bodyCfg.mCollisionGroup.SetGroupID(pComp->m_uiObjectFilterID);
  bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
  bodyCfg.mEnhancedInternalEdgeRemoval = true;
  bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserData);

  auto* pBodies = &pModule->GetJoltSystem()->GetBodyInterface();
  JPH::Body* pBody = pBodies->CreateBody(bodyCfg);
  if (pBody == nullptr)
  {
    plLog::Error("Heightfield body creation failed. Increase the maximum number of bodies.");
    pComp->DeleteComponent();
    return PL_FAILURE;
  }

  pComp->m_uiJoltBodyID = pBody->GetID().GetIndexAndSequenceNumber();

  pModule->QueueBodyToAdd(pBody, false);
  return PL_SUCCESS;
}

void plJoltWorldModule::ReleaseHeightfieldCacheEntry(plStringView sIdentifier, const plJoltHeightfieldCacheEntry* pEntry)
{
  if (sIdentifier.IsEmpty())
    return;

  auto it = m_HeightfieldShapeCache.Find(sIdentifier);
  if (!it.IsValid())
    return;

  // A rebake may have replaced the map entry while an older body still owns its
  // previous version. Releasing that body must not evict the new version.
  if (it.Value().Borrow() == pEntry && pEntry->GetRefCount() == 2)
  {
    m_HeightfieldShapeCache.Remove(it);
  }
}

plResult plJoltWorldModule::TrySetHeightfieldCollider(plGameObject* pOwner, plStringView sIdentifier, plUInt8 uiCollisionLayer)
{
  if (sIdentifier.IsEmpty())
    return PL_FAILURE;

  auto it = m_HeightfieldShapeCache.Find(sIdentifier);
  if (!it.IsValid())
    return PL_FAILURE;

  return AttachHeightfieldBody(this, pOwner, it.Value(), uiCollisionLayer, sIdentifier);
}

void plJoltWorldModule::CreateHeightfieldCollider(plGameObject* pOwner, plStringView sIdentifier, const HeightfieldColliderData& data)
{
  plSharedPtr<HeightfieldCacheEntry> pEntry = PL_DEFAULT_NEW(HeightfieldCacheEntry);
  if (BuildJoltHeightfieldShape(data, *pEntry).Failed())
    return;

  // Empty identifiers deliberately bypass the cache; unrelated terrain tiles must
  // never share mutable geometry or surface ownership through the empty key.
  if (!sIdentifier.IsEmpty())
    m_HeightfieldShapeCache[sIdentifier] = pEntry;
  AttachHeightfieldBody(this, pOwner, pEntry, data.m_uiCollisionLayer, sIdentifier).IgnoreResult();
}

void plJoltWorldModule::RemoveHeightfieldCollider(plGameObject* pOwner)
{
  plJoltHeightfieldColliderComponent* pComp = nullptr;
  if (pOwner->TryGetComponentOfBaseType(pComp))
  {
    pComp->DeleteComponent();
  }
}

plResult plJoltWorldModule::BuildJoltTriangleMeshShape(const plPhysicsWorldModuleInterface::TriangleMeshColliderData& data, JPH::Ref<JPH::Shape>& out_pShape, const plJoltMaterial*& out_pFirstMaterial)
{
  const plUInt32 uiVertCount = data.m_Vertices.GetCount();
  const plUInt32 uiIdxCount = data.m_Indices.GetCount();

  if (uiVertCount == 0 || uiIdxCount == 0 || (uiIdxCount % 3) != 0)
  {
    plLog::Error("BuildJoltTriangleMeshShape: invalid mesh data (vertices={}, indices={}).", uiVertCount, uiIdxCount);
    return PL_FAILURE;
  }

  const plUInt32 uiTriCount = uiIdxCount / 3;
  const bool bHasMaterials = !data.m_Surfaces.IsEmpty() && !data.m_TriangleMaterials.IsEmpty();

  // Resolve surface handles to Jolt material pointers.
  plDynamicArray<const plJoltMaterial*> materialPtrs;
  if (bHasMaterials)
  {
    materialPtrs.SetCount(data.m_Surfaces.GetCount(), nullptr);
    for (plUInt32 i = 0; i < data.m_Surfaces.GetCount(); ++i)
    {
      if (data.m_Surfaces[i].IsValid())
      {
        plResourceLock<plSurfaceResource> pSurface(data.m_Surfaces[i], plResourceAcquireMode::BlockTillLoaded_NeverFail);
        if (pSurface.GetAcquireResult() == plResourceAcquireResult::Final && pSurface->m_pPhysicsMaterialJolt != nullptr)
        {
          materialPtrs[i] = reinterpret_cast<const plJoltMaterial*>(pSurface->m_pPhysicsMaterialJolt);
        }
      }
    }
    out_pFirstMaterial = (!materialPtrs.IsEmpty() && materialPtrs[0] != nullptr) ? materialPtrs[0] : plJoltCore::GetDefaultMaterial();
  }
  else
  {
    out_pFirstMaterial = plJoltCore::GetDefaultMaterial();
  }

  JPH::VertexList vertexList;
  vertexList.resize(uiVertCount);
  for (plUInt32 i = 0; i < uiVertCount; ++i)
  {
    vertexList[i] = plJoltConversionUtils::ToFloat3(data.m_Vertices[i]);
  }

  JPH::IndexedTriangleList triangleList;
  triangleList.reserve(uiTriCount);
  for (plUInt32 i = 0; i < uiTriCount; ++i)
  {
    const plUInt32 idx0 = data.m_Indices[i * 3 + 0];
    const plUInt32 idx1 = data.m_Indices[i * 3 + 1];
    const plUInt32 idx2 = data.m_Indices[i * 3 + 2];

    if (idx0 == idx1 || idx0 == idx2 || idx1 == idx2)
      continue;

    const plVec3 v0 = plJoltConversionUtils::ToVec3(vertexList[idx0]);
    const plVec3 v1 = plJoltConversionUtils::ToVec3(vertexList[idx1]);
    const plVec3 v2 = plJoltConversionUtils::ToVec3(vertexList[idx2]);
    if (v0.IsEqual(v1, 0.001f) || v0.IsEqual(v2, 0.001f) || v1.IsEqual(v2, 0.001f))
      continue;

    auto& tri = triangleList.emplace_back();
    tri.mIdx[0] = idx0;
    tri.mIdx[1] = idx1;
    tri.mIdx[2] = idx2;
    tri.mMaterialIndex = (bHasMaterials && i < data.m_TriangleMaterials.GetCount()) ? data.m_TriangleMaterials[i] : 0;
  }

  if (triangleList.empty())
  {
    plLog::Error("BuildJoltTriangleMeshShape: all triangles were degenerate.");
    return PL_FAILURE;
  }

  JPH::MeshShapeSettings meshSettings(vertexList, triangleList);
  if (bHasMaterials)
  {
    meshSettings.mMaterials.resize(materialPtrs.GetCount());
    for (plUInt32 i = 0; i < materialPtrs.GetCount(); ++i)
      meshSettings.mMaterials[i] = (materialPtrs[i] != nullptr) ? materialPtrs[i] : plJoltCore::GetDefaultMaterial();
  }
  else
  {
    meshSettings.mMaterials.resize(1);
    meshSettings.mMaterials[0] = plJoltCore::GetDefaultMaterial();
  }

  JPH::ShapeSettings::ShapeResult result = meshSettings.Create();
  if (result.HasError())
  {
    plLog::Error("BuildJoltTriangleMeshShape: failed to create JPH::MeshShape: {}", result.GetError().c_str());
    return PL_FAILURE;
  }

  out_pShape = result.Get();
  return PL_SUCCESS;
}

void plJoltWorldModule::AttachTriangleMeshBody(plJoltWorldModule* pModule, plGameObject* pOwner, JPH::Ref<JPH::Shape> pShape, const plJoltMaterial* pMaterial, plUInt8 uiCollisionLayer, plArrayPtr<const plSurfaceResourceHandle> surfaces)
{
  PL_ASSERT_DEV(pShape != nullptr, "Invalid shape");

  // Remove any existing triangle mesh collider component.
  plJoltTriangleMeshColliderComponent* pExisting = nullptr;
  if (pOwner->TryGetComponentOfBaseType(pExisting))
  {
    pExisting->DeleteComponent();
    pExisting = nullptr;
  }

  plJoltTriangleMeshColliderComponent* pComp = nullptr;
  plJoltTriangleMeshColliderComponent::CreateComponent(pOwner, pComp);
  pComp->m_Surfaces = surfaces;

  plJoltUserData* pUserData = nullptr;
  pComp->m_uiUserDataIndex = pModule->AllocateUserData(pUserData);
  pUserData->Init(pComp);

  pComp->m_uiObjectFilterID = pModule->CreateObjectFilterID();

  if (pMaterial == nullptr)
    pMaterial = plJoltCore::GetDefaultMaterial();

  const plSimdTransform trans = pOwner->GetGlobalTransformSimd();

  JPH::BodyCreationSettings bodyCfg;
  bodyCfg.SetShape(pShape);
  bodyCfg.mPosition = plJoltConversionUtils::ToVec3(trans.m_Position);
  bodyCfg.mRotation = plJoltConversionUtils::ToQuat(trans.m_Rotation).Normalized();
  bodyCfg.mMotionType = JPH::EMotionType::Static;
  bodyCfg.mObjectLayer = plJoltCollisionFiltering::ConstructObjectLayer(uiCollisionLayer, plJoltBroadphaseLayer::Static);
  bodyCfg.mRestitution = pMaterial->m_fRestitution;
  bodyCfg.mFriction = pMaterial->m_fFriction;
  bodyCfg.mCollisionGroup.SetGroupID(pComp->m_uiObjectFilterID);
  bodyCfg.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter());
  bodyCfg.mEnhancedInternalEdgeRemoval = true;
  bodyCfg.mUserData = reinterpret_cast<plUInt64>(pUserData);

  auto* pBodies = &pModule->GetJoltSystem()->GetBodyInterface();
  JPH::Body* pBody = pBodies->CreateBody(bodyCfg);
  PL_ASSERT_DEV(pBody != nullptr, "Jolt body creation failed. Increase the maximum number of bodies.");

  pComp->m_uiJoltBodyID = pBody->GetID().GetIndexAndSequenceNumber();
  pModule->QueueBodyToAdd(pBody, false);
}

void plJoltWorldModule::CreateTriangleMeshCollider(plGameObject* pOwner, const TriangleMeshColliderData& data)
{
  JPH::Ref<JPH::Shape> pShape;
  const plJoltMaterial* pMaterial = nullptr;
  if (BuildJoltTriangleMeshShape(data, pShape, pMaterial).Failed())
    return;

  AttachTriangleMeshBody(this, pOwner, pShape, pMaterial, data.m_uiCollisionLayer, data.m_Surfaces);
}

void plJoltWorldModule::RemoveTriangleMeshCollider(plGameObject* pOwner)
{
  plJoltTriangleMeshColliderComponent* pComp = nullptr;
  if (pOwner->TryGetComponentOfBaseType(pComp))
  {
    pComp->DeleteComponent();
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_System_JoltWorldModule);
