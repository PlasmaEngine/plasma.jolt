#pragma once

#include <JoltPlugin/JoltPluginDLL.h>

#include <Core/ResourceManager/ResourceHandle.h>
#include <Foundation/Threading/TaskSystem.h>
#include <GameEngine/Physics/Breakable2D.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Meshes/SkinnedMeshRenderData.h>

using plMaterialResourceHandle = plTypedResourceHandle<class plMaterialResource>;
using plMeshResourceHandle = plTypedResourceHandle<class plMeshResource>;

struct plMsgPhysicsAddImpulse;
struct plMsgExtractRenderData;
struct plMsgDamage;
class plGeometry;
class plMeshResourceDescriptor;
class plJoltMaterial;

namespace JPH
{
  class BodyInterface;
}

/// \brief Flags that define which edges of a breakable slab are fixed/anchored in the world.
/// A fixed edge means that shards adjacent to that edge will remain stationary and not fall under gravity.
struct PL_JOLTPLUGIN_DLL plJoltBreakableSlabFlags
{
  using StorageType = plUInt8;

  enum Enum
  {
    FixedEdgeTop = PL_BIT(0),    ///< Shards will stick to this edge
    FixedEdgeRight = PL_BIT(1),  ///< Shards will stick to this edge
    FixedEdgeBottom = PL_BIT(2), ///< Shards will stick to this edge
    FixedEdgeLeft = PL_BIT(3),   ///< Shards will stick to this edge

    Default = FixedEdgeTop | FixedEdgeRight | FixedEdgeBottom | FixedEdgeLeft
  };

  struct Bits
  {
    StorageType FixedEdgeTop : 1;
    StorageType FixedEdgeRight : 1;
    StorageType FixedEdgeBottom : 1;
    StorageType FixedEdgeLeft : 1;
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltBreakableSlabFlags);

/// \brief The general shape of the breakable slab.
///
/// Can be extended with further shapes, if desired.
struct PL_JOLTPLUGIN_DLL plJoltBreakableShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Rectangle,
    Triangle,
    Circle,

    Default = Rectangle
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltBreakableShape);

class PL_JOLTPLUGIN_DLL plJoltBreakableSlabComponentManager : public plComponentManager<class plJoltBreakableSlabComponent, plBlockStorageType::FreeList>
{
public:
  plJoltBreakableSlabComponentManager(plWorld* pWorld);
  ~plJoltBreakableSlabComponentManager();

  virtual void Initialize() override;

private:
  friend class plJoltWorldModule;
  friend class plJoltBreakableSlabComponent;

  void PreAsyncUpdate(const plWorldModule::UpdateContext& context);
  void ReinitSlabs(const plWorldModule::UpdateContext& context);
  void PostAsyncUpdate(const plWorldModule::UpdateContext& context);

  plSet<plComponentHandle> m_RequireBreakage;
  plSet<plComponentHandle> m_RequireBreakUpdate;

  plAtomicInteger32 m_iTriggerBoundsUpdateSlot;
  plDynamicArray<plJoltBreakableSlabComponent*> m_TriggerBoundsUpdate;
};

/// \brief Represents a point in world space, where the breakable slab should be shattered.
struct plShatterPoint
{
  plVec3 m_vGlobalPosition;
  plVec3 m_vImpulse;                                                        ///< With which impulse to push aways new, dynamic shards.
  plUInt32 m_uiShardIdx = plInvalidIndex;                                   ///< Which shard to break apart.
  float m_fImpactRadius = 0.05f;                                            ///< The size of the shatter point. Only relevant for some patterns.
  float m_fCellSize = 0.4f;                                                 ///< For the cellular (voronoi) pattern, how large to make cells.
  float m_fMakeDynamicRadius = 0.25f;                                       ///< In what radius around the shatter position to always make new shards dynamic. Unsupported shards will become dynamic regardless.
  plUInt8 m_uiAllowedBreakPatterns = (plUInt8)plBreakablePattern::Cellular; ///< With which pattern to potentially break the shard.
};

