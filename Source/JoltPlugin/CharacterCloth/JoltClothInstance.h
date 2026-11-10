#pragma once

#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Time/Time.h>
#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

class plJoltWorldModule;

namespace JPH
{
  class Body;
  class SoftBodySharedSettings;
} // namespace JPH

/// \brief What a teleport does to the cloth.
struct PL_JOLTPLUGIN_DLL plJoltClothTeleportMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Reset,     ///< The cloth snaps to the skinned pose and loses its velocity.
    KeepShape, ///< The cloth moves with the character and keeps its shape and velocity.

    Default = Reset
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltClothTeleportMode);

/// \brief The Jolt data of a garment for one scale and orientation of its character. Instances share it.
struct PL_JOLTPLUGIN_DLL plJoltClothSolverSetup
{
  PL_DISALLOW_COPY_AND_ASSIGN(plJoltClothSolverSetup);

  plJoltClothSolverSetup();
  ~plJoltClothSolverSetup();

  /// \brief Builds the particles, constraints and skinning data.
  ///
  /// mBindToSim takes the bind space of the mesh to the space the cloth is simulated in: the character's own axes at
  /// world scale. It may scale and mirror; rest lengths and face winding follow it.
  plResult Build(const plJoltClothDescriptor& desc, const plMat4& mBindToSim);

  JPH::SoftBodySharedSettings* m_pSettings = nullptr; ///< reference counted, held by this struct
  plMat4 m_mBindToSim = plMat4::MakeIdentity();
  plMat4 m_mSimToBind = plMat4::MakeIdentity();
  float m_fUniformScale = 1.0f; ///< how many metres of simulation space one unit of bind space is
  plDynamicArray<plUInt16> m_Bones; ///< the mesh bones the particles use; the solver's joint i is mesh bone m_Bones[i]
  plUInt32 m_uiNumConstraints = 0;
};

/// \brief One simulated garment: a Jolt soft body that is stepped by hand, right after its character's pose is known.
///
/// The body is never added to the physics system, so the physics step does not touch it. It still collides with the
/// bodies in the system that its object layer and collision group allow.
///
/// The cloth lives in a space that has the character's position as origin and the world's axes. Step() on different
/// instances may run on different threads at the same time, as long as the physics system is not stepping.
class PL_JOLTPLUGIN_DLL plJoltClothInstance
{
  PL_DISALLOW_COPY_AND_ASSIGN(plJoltClothInstance);

public:
  struct StepInput
  {
    plVec3 m_vOwnerPosition = plVec3::MakeZero();
    plQuat m_qOwnerRotation = plQuat::MakeIdentity();

    /// Per bone of the render mesh: bind space to the space skinning writes to (relative to the skeleton root).
    plArrayPtr<const plMat4> m_SkinTransforms;

    float m_fTimeStep = 1.0f / 60.0f;
    plVec3 m_vWind = plVec3::MakeZero(); ///< world space, metres per second
    float m_fMaxDistanceScale = 1.0f;
    float m_fLinearInertia = 0.7f;
    float m_fAngularInertia = 0.7f;
    float m_fGravityFactor = 1.0f;
    float m_fTeleportDistance = 2.0f;
    plEnum<plJoltClothTeleportMode> m_TeleportMode;
    bool m_bReset = false; ///< snap to the skinned pose before stepping
  };

  plJoltClothInstance();
  ~plJoltClothInstance();

  /// \brief Creates the soft body and the bodies of the garment's colliders. The descriptor and the setup must outlive
  /// the instance. fColliderScale is the scale of the character, which its collider shapes grow with.
  plResult Initialize(plJoltWorldModule& ref_module, const plJoltClothDescriptor& desc, const plJoltClothSolverSetup& setup, plUInt16 uiObjectLayer, plUInt32 uiCollisionGroupId, float fColliderScale = 1.0f);
  void Deinitialize();
  bool IsInitialized() const { return m_pBody != nullptr; }

  /// \brief Moves the body colliders to this frame's pose: the world transform of the joint each one follows, in the
  /// order of the descriptor's colliders. Call before Step(), from one thread at a time for the whole physics system.
  void SetColliderJointTransforms(plArrayPtr<const plTransform> jointTransforms);

  plUInt32 GetNumColliders() const { return m_ColliderBodies.GetCount(); }

  void Step(const StepInput& input);

  /// \brief The particles in the space skinning writes to, with the rotation of each particle's surroundings since the
  /// bind pose. This is what plComputeSkinningPool::SetClothForFrame takes.
  void GetRenderParticles(plDynamicArray<plClothParticleState>& out_particles) const;

  /// \brief Particle positions in simulation space: relative to the owner position, in world axes.
  void GetSimPositions(plDynamicArray<plVec3>& out_positions) const;

  /// \brief Where skinning alone would put each particle, in simulation space. Valid after the first Step().
  void GetSkinnedPositions(plDynamicArray<plVec3>& out_positions) const;

  /// \brief Bounds of the particles in simulation space.
  plBoundingBox GetSimBounds() const;

  plUInt32 GetParticleCount() const { return m_uiNumParticles; }
  plTime GetLastStepDuration() const { return m_LastStepDuration; }
  plUInt32 GetLastIterations() const { return m_uiLastIterations; }
  bool DidResetInLastStep() const { return m_bResetInLastStep; }

private:
  void FollowOwner(const StepInput& input, bool bKeepShape);
  void ApplyAerodynamics(const StepInput& input, const plVec3& vOwnerVelocity);
  void ComputeSkinnedPositions(float fTimeStep, bool bReset);
  void ClampToEnvelope(float fMaxDistanceScale);

  plJoltWorldModule* m_pModule = nullptr;
  const plJoltClothDescriptor* m_pDesc = nullptr;
  const plJoltClothSolverSetup* m_pSetup = nullptr;
  JPH::Body* m_pBody = nullptr;

  plUInt32 m_uiNumParticles = 0;
  bool m_bHasState = false;
  bool m_bResetInLastStep = false;
  plVec3 m_vLastOwnerPosition = plVec3::MakeZero();
  plQuat m_qLastOwnerRotation = plQuat::MakeIdentity();
  plVec3 m_vLastOwnerVelocity = plVec3::MakeZero();
  bool m_bHasOwnerVelocity = false;
  plTime m_LastStepDuration;
  plUInt32 m_uiLastIterations = 0;

  /// Bind space to simulation space of the last step, per bone of the solver setup.
  plDynamicArray<plMat4> m_BoneToSim;

  // Per particle: the solver joints it is skinned to with their normalized weights, and where the last pose puts it.
  plDynamicArray<plVec4U16> m_SkinJoints;
  plDynamicArray<plVec4> m_SkinWeights;
  plDynamicArray<plVec3> m_Skinned;
  plDynamicArray<plVec3> m_SkinnedVelocity;

  /// Kinematic bodies in the ClothCollider layer, one per collider of the descriptor (invalid where creation failed).
  plDynamicArray<plUInt32> m_ColliderBodies;
  float m_fColliderScale = 1.0f;

  // for the rotation of each particle's surroundings: a second particle to aim the tangent at, and the bind-pose frame
  plDynamicArray<plUInt16> m_FrameNeighbor;
  plDynamicArray<plQuat> m_RestFrameInverse;
};
