#pragma once

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <JoltPlugin/CharacterCloth/JoltClothInstance.h>
#include <JoltPlugin/CharacterCloth/JoltClothResource.h>

struct plMsgUpdateLocalBounds;
struct plMsgAnimationPoseUpdated;
class plAnimatedMeshComponent;
class plSkeleton;

/// \brief Where a garment stands in the budget when more garments want to simulate than may.
struct PL_JOLTPLUGIN_DLL plJoltClothImportance
{
  using StorageType = plUInt8;

  enum Enum
  {
    Low,
    Normal,
    High,
    Always, ///< Never drops below full rate. For the player's garments.

    Default = Normal
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltClothImportance);

/// \brief What the garment collides with in the world: what its cloth asset says, or an override.
struct PL_JOLTPLUGIN_DLL plJoltClothWorldCollisionMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Asset,
    Off,
    Static,
    All,

    Default = Asset
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltClothWorldCollisionMode);

/// \brief What a garment is doing this frame.
enum class plJoltClothState : plUInt8
{
  Inactive, ///< No cloth data, no pose, or the device cannot draw simulated cloth.
  Skinned,  ///< Not simulated: the garment is plain skinned.
  Blending, ///< Simulated while the max distance fades in or out.
  Reduced,  ///< Simulated at half rate.
  Full,     ///< Simulated every frame.
};

// FreeList storage: a component owns a soft body and must not be moved in memory.
class PL_JOLTPLUGIN_DLL plJoltCharacterClothComponentManager : public plComponentManager<class plJoltCharacterClothComponent, plBlockStorageType::FreeList>
{
public:
  plJoltCharacterClothComponentManager(plWorld* pWorld);
  ~plJoltCharacterClothComponentManager();

  virtual void Initialize() override;

  /// \brief Solve time of all garments in the last update.
  plTime GetLastSolveTime() const { return m_LastSolveTime; }

  /// \brief Number of garments that stepped in the last update.
  plUInt32 GetNumSimulated() const { return m_uiNumSimulated; }

  /// \brief Measures LOD distances from this point instead of from the cameras of the world's views, and treats every
  /// garment as visible. For worlds without a view, such as a server that still wants cloth, and for tests.
  void SetLodReferencePoint(const plVec3& vPosition);
  void ClearLodReferencePoint();

  /// \brief How many garments may simulate at once in a quality tier of the game settings ("low", "medium", "high",
  /// "max", "mobile"): the cvars Jolt.Cloth.Budget.*. Any other name gets the medium budget.
  static plInt32 GetBudgetForTier(plStringView sQualityPreset);

  /// \brief The budget in force: Jolt.Cloth.MaxSimulatedGarments when it is 0 or more, else the budget of the quality
  /// tier the game settings have active. Negative means no limit.
  plInt32 GetBudget();

private:
  void Update(const plWorldModule::UpdateContext& context);
  void UpdateLod();

  plTime m_LastTierCheck;
  plString m_sQualityTier;

  plDynamicArray<plJoltCharacterClothComponent*> m_ToStep;
  plDynamicArray<plJoltCharacterClothComponent*> m_LodCandidates;
  plTime m_LastSolveTime;
  plUInt32 m_uiNumSimulated = 0;
  plVec3 m_vLodReferencePoint = plVec3::MakeZero();
  bool m_bHasLodReferencePoint = false;
};

/// \brief Simulates the garments of a character: capes, coats, skirts and other cloth that is skinned to its skeleton.
///
/// Add it next to the plAnimatedMeshComponent that draws the garments and give it a cloth asset. Every particle of
/// the cloth may move a painted distance away from where skinning puts it and no further, so the cloth always stays
/// with the character. The component steps after the pose of the frame is known and hands the result to the mesh.
class PL_JOLTPLUGIN_DLL plJoltCharacterClothComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltCharacterClothComponent, plComponent, plJoltCharacterClothComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnSimulationStarted() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plJoltCharacterClothComponent

