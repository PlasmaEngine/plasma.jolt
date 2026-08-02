#pragma once

#include <JoltPlugin/JoltPluginDLL.h>

#include <Core/ResourceManager/ResourceHandle.h>
#include <GameEngine/Destruction/DestructibleGraph.h>
#include <JoltPlugin/Resources/JoltDestructibleMeshResource.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Pipeline/RenderData.h>

using plMaterialResourceHandle = plTypedResourceHandle<class plMaterialResource>;
using plSurfaceResourceHandle = plTypedResourceHandle<class plSurfaceResource>;

struct plMsgPhysicsAddImpulse;
struct plMsgPhysicsDestructibleDamage;
struct plMsgPhysicContact;
struct plMsgExtractRenderData;
struct plMsgDamage;
class plJoltMaterial;
class plJoltWorldModule;

namespace JPH
{
  class BodyInterface;
  class Constraint;
}

/// \brief Whether a destructible object is anchored to the world or free-standing.
struct PL_JOLTPLUGIN_DLL plJoltDestructibleAnchorUsage
{
  using StorageType = plUInt8;

  enum Enum
  {
    FromAsset,    ///< Anchored via the anchor bonds baked into the asset (static until support is lost).
    FreeStanding, ///< The whole object is one dynamic body from the start; anchor bonds are ignored.
    Static,       ///< Every chunk is world-anchored: nothing ever collapses from lost support, only chunks
                  ///< whose bonds are all broken detach as dynamic debris. For permanent barriers.

    Default = FromAsset
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltDestructibleAnchorUsage);

class PL_JOLTPLUGIN_DLL plJoltDestructibleComponentManager : public plComponentManager<class plJoltDestructibleComponent, plBlockStorageType::FreeList>
{
public:
  plJoltDestructibleComponentManager(plWorld* pWorld);
  ~plJoltDestructibleComponentManager();

  virtual void Initialize() override;

private:
  friend class plJoltWorldModule;
  friend class plJoltDestructibleComponent;

  void UpdateMovingDestructibles(plTime deltaTime);
  void PostAsyncUpdate(const plWorldModule::UpdateContext& context);

  plSet<plComponentHandle> m_RequireDamageProcessing;
  plSet<plJoltDestructibleComponent*> m_MovingDestructibles;
  plSet<plJoltDestructibleComponent*> m_FadingDestructibles;
  plSet<plJoltDestructibleComponent*> m_DebrisLodComponents;
  plUInt32 m_uiBreakEffectsThisFrame = 0; ///< World-wide break-effect spawns this frame, capped by Jolt.Destructible.MaxBreakEffectsPerFrame.
};

/// \brief A pre-fractured mesh (see plJoltDestructibleMeshResource) that breaks apart under damage, NVIDIA-Blast style.
///
/// The baked chunk graph tracks bonds between adjacent chunks. Damage (explosions via
/// plMsgPhysicsDestructibleDamage, impulses, hard contacts) breaks bonds; chunks that lose their
/// connection to the world anchors split off as rigid bodies (one compound body per connected island,
/// plain convex bodies for single-chunk debris). The component owns all bodies internally and renders
/// every visible chunk as a sub-mesh of the baked render mesh.
class PL_JOLTPLUGIN_DLL plJoltDestructibleComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltDestructibleComponent, plRenderComponent, plJoltDestructibleComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;
  virtual void OnSimulationStarted() override;

  //////////////////////////////////////////////////////////////////////////
  // plRenderComponent

public:
  virtual plResult GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg) override;

  //////////////////////////////////////////////////////////////////////////
  // plJoltDestructibleComponent

public:
  void SetDestructibleMeshFile(const char* szFile); // [ property ]
  const char* GetDestructibleMeshFile() const;      // [ property ]

  void SetOnBreakInteraction(const char* szName) { m_sOnBreakInteraction.Assign(szName); } // [ property ]
  const char* GetOnBreakInteraction() const { return m_sOnBreakInteraction.GetData(); }    // [ property ]

  void SetOnChipInteraction(const char* szName) { m_sOnChipInteraction.Assign(szName); } // [ property ]
  const char* GetOnChipInteraction() const { return m_sOnChipInteraction.GetData(); }    // [ property ]

  void SetOnSettleInteraction(const char* szName) { m_sOnSettleInteraction.Assign(szName); } // [ property ]
  const char* GetOnSettleInteraction() const { return m_sOnSettleInteraction.GetData(); }    // [ property ]

  plJoltDestructibleMeshResourceHandle m_hDestructible; // [ property ]

  /// \brief Applies radial damage at a world position. Bonds within the radius lose health with linear falloff.
  void ApplyDamage(const plVec3& vGlobalPosition, float fRadius, float fDamage, const plVec3& vImpulse); // [ scriptable ]

