#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec3.h>
#include <JoltPlugin/JoltPluginDLL.h>

/// \brief What the paint tools need to know about a sim mesh: its particles, triangles, and which particles share an edge.
struct PL_JOLTPLUGIN_DLL plJoltClothMapTopology
{
  plDynamicArray<plVec3> m_Positions;
  plDynamicArray<plUInt32> m_Indices; ///< triangle list

  plDynamicArray<plUInt32> m_NeighbourStart; ///< per particle the first of its entries in m_Neighbours, and one entry more at the end
  plDynamicArray<plUInt32> m_Neighbours;
  plDynamicArray<plUInt32> m_Island; ///< per particle the connected piece it belongs to
  plUInt32 m_uiIslandCount = 0;

  void Build(plArrayPtr<const plVec3> positions, plArrayPtr<const plUInt32> indices);

  plArrayPtr<const plUInt32> GetNeighbours(plUInt32 uiParticle) const
  {
    return m_Neighbours.GetArrayPtr().GetSubArray(m_NeighbourStart[uiParticle], m_NeighbourStart[uiParticle + 1] - m_NeighbourStart[uiParticle]);
  }
};

/// \brief A round brush on the surface. Inside the hardness fraction of the radius it has full strength, then fades to 0.
struct PL_JOLTPLUGIN_DLL plJoltClothBrush
{
  plVec3 m_vCenter = plVec3::MakeZero();
  float m_fRadius = 0.1f;
  float m_fStrength = 1.0f;
  float m_fHardness = 0.5f;

  /// \brief 0..m_fStrength at this position.
  float GetWeight(const plVec3& vPosition) const;
};

/// \brief The operations of the cloth window's paint tools. Each works on one map: a float per particle.
class PL_JOLTPLUGIN_DLL plJoltClothMapTools
{
public:
  struct RayHit
  {
    plVec3 m_vPosition = plVec3::MakeZero();
    float m_fDistance = 0.0f;
    plUInt32 m_uiTriangle = plInvalidIndex;
    plUInt32 m_uiClosestParticle = plInvalidIndex; ///< the corner of the triangle nearest to the hit
  };

  /// \brief The closest hit of a ray with the sim mesh, front or back side. vDirection must be normalized.
  static bool RayCast(const plJoltClothMapTopology& topology, const plVec3& vStart, const plVec3& vDirection, RayHit& out_hit);

  /// \brief Raises the stroke weight of every particle under the brush to the brush's weight there.
  ///
  /// A stroke keeps one weight per particle, so holding the brush still does not keep adding.
  /// Returns whether a weight changed.
  static bool AccumulateStroke(const plJoltClothMapTopology& topology, const plJoltClothBrush& brush, plArrayPtr<float> ref_weights);

  /// \brief The map of a paint stroke: the values at the start of the stroke, moved towards fValue by the stroke weights.
  static void ApplyPaint(plArrayPtr<const float> startValues, plArrayPtr<const float> weights, float fValue, plArrayPtr<float> ref_map);

  /// \brief Moves every value under the brush towards the average of its neighbours. Adds up when called again.
  static void Smooth(const plJoltClothMapTopology& topology, const plJoltClothBrush& brush, plArrayPtr<float> ref_map);

  /// \brief Sets one connected piece of the mesh to a value. plInvalidIndex sets every piece.
  static void Fill(const plJoltClothMapTopology& topology, plUInt32 uiIsland, float fValue, plArrayPtr<float> ref_map);

  /// \brief A linear ramp from fFromValue at vFrom to fToValue at vTo over one connected piece, or over all of them.
  static void Gradient(const plJoltClothMapTopology& topology, plUInt32 uiIsland, const plVec3& vFrom, const plVec3& vTo, float fFromValue, float fToValue, plArrayPtr<float> ref_map);

  /// \brief A first max distance map: pins the top of every piece and lets the distance grow along the surface.
  ///
  /// The top fPinnedFraction of a piece's height along vUp gets 0. From there the value rises with the distance
  /// measured along the mesh, up to fMaxDistance at the point furthest away.
  static void AutoMask(const plJoltClothMapTopology& topology, const plVec3& vUp, float fPinnedFraction, float fMaxDistance, plArrayPtr<float> ref_map);
};