public:
  plJoltCharacterClothComponent();
  ~plJoltCharacterClothComponent();

  void SetCloth(const plJoltClothResourceHandle& hCloth);
  const plJoltClothResourceHandle& GetCloth() const { return m_hCloth; }

  /// \brief Off leaves the garment skinned. The cloth fades over the given time instead of popping.
  void SetSimulate(bool bSimulate, plTime blendTime = plTime::MakeFromSeconds(0.3)); // [ scriptable ]
  void SetSimulateProperty(bool bSimulate) { SetSimulate(bSimulate); }               // [ property ]
  bool GetSimulate() const { return m_bSimulate; }                                   // [ property ]

  /// \brief Scales every particle's max distance. 0 hands the garment back to animation, for a cutscene or a finisher.
  void SetMaxDistanceScale(float fScale, plTime blendTime = plTime::MakeZero()); // [ scriptable ]
  void SetMaxDistanceScaleProperty(float fScale) { SetMaxDistanceScale(fScale); } // [ property ]
  float GetMaxDistanceScale() const { return m_fMaxDistanceScaleTarget; }         // [ property ]

  void SetWindInfluence(float fInfluence) { m_fWindInfluence = fInfluence; } // [ property ] [ scriptable ]
  float GetWindInfluence() const { return m_fWindInfluence; }                // [ property ]

  /// \brief Snaps the cloth to the skinned pose on the next step. Call after a camera cut so that no settling shows.
  void Reset(); // [ scriptable ]

  plEnum<plJoltClothImportance> m_Importance;              // [ property ]
  float m_fLinearInertia = 0.7f;                           // [ property ] How much of the character's linear acceleration reaches the cloth.
  float m_fAngularInertia = 0.7f;                          // [ property ] The same for turning.
  float m_fGravityFactor = 1.0f;                           // [ property ]
  plEnum<plJoltClothWorldCollisionMode> m_WorldCollision;  // [ property ]
  plUInt8 m_uiCollisionLayer = 0;                          // [ property ] Used for world collision.
  float m_fTeleportDistance = 2.0f;                        // [ property ] A move further than this in one frame is a teleport.
  plEnum<plJoltClothTeleportMode> m_TeleportMode;          // [ property ]

  /// \brief Gives the pose from code, for an owner without a plAnimatedMeshComponent.
  ///
  /// skinTransforms take the bind space of the cloth to the space skinning writes to, per bone of its mesh.
  /// mSkinSpaceToOwner takes that space to the owner object (the root transform of the skeleton).
  void SetPose(plArrayPtr<const plMat4> skinTransforms, const plMat4& mSkinSpaceToOwner);

  /// \brief Gives the world transforms of the joints the body colliders follow, in the order of the asset's colliders.
  /// Only needed together with SetPose(); next to a plAnimatedMeshComponent the component works them out itself.
  void SetColliderJointTransforms(plArrayPtr<const plTransform> jointTransforms);

  /// \brief Draws one of the cloth views (see the cvar Jolt.Cloth.View) over this garment only. 0 follows the cvar.
  /// The cloth window uses it; it is not saved.
  void SetDebugView(plInt32 iView) { m_iDebugView = iView; }
  plInt32 GetDebugView() const { return m_iDebugView; }

  /// \brief The cloth data the garment currently simulates, or nullptr.
  plSharedPtr<plJoltClothData> GetClothData() const { return m_pData; }

  /// \brief The colour the map views give a value from 0 to 1: blue through teal and amber to red.
  static plColor GetHeatColor(float fValue);

  /// \brief How far to move every particle of a sim mesh along its normal to draw a map over the garment: a few
  /// millimetres, so that the overlay does not flicker against the mesh. Draw it at plus and at minus the offset.
  static void GetOverlayOffsets(plArrayPtr<const plVec3> positions, plArrayPtr<const plUInt32> indices, plDynamicArray<plVec3>& out_offsets);

  plJoltClothState GetState() const { return m_State; }
  const plJoltClothInstance& GetInstance() const { return m_Instance; }

  /// \brief What the instance was built from, or nullptr while there is none. The stress test measures against it.
  const plJoltClothSolverSetup* GetSolverSetup() const { return m_pSetup; }

  /// \brief Metres to the nearest camera, as the LOD saw it in the last update.
  float GetLodDistance() const { return m_fLodDistance; }

  /// \brief Solve time of this garment in its last step.
  plTime GetLastStepDuration() const { return m_Instance.GetLastStepDuration(); }

