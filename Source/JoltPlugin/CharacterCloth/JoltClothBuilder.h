#pragma once

#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

class plMeshResourceDescriptor;
struct plSkeletonResourceDescriptor;

/// \brief One triangle mesh that cloth is built from: a garment of the render mesh, or a proxy that is simulated in its place.
///
/// All per-vertex arrays have one entry per vertex of the piece. A map that is left empty uses the default from the options.
struct PL_JOLTPLUGIN_DLL plJoltClothBuildPiece
{
  plArrayPtr<const plVec3> m_Positions;
  plArrayPtr<const plUInt32> m_Indices; ///< triangle list, local to the piece
  plArrayPtr<const plVec4U16> m_BoneIndices;
  plArrayPtr<const plVec4> m_BoneWeights;

  plArrayPtr<const float> m_MaxDistance;
  plArrayPtr<const float> m_BackstopDistance;
  plArrayPtr<const float> m_BackstopRadius;
  plArrayPtr<const float> m_StiffnessScale;

  /// Where the piece's vertices start in the render mesh's vertex buffer. plInvalidIndex for a proxy.
  plUInt32 m_uiFirstRenderVertex = plInvalidIndex;

  /// Index of the piece that is simulated in place of this one, or plInvalidIndex when the piece simulates itself.
  plUInt32 m_uiProxyPiece = plInvalidIndex;

  bool IsProxy() const { return m_uiFirstRenderVertex == plInvalidIndex; }
};

struct PL_JOLTPLUGIN_DLL plJoltClothBuildOptions
{
  float m_fWeldDistance = 0.0001f; ///< vertices closer than this become one particle (bind-space units)

  float m_fDefaultMaxDistance = 0.3f;
  float m_fDefaultBackstopDistance = 0.02f;
  float m_fDefaultBackstopRadius = 0.4f;
  float m_fDefaultStiffnessScale = 1.0f;

  /// A vertex whose particles may move at least this far follows the simulation fully. Below that it fades towards its
  /// skinned position, so pinned edges meet the rest of the character without a seam.
  float m_fBlendDistance = 0.02f;

  /// Number of vertices in the render mesh's vertex buffer. The bindings get one entry for each.
  plUInt32 m_uiRenderVertexCount = 0;

  /// BuildFromMesh: off ignores the masks the mesh was imported with, so every map starts at its default.
  bool m_bUseImportedMasks = true;
};

/// \brief Which shapes of a skeleton become body colliders of a garment.
struct PL_JOLTPLUGIN_DLL plJoltClothSkeletonColliderOptions
{
  plArrayPtr<const plString> m_Joints; ///< take the shapes of these joints; empty takes every shape the cloth can reach
  float m_fInflate = 0.0f;             ///< metres added to every radius and half extent
  float m_fReachMargin = 0.05f;        ///< with m_Joints empty: a shape is in reach when a free particle can come this close to it
  bool m_bAllShapes = false;           ///< take every shape, whatever m_Joints says. The stress test checks the cloth against all of them.
};

/// \brief Maps painted in the cloth window.
///
/// Each sample remembers the bind-space position it was painted at, so the maps survive a reimport that changes the
/// sim mesh. A map that was never painted is empty.
struct PL_JOLTPLUGIN_DLL plJoltClothPaintedMaps
{
  plDynamicArray<plVec3> m_Positions;
  plDynamicArray<float> m_MaxDistance;
  plDynamicArray<float> m_BackstopDistance;
  plDynamicArray<float> m_BackstopRadius;
  plDynamicArray<float> m_StiffnessScale;

  bool IsEmpty() const { return m_Positions.IsEmpty() || (m_MaxDistance.IsEmpty() && m_BackstopDistance.IsEmpty() && m_BackstopRadius.IsEmpty() && m_StiffnessScale.IsEmpty()); }

  void Clear();

  plResult Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);
};

/// \brief What the builder found. Loose vertices and non-manifold edges still simulate, but usually point at a modelling mistake.
struct PL_JOLTPLUGIN_DLL plJoltClothBuildReport
{
  plUInt32 m_uiSourceVertices = 0;
  plUInt32 m_uiParticles = 0;
  plUInt32 m_uiTriangles = 0;
  plUInt32 m_uiDegenerateTriangles = 0; ///< dropped: two of their corners welded into one particle
  plUInt32 m_uiLooseVertices = 0;       ///< particles without a triangle; they are pinned
  plUInt32 m_uiBoundaryEdges = 0;
  plUInt32 m_uiNonManifoldEdges = 0; ///< edges shared by more than two triangles
  plUInt32 m_uiBoundVertices = 0;    ///< render vertices that follow the cloth
};

/// \brief Turns garment meshes into a cloth descriptor: welds seams, builds the sim mesh and binds the render vertices to it.
class PL_JOLTPLUGIN_DLL plJoltClothBuilder
{
public:
  /// \brief Builds the sim mesh from every piece that simulates itself and from every proxy, and binds the garments.
  ///
  /// A garment without a proxy is bound vertex to particle. A garment with a proxy is bound to the closest triangle of
  /// that proxy. Fabric, simulation settings and colliders of out_desc are left untouched.
  static plResult Build(plArrayPtr<const plJoltClothBuildPiece> pieces, const plJoltClothBuildOptions& options, plJoltClothDescriptor& out_desc, plJoltClothBuildReport* out_pReport = nullptr);

  /// \brief Builds from the cloth pieces a mesh was imported with (plMeshResourceDescriptor::GetClothSubMeshes()).
  ///
  /// garmentNames selects pieces by name; empty takes all. A proxy piece is used for the garment with the same name
  /// after the prefix, e.g. CLOTHPROXY_Cape for CLOTH_Cape. Imported pin masks become max distances: a fully pinned
  /// vertex gets 0, a free one fMaxDistance. A painted green channel scales the default backstop distance, a painted
  /// blue channel the default stiffness scale.
  static plResult BuildFromMesh(const plMeshResourceDescriptor& mesh, plArrayPtr<const plString> garmentNames, float fMaxDistance, const plJoltClothBuildOptions& options, plJoltClothDescriptor& out_desc, plJoltClothBuildReport* out_pReport = nullptr);

  /// \brief Recomputes how strongly each bound render vertex follows the simulation. Call after changing max distances.
  static void UpdateBindingBlend(plJoltClothDescriptor& ref_desc, float fBlendDistance);

  /// \brief Adds the sphere, capsule and box shapes of a skeleton as body colliders and returns how many it added.
  ///
  /// The shapes are the ones ragdolls and hitboxes use, so a character that has those needs no extra setup.
  /// Convex shapes are skipped.
  static plUInt32 AddSkeletonColliders(const plSkeletonResourceDescriptor& skeleton, const plJoltClothSkeletonColliderOptions& options, plJoltClothDescriptor& ref_desc);

  /// \brief Notes for every collider where its joint is in the skeleton's rest pose (plJoltClothCollider::m_JointBindPose).
  static void SetJointBindPoses(const plSkeletonResourceDescriptor& skeleton, plJoltClothDescriptor& ref_desc);

  /// \brief Gives every particle the painted values of the sample closest to it, then updates the binding blend.
  static void ApplyPaintedMaps(const plJoltClothPaintedMaps& maps, float fBlendDistance, plJoltClothDescriptor& ref_desc);

  /// \brief Copies the maps of a built cloth into painted maps, as the starting point for painting.
  static void ExtractPaintedMaps(const plJoltClothDescriptor& desc, plJoltClothPaintedMaps& out_maps);
};