/// \brief Most of the shatter calculation (and physics collider generation) is done in this task, to prevent performance drops.
///
/// The result may be ready only with 1-3 frames delay.
class plShatterTask : public plTask
{
public:
  plJoltBreakableSlabComponent* m_pComponent = nullptr;
  plHybridArray<JPH::Ref<JPH::ConvexShape>, 128> m_Shapes;
  plMeshResourceHandle m_hShardsMesh;
  plHybridArray<plShatterPoint, 8> m_ShatterPoints;
  plVec3 m_vFinalImpulse;

protected:
  virtual void Execute() override;
};


/// \brief Component that represents a destructible slab that can be broken into shards.
///
/// This component creates a breakable surface that can shatter into smaller pieces when hit.
/// The slab can be rectangular, triangular or circular and can be anchored on any of its edges.
/// When broken, the shards become dynamic physics objects that can collide and fall under gravity.
class PL_JOLTPLUGIN_DLL plJoltBreakableSlabComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltBreakableSlabComponent, plRenderComponent, plJoltBreakableSlabComponentManager);

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
  // plJoltBreakableSlabComponent

public:
  void SetWidth(float fWidth);         // [ property ]
  float GetWidth() const;              // [ property ]

  void SetHeight(float fHeight);       // [ property ]
  float GetHeight() const;             // [ property ]

  void SetThickness(float fThickness); // [ property ]
  float GetThickness() const;          // [ property ]

  /// \brief Sets the UV scaling factor for texture mapping
  void SetUvScale(plVec2 vScale); // [ property ]
  plVec2 GetUvScale() const;      // [ property ]

  /// \brief Sets which edges of the slab are fixed/anchored in the world
  void SetFlags(plBitflags<plJoltBreakableSlabFlags> flags); // [ property ]
  plBitflags<plJoltBreakableSlabFlags> GetFlags() const      // [ property ]
  {
    return m_Flags;
  }

  /// \brief Sets the basic shape of the breakable slab (rectangle, triangle, circle)
  void SetShape(plEnum<plJoltBreakableShape> shape); // [ property ]
  plEnum<plJoltBreakableShape> GetShape() const      // [ property ]
  {
    return m_Shape;
  }

  void SetOnCrackInteraction(const char* szName) { m_sOnCrackInteraction.Assign(szName); } // [ property ]
  const char* GetOnCrackInteraction() const { return m_sOnCrackInteraction.GetData(); }    // [ property ]

  /// \brief Restores the slab to its original unbroken state
  void Restore(); // [ scriptable ]

  /// \brief Shatters the entire slab into pieces of roughly the given size
  /// \param fShardSize The approximate size of generated shards
  /// \param vImpulse The impulse to apply to the shards
  void ShatterAll(float fShardSize, const plVec3& vImpulse); // [ scriptable ]

  /// \brief Shatters the slab using a cellular (Voronoi) pattern around a point
  /// \param vGlobalPosition The world position where to initiate the break
  /// \param fCellSize The approximate size of the cellular shards
  /// \param vImpulse The impulse to apply to the shards
  /// \param fMakeDynamicRadius Radius around the break point where shards become dynamic
  void ShatterCellular(const plVec3& vGlobalPosition, float fCellSize, const plVec3& vImpulse, float fMakeDynamicRadius); // [ scriptable ]

  /// \brief Shatters the slab in a radial pattern around a point
  /// \param vGlobalPosition The world position where to initiate the break
  /// \param fImpactRadius The radius of the impact area
  /// \param vImpulse The impulse to apply to the shards
  /// \param fMakeDynamicRadius Radius around the break point where shards become dynamic
  void ShatterRadial(const plVec3& vGlobalPosition, float fImpactRadius, const plVec3& vImpulse, float fMakeDynamicRadius); // [ scriptable ]

