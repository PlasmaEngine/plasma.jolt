#pragma once

#include <Foundation/Math/Angle.h>
#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

/// \brief How a simulated bone chain behaves: a ponytail, a strap, a pouch or a tail.
struct PL_JOLTPLUGIN_DLL plJoltBoneChainSettings
{
  float m_fStiffness = 0.25f;     ///< 0 hangs freely, 1 holds the animated pose
  float m_fDamping = 0.15f;       ///< how quickly the swinging dies down
  float m_fGravityFactor = 1.0f;
  float m_fLinearInertia = 0.8f;  ///< how much of the character's movement the chain is left behind by
  float m_fDrag = 0.5f;           ///< how strongly wind and the air the character moves through push the chain
  float m_fRadius = 0.03f;        ///< metres every joint keeps away from the body's shapes
  plAngle m_MaxAngle = plAngle::MakeFromDegree(60); ///< how far a bone may bend away from its animated direction
  float m_fTeleportDistance = 2.0f; ///< a move further than this in one step starts the chain over from the pose
};

/// \brief Simulates one chain of joints on top of an animated pose: the joints lag, swing and collide, the bone lengths stay.
///
/// The chain is simulated in the world, as one point per joint. Its first joint stays where the animation has it.
/// After the step the model transforms of the chain's joints are rewritten, and the joints that hang off the chain
/// are carried along.
class PL_JOLTPLUGIN_DLL plJoltBoneChainSolver
{
public:
  /// \brief A joint that is not part of the chain but has a chain joint as an ancestor.
  struct Follower
  {
    plUInt16 m_uiJoint = 0;
    plUInt16 m_uiChainIndex = 0; ///< which joint of the chain carries it
  };

  struct StepInput
  {
    plMat4 m_mModelToWorld = plMat4::MakeIdentity();
    float m_fTimeStep = 1.0f / 60.0f;
    plVec3 m_vGravity = plVec3(0, 0, -9.81f);
    plVec3 m_vWind = plVec3::MakeZero();

    // the shapes of the body the chain collides with, and the world transform of the joint each one follows
    plArrayPtr<const plJoltClothCollider> m_Shapes;
    plArrayPtr<const plTransform> m_ShapeJoints;
    float m_fShapeScale = 1.0f;

    bool m_bReset = false;
  };

  /// \brief joints are the chain's joints from its first joint to its tip, as indices into the model pose.
  void SetChain(plArrayPtr<const plUInt16> joints, plArrayPtr<const Follower> followers);

  bool IsValid() const { return m_Joints.GetCount() >= 1; }

  /// \brief Simulates the chain and rewrites its joints in the model pose, which holds the animated pose on entry.
  void Step(const plJoltBoneChainSettings& settings, const StepInput& input, plArrayPtr<plMat4> ref_modelPose);

  /// \brief The simulated points in the world: one per joint, and one more for the end of the last bone.
  plArrayPtr<const plVec3> GetPoints() const { return m_Points; }

private:
  plDynamicArray<plUInt16> m_Joints;
  plDynamicArray<Follower> m_Followers;

  plDynamicArray<plVec3> m_Points;
  plDynamicArray<plVec3> m_PreviousPoints;
  plDynamicArray<plVec3> m_Animated; ///< where the animation puts each point, in the world
  plVec3 m_vLastAnchor = plVec3::MakeZero();
  bool m_bHasState = false;
};