private:
  friend class plJoltCharacterClothComponentManager;

  void OnMsgUpdateLocalBounds(plMsgUpdateLocalBounds& ref_msg);
  void OnAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg);

  /// Gathers this frame's input on the main thread. Returns false when the garment does not step this frame.
  bool PrepareStep(plTime timeStep);
  /// Hands the result of the step to the mesh and updates the bounds. Main thread.
  void FinishStep();

  bool AcquireData();
  bool GatherPose(plMat4& out_mSkinSpaceToOwner);
  void ResolveColliderBones();
  void UpdateColliders(const plMat4& mSkinSpaceToWorld);
  void DrawView(plInt32 iView) const;
  bool WantsSimulation() const;
  void ReleaseInstance();
  void ClearClothOnMesh();
  plJoltClothWorldCollision::Enum GetEffectiveWorldCollision() const;

  plJoltClothResourceHandle m_hCloth;
  bool m_bSimulate = true;
  float m_fWindInfluence = 1.0f;

  // max distance scale: the property, and where the blend currently is
  float m_fMaxDistanceScaleTarget = 1.0f;
  float m_fMaxDistanceScale = 1.0f;
  float m_fMaxDistanceScaleSpeed = 0.0f; ///< per second; 0 jumps

  // how far the cloth is switched on: 1 simulated, 0 skinned
  float m_fSimulateBlend = 1.0f;
  float m_fSimulateBlendSpeed = 0.0f;

  // LOD, decided by the manager every frame: whether the budget lets this garment simulate, and at which rate
  bool m_bLodGranted = true;
  bool m_bLodReduced = false;
  float m_fLodBlend = 1.0f;       ///< 1 simulated, 0 skinned by LOD
  float m_fLodDistance = 0.0f;    ///< metres to the nearest camera
  float m_fSkippedTime = 0.0f;    ///< time of the frames a garment at reduced rate did not step

  bool m_bResetPending = false;
  plInt32 m_iDebugView = 0;
  plJoltClothState m_State = plJoltClothState::Inactive;

  // what the instance was built from
  plSharedPtr<plJoltClothData> m_pData;
  const plJoltClothSolverSetup* m_pSetup = nullptr;
  plUInt32 m_uiResourceChangeCounter = 0;
  plUInt16 m_uiObjectLayer = 0;
  plUInt32 m_uiObjectFilterID = plInvalidIndex;

  plJoltClothInstance m_Instance;
  plJoltClothInstance::StepInput m_StepInput;

  plDynamicArray<plMat4> m_SkinTransforms;
  plMat4 m_mExternalSkinSpaceToOwner = plMat4::MakeIdentity();
  bool m_bHasExternalPose = false;

  // Body colliders follow joints. A joint's pose comes from the skeleton pose of the frame; without one it is
  // recovered from the skinning matrix of its mesh bone.
  struct ColliderBone
  {
    plUInt32 m_uiBone = plInvalidIndex; ///< bone of the mesh, or plInvalidIndex when the joint is not skinned to it
    plMat4 m_mBindPose = plMat4::MakeIdentity();
  };
  plDynamicArray<ColliderBone> m_ColliderBones;
  plDynamicArray<plTransform> m_ColliderTransforms;
  bool m_bHasExternalColliderPose = false;

  const plSkeleton* m_pColliderSkeleton = nullptr; ///< the skeleton m_ColliderJoints was resolved against
  plDynamicArray<plUInt16> m_ColliderJoints;       ///< joint of the skeleton per collider, or plInvalidJointIndex
  plDynamicArray<plMat4> m_ColliderJointPoses;     ///< model space, from the last pose message
  bool m_bHasColliderJointPoses = false;

  plDynamicArray<plClothParticleState> m_Particles;
  plBoundingBox m_LocalBounds = plBoundingBox::MakeInvalid();
  plComponentHandle m_hMeshComponent; ///< the mesh the cloth was last handed to
};