private:
  friend class plShatterTask;

  void PrepareBreakAsync(plDynamicArray<JPH::Ref<JPH::ConvexShape>>& out_Shapes, plArrayPtr<const plShatterPoint> points);
  void ApplyBreak(plArrayPtr<JPH::Ref<JPH::ConvexShape>> shapes, const plMeshResourceHandle& hMesh, const plVec3& vImpulse);
  void DebugDraw();
  void Cleanup();
  void ReinitMeshes();
  plMeshResourceHandle CreateShardsMesh() const;
  void PrepareShardColliders(plUInt32 uiFirstShard, plDynamicArray<JPH::Ref<JPH::ConvexShape>>& out_Shapes) const;
  void CreateShardColliders(plUInt32 uiFirstShard, plArrayPtr<JPH::Ref<JPH::ConvexShape>> shapes);
  void DestroyAllShardColliders();
  void DestroyShardCollider(plUInt32 uiShardIdx, JPH::BodyInterface& jphBodies, bool bUpdateVis);
  void RetrieveShardTransforms();
  void ApplyImpulse(const plVec3& vImpulse, plUInt32 uiFirstShard = 0);
  void UpdateShardColliders();
  void WakeUpBodies();
  bool IsPointOnSlab(const plVec3& vGlobalPosition) const;
  plUInt32 FindClosestShard(const plVec3& vGlobalPosition) const;

  void OnMsgDamage(plMsgDamage& ref_msg);                                                   // [ msg handler ]
  void OnMsgPhysicsAddImpulse(plMsgPhysicsAddImpulse& ref_msg);                             // [ msg handler ]
  void OnMsgPhysicContactMsg(plMsgPhysicContact& ref_msg);                                  // [ msg handler ]
  void OnMsgPhysicCharacterContact(plMsgPhysicCharacterContact& ref_msg);                   // [ msg handler ]
  void OnMsgCustomInstanceDataOffsetChanged(plMsgCustomInstanceDataOffsetChanged& ref_msg); // [ msg handler ]

  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;
  void BuildMeshResourceFromGeometry(plGeometry& Geometry, plMeshResourceDescriptor& MeshDesc, bool bWithSkinningData) const;
  const plJoltMaterial* GetPhysicsMaterial();

  float m_fGravityFactor = 1.0f;
  float m_fWidth = 1.0f;
  float m_fHeight = 1.0f;
  float m_fThickness = 0.02f;
  plVec2 m_vUvScale = plVec2(1.0f);
  float m_fContactReportForceThreshold = 0.0f;

  // two-stage breakage from gameplay damage (plMsgDamage): while the health pool lasts, hits only
  // spawn the crack interaction (spider-web decal + sound); once spent, the pane shatters radially
  float m_fHealth = 0.0f;               ///< 0 = ignore plMsgDamage entirely (legacy script-driven breakage).
  float m_fInstantShatterDamage = 0.0f; ///< A single hit with at least this much damage shatters immediately. 0 = off.
  plHashedString m_sOnCrackInteraction = plMakeHashedString("Crack");
  float m_fDamageTaken = 0.0f;          ///< Runtime damage accumulator, reset by Restore().

  plBreakable2D m_Breakable;

  plMeshResourceHandle m_hMesh;
  plMaterialResourceHandle m_hMaterial;
  plSurfaceResourceHandle m_hSurface;

  bool m_bReinitMeshes = true;
  mutable plUInt8 m_uiShardsSleeping = 200;
  mutable plInstanceDataOffset m_InstanceDataOffset;

  static plAtomicInteger32 s_iShardMeshCounter;

  plUInt8 m_uiCollisionLayerStatic = 0;
  plUInt8 m_uiCollisionLayerDynamic = 0;

  plUInt32 m_uiUserDataIndexStatic = plInvalidIndex;
  plUInt32 m_uiUserDataIndexDynamic = plInvalidIndex;
  plUInt32 m_uiObjectFilterID = plInvalidIndex;

  plDynamicArray<plUInt32> m_ShardBodyIDs;

  plSkinningState m_SkinningState;
  plBoundingBoxSphere m_Bounds;

  plEnum<plJoltBreakableShape> m_Shape;
  plBitflags<plJoltBreakableSlabFlags> m_Flags;

  plHybridArray<plShatterPoint, 2> m_ShatterPoints;
  plSharedPtr<plShatterTask> m_pShatterTask;
};
