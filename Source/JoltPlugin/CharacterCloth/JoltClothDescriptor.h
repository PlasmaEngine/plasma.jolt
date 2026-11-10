#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Mat4.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Strings/HashedString.h>
#include <Foundation/Strings/String.h>
#include <JoltPlugin/CharacterCloth/JoltClothDeclarations.h>
#include <JoltPlugin/JoltPluginDLL.h>
#include <RendererCore/Meshes/ClothDeformation.h>

class plStreamReader;
class plStreamWriter;

/// \brief How a garment's material behaves. Stretch, shear, bend and damping are resistances from 0 to 1.
struct PL_JOLTPLUGIN_DLL plJoltClothFabric
{
  float m_fDensity = 0.15f;    ///< kg per square metre
  float m_fStretch = 0.95f;    ///< 1 does not stretch at all
  float m_fShear = 0.9f;       ///< 1 keeps the weave square
  float m_fBend = 0.2f;        ///< 0 folds freely, 1 is stiff like a board
  float m_fDamping = 0.2f;     ///< how quickly motion dies down
  float m_fDrag = 0.7f;        ///< how strongly air pushes against the surface
  float m_fLift = 0.2f;        ///< how strongly air moving along the surface lifts it
  float m_fFriction = 0.2f;    ///< against the shapes it touches
  float m_fThickness = 0.01f;  ///< metres the cloth stays away from colliders

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);
};

/// \brief A shape on a skeleton joint that the garment collides with.
struct PL_JOLTPLUGIN_DLL plJoltClothCollider
{
  enum class Type : plUInt8
  {
    Sphere,
    Capsule, ///< along the local X axis, like the skeleton's joint shapes
    Box,
  };

  plHashedString m_sJoint;
  Type m_Type = Type::Capsule;
  plTransform m_Transform = plTransform::MakeIdentity(); ///< relative to the joint; the scale is not used
  float m_fRadius = 0.05f;
  float m_fLength = 0.2f;                    ///< capsule: distance between the two sphere centres
  plVec3 m_vHalfExtents = plVec3(0.05f);     ///< box

  /// Where the joint is in the bind pose, in the bind space of the cloth. The simulation never reads it: tools use
  /// it to place the collider when there is no pose, such as the stress test's motion script.
  plTransform m_JointBindPose = plTransform::MakeIdentity();

  /// \brief The collider's transform for a joint at jointTransform, with shapes of a character scaled by fScale.
  plTransform GetShapeTransform(const plTransform& jointTransform, float fScale = 1.0f) const
  {
    return plTransform(jointTransform.m_vPosition + jointTransform.m_qRotation * (m_Transform.m_vPosition * fScale), jointTransform.m_qRotation * m_Transform.m_qRotation);
  }

  /// \brief Moves a point that is closer than fMargin to the shape out to that distance. Returns whether it moved.
  /// shapeTransform is what GetShapeTransform() gave, in the space of the point.
  bool PushOut(const plTransform& shapeTransform, float fScale, float fMargin, plVec3& ref_vPoint) const;

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);
};

/// \brief How a garment is stepped.
struct PL_JOLTPLUGIN_DLL plJoltClothSimulationSettings
{
  float m_fStepRate = 60.0f;  ///< Hz. Together with the iterations this fixes the length of a solver sub step.
  plUInt8 m_uiIterations = 8; ///< solver sub steps per step at the step rate
  bool m_bTethers = true;     ///< long-range attachments that stop free vertices from drifting away from the pinned ones
  float m_fTetherSlack = 1.03f;
  plEnum<plJoltClothWorldCollision> m_WorldCollision;
  float m_fGravityFactor = 1.0f;
  float m_fFullRateDistance = 14.0f; ///< metres from the camera up to which the garment steps every frame
  float m_fSimulateDistance = 30.0f; ///< metres beyond which the garment is plain skinned

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);
};

/// \brief Everything a garment needs at runtime: the sim mesh, its maps, how the render mesh follows it, and its settings.
///
/// The sim mesh is in the bind space of the render mesh. Distances in the maps are metres.
struct PL_JOLTPLUGIN_DLL plJoltClothDescriptor
{
  // Sim mesh, one entry per particle.
  plDynamicArray<plVec3> m_Positions;
  plDynamicArray<plVec4U16> m_BoneIndices; ///< bones of the render mesh, see plMeshResourceDescriptor::BoneData::m_uiBoneIndex
  plDynamicArray<plVec4> m_BoneWeights;

  // Maps, one entry per particle.
  plDynamicArray<float> m_MaxDistance;      ///< 0 pins the particle to its skinned position
  plDynamicArray<float> m_BackstopDistance; ///< how far the particle may move inward, against its normal
  plDynamicArray<float> m_BackstopRadius;
  plDynamicArray<float> m_StiffnessScale;

  plDynamicArray<plUInt32> m_Indices; ///< triangle list

  /// One entry per vertex of the render mesh's vertex buffer.
  plDynamicArray<plClothVertexBinding> m_Bindings;

  plJoltClothFabric m_Fabric;
  plJoltClothSimulationSettings m_Simulation;
  plDynamicArray<plJoltClothCollider> m_Colliders;

  /// The mesh the bindings were made for, as its resource ID. Empty for cloth built in code.
  plString m_sMesh;

  /// Bind space to the character's object space, as the skeleton of the mesh has it. The simulation takes this from
  /// the pose; tools that have no pose, such as the stress test's motion script, use this one.
  plTransform m_RootTransform = plTransform::MakeIdentity();

  plUInt32 GetParticleCount() const { return m_Positions.GetCount(); }
  plUInt32 GetTriangleCount() const { return m_Indices.GetCount() / 3; }

  /// \brief Checks that every array has the right size and every index is in range. Logs what is wrong.
  plResult Validate() const;

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);

  plUInt64 GetHeapMemoryUsage() const;
};