private:
  friend class plJoltDestructibleComponentManager;

  struct DamageEvent
  {
    plVec3 m_vGlobalPosition;
    float m_fRadius;
    float m_fDamage;
    plVec3 m_vImpulse;
    plUInt32 m_uiChunkIndex = plInvalidIndex;
  };

  struct IslandBody
  {
    plUInt32 m_uiBodyID = plInvalidIndex;
    plTransform m_Transform = plTransform::MakeIdentity();           ///< Body origin transform = component-space to world.
    plTransform m_LastRenderTransform = plTransform::MakeIdentity(); ///< m_Transform of the previous update, feeds per-instance motion vectors (TAA velocity).
    plTime m_SpawnTime;
    plTime m_FadeStartTime;
    bool m_bSingleChunk = false;
    bool m_bFading = false;
    bool m_bMerged = false;                    ///< Frozen into the component's consolidated static debris proxy.
    JPH::Constraint* m_pTether = nullptr;      ///< Dangling debris: world-space point constraint at the last surviving bond site.
    float m_fTetherBreakThreshold = 0.0f;      ///< Constraint impulse above which the tether tears (island falls for real).
  };

  struct SavedNodeState
  {
    PL_DECLARE_POD_TYPE();

    plUInt32 m_uiChunkIndex = plInvalidIndex;
    plUInt8 m_uiState = 0;
    float m_fAccumulatedDamage = 0.0f;
  };

  struct SavedBondState
  {
    PL_DECLARE_POD_TYPE();

    plUInt32 m_uiChunkA = plInvalidIndex;
    plUInt32 m_uiChunkB = plInvalidIndex;
    float m_fHealth = 0.0f;
  };

  void Initialize(plJoltWorldModule* pModule);
  void Cleanup();
  void CreateInitialBodies(plJoltWorldModule* pModule);
  JPH::Shape* BuildIslandShape(plArrayPtr<const plUInt32> nodes, bool bStatic);
  plUInt32 CreateIslandBody(plJoltWorldModule* pModule, JPH::Shape* pShape, const plTransform& transform, const plVec3& vLinearVelocity, const plVec3& vAngularVelocity, float fMass);
  float GetIslandMass(plArrayPtr<const plUInt32> nodes) const;
  void RebuildStaticBody(plJoltWorldModule* pModule);
  void ProcessDamage(plJoltWorldModule* pModule);
  void SyncAnchoredBodyTransform(plJoltWorldModule* pModule, plTime deltaTime);
  void RetrieveIslandTransforms();
  void RebuildMergedDebrisProxy(plJoltWorldModule* pModule);
  void DestroyMergedDebrisProxy(plJoltWorldModule* pModule);
  void BeginDebrisFade(JPH::BodyInterface& jphBodies, plUInt32 uiGroupId, plTime now);
  void DestroyIslandPhysicsBody(JPH::BodyInterface& jphBodies, IslandBody& island);
  void DestroyIslandBody(JPH::BodyInterface& jphBodies, plUInt32 uiGroupId);
  void DestroyAllBodies();
  void QueueDamageEvent(const DamageEvent& event);
  plUInt32 ResolveHitChunk(void* pInternalPhysicsActor, void* pInternalPhysicsShape) const;
  const plJoltMaterial* GetPhysicsMaterial() const;
  void SpawnBreakEffects(plArrayPtr<const plDestructibleGraph::BrokenBond> brokenBonds, const plHashedString& sInteraction);
  void SpawnSurfaceInteraction(const plVec3& vGlobalPosition, const plVec3& vNormal, const plHashedString& sInteraction);
  void TryCreateTether(plJoltWorldModule* pModule, IslandBody& ref_island, plArrayPtr<const plUInt32> nodes, float fIslandMass);
  void RemoveTether(IslandBody& ref_island, bool bActivateBody);
  plUInt32 CountTetheredIslands() const;
  void ApplyRestoredDamageState(plJoltWorldModule* pModule, const plDestructibleMeshDescriptor& desc);
  void DebugDraw();

  void OnMsgPhysicsDestructibleDamage(plMsgPhysicsDestructibleDamage& ref_msg); // [ msg handler ]
  void OnMsgPhysicsAddImpulse(plMsgPhysicsAddImpulse& ref_msg);                 // [ msg handler ]
  void OnMsgPhysicContactMsg(plMsgPhysicContact& ref_msg);                      // [ msg handler ]
  void OnMsgDamage(plMsgDamage& ref_msg);                                       // [ msg handler ]

  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

  // properties
  plSurfaceResourceHandle m_hSurface;
  plEnum<plJoltDestructibleAnchorUsage> m_AnchorUsage;
  float m_fToughness = 1.0f;               ///< Bond health per m^2 of contact area. Higher = harder to break.
  float m_fInteriorDamageScale = 0.25f;    ///< Damage scale on interior chunks still covered by exterior ones (0 = invulnerable until exposed).
  float m_fStressFactor = 0.0f;            ///< How aggressively unsupported weight tears bonds. 0 = gravity load ignored (no self-collapse).
  float m_fStressSafetyFactor = 1.5f;      ///< Load headroom a bond must exceed before it starts tearing under gravity stress.
  plHashedString m_sOnBreakInteraction = plMakeHashedString("Break"); ///< Surface interaction spawned where bonds break AND chunks detach. Empty = no effects.
  plHashedString m_sOnChipInteraction = plMakeHashedString("Chip");   ///< Surface interaction spawned where bonds break WITHOUT detaching anything (surface scarring). Empty = no effects.
  plHashedString m_sOnSettleInteraction;   ///< Surface interaction spawned under debris when it freezes into the merged static proxy (settled dust). Empty = off.
  plUInt8 m_uiMaxBreakEffects = 4;         ///< Max break/chip effects spawned per damage event (largest broken bonds first).
  float m_fDamageVisualIntensity = 1.0f;   ///< How strongly accumulated chunk damage darkens the rendered chunks (0 = off).
  float m_fDanglingChance = 0.35f;         ///< Chance that a detaching island hangs from its last bond site instead of falling freely (0 = off).
  float m_fDanglingBreakForceFactor = 0.5f; ///< Tether break impulse per kg of island mass. ~0.16 holds exactly the static load at 60 Hz; higher = sturdier.
  float m_fDanglingMaxMass = 50.0f;        ///< Islands heavier than this never dangle.
  plUInt8 m_uiMaxDangling = 8;             ///< Max simultaneously tethered islands.
  float m_fDensity = 500.0f;               ///< kg/m^3, used to derive chunk masses.
  float m_fContactDamageThreshold = 0.0f;  ///< Contact impact force above which automatic damage is applied. 0 = off.
  float m_fContactDamageFactor = 0.01f;    ///< Converts contact impact force to damage.
  float m_fDamageMsgRadius = 0.5f;         ///< Damage radius applied when a plMsgDamage (bullet/hit) lands on this object.
  float m_fDamageMsgFactor = 0.1f;         ///< Converts gameplay damage numbers to destruction damage.
  float m_fGravityFactor = 1.0f;
  plUInt8 m_uiCollisionLayerStatic = 0;
  plUInt8 m_uiCollisionLayerDynamic = 0;
  plUInt16 m_uiMaxDebris = 64;             ///< Oldest single-chunk debris is removed when exceeding this.
  plTime m_DebrisLifetime = plTime::MakeFromSeconds(20.0);
  plTime m_DebrisFadeDuration = plTime::MakeFromSeconds(0.5); ///< Visual shrink-out after debris physics is culled. 0 = immediate removal.
  plTime m_DebrisMergeDelay = plTime::MakeFromSeconds(2.0);  ///< Sleeping single chunks become one static compound after this delay. 0 = disabled.

  // runtime state
  plVec3 m_vScale = plVec3(1.0f); ///< Owner scale captured at simulation start (scale changes during play are not supported).
  plDestructibleGraph m_Graph;
  plMap<plUInt32, IslandBody> m_IslandBodies; ///< Group id -> body. Group 0 = the static (anchored) body.
  plDynamicArray<plUInt32> m_DebrisSpawnOrder; ///< Group ids of single-chunk debris, oldest first.
  plHybridArray<DamageEvent, 4> m_DamageEvents;
  plUInt32 m_uiMergedDebrisBodyID = plInvalidIndex;

  plUInt32 m_uiObjectFilterID = plInvalidIndex;
  plUInt32 m_uiUserDataIndexStatic = plInvalidIndex;
  plUInt32 m_uiUserDataIndexDynamic = plInvalidIndex;

  // damage state restored from a save game; applied on the next Initialize
  plDynamicArray<SavedNodeState> m_PendingRestoreNodes;
  plDynamicArray<SavedBondState> m_PendingRestoreBonds;
  bool m_bPendingRestore = false;

  plBoundingBoxSphere m_Bounds;
  mutable plUInt8 m_uiSleepCounter = 200;
  mutable plDynamicArray<plInstanceDataOffset> m_ChunkInstanceOffsets; ///< One per chunk (slot 0 = pristine mesh); stable across chunk splits.
};
