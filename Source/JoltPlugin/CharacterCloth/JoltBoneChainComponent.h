#pragma once

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <JoltPlugin/CharacterCloth/JoltBoneChainSolver.h>
#include <RendererCore/AnimationSystem/Declarations.h>

struct plSkeletonResourceDescriptor;

// FreeList storage: a component owns its solvers and must not be moved in memory.
class PL_JOLTPLUGIN_DLL plJoltBoneChainComponentManager : public plComponentManager<class plJoltBoneChainComponent, plBlockStorageType::FreeList>
{
public:
  plJoltBoneChainComponentManager(plWorld* pWorld);

  virtual void Initialize() override;

private:
  void Update(const plWorldModule::UpdateContext& context);
};

/// \brief Lets chains of joints swing on top of the animation: a ponytail, a strap, a pouch, a tail.
///
/// Add it next to the plAnimatedMeshComponent and name the first joint of every chain. Each chain runs from that
/// joint along its first child to the last joint. The chains lag behind the character's movement, hang under
/// gravity, follow the wind and stay out of the skeleton's shapes, and the bones keep their length. It costs a few
/// microseconds a chain, so it also suits what is too small or too far away for simulated cloth.
class PL_JOLTPLUGIN_DLL plJoltBoneChainComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltBoneChainComponent, plComponent, plJoltBoneChainComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnSimulationStarted() override;

  //////////////////////////////////////////////////////////////////////////
  // plJoltBoneChainComponent

public:
  plJoltBoneChainComponent();
  ~plJoltBoneChainComponent();

  plHybridArray<plString, 2> m_Chains; // [ property ] The first joint of every chain.

  float m_fStiffness = 0.25f;                       // [ property ] 0 hangs freely, 1 holds the animated pose.
  float m_fDamping = 0.15f;                         // [ property ]
  float m_fGravityFactor = 1.0f;                    // [ property ]
  float m_fLinearInertia = 0.8f;                    // [ property ] How much of the character's movement leaves the chains behind.
  float m_fWindInfluence = 1.0f;                    // [ property ]
  float m_fRadius = 0.03f;                          // [ property ] How far the joints stay away from the body's shapes.
  plAngle m_MaxAngle = plAngle::MakeFromDegree(60); // [ property ] How far a bone may bend away from its animated direction.
  bool m_bCollideWithBody = true;                   // [ property ] Collide with the skeleton's shapes.
  float m_fTeleportDistance = 2.0f;                 // [ property ]

  /// A garment that is skinned to joints of its own can have its chains swing only while its Character Cloth is not
  /// simulating: far away, off screen or out of budget. Near the camera the cloth moves it, further away the chains
  /// do, for a fraction of the cost.
  bool m_bStandInForCloth = false; // [ property ]

  /// \brief Puts the chains back on the animated pose. Call after a camera cut so that no settling shows.
  void Reset(); // [ scriptable ]

  /// \brief Names the skeleton, for an owner that has no component to ask for it. Next to a plAnimatedMeshComponent
  /// the component finds it by itself.
  void SetSkeleton(const plSkeletonResourceHandle& hSkeleton);

  /// \brief The simulated points of a chain in the world, for debugging and tests.
  plArrayPtr<const plVec3> GetChainPoints(plUInt32 uiChain) const;
  plUInt32 GetChainCount() const { return m_Solvers.GetCount(); }

private:
  friend class plJoltBoneChainComponentManager;

  void OnAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg);
  void Update(plTime timeStep);
  void BuildChains(const plSkeletonResourceDescriptor& skeleton);

  plSkeletonResourceHandle m_hSkeleton;
  plUInt32 m_uiSkeletonChangeCounter = 0;
  bool m_bChainsBuilt = false;

  // the animated pose of the frame, as the animation sent it
  plDynamicArray<plMat4> m_AnimatedPose;
  plTransform m_RootTransform = plTransform::MakeIdentity();
  bool m_bHasPose = false;
  bool m_bSendingPose = false;
  bool m_bResetPending = false;

  plDynamicArray<plMat4> m_Pose; ///< the pose with the chains simulated, as it is sent on
  plDynamicArray<plJoltBoneChainSolver> m_Solvers;

  // the skeleton's shapes that the chains collide with
  plDynamicArray<plJoltClothCollider> m_Shapes;
  plDynamicArray<plUInt16> m_ShapeJointIndices;
  plDynamicArray<plTransform> m_ShapeJoints;
};
