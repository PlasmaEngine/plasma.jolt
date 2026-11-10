#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/IO/Stream.h>
#include <JoltPlugin/CharacterCloth/JoltClothBuilder.h>
#include <RendererCore/AnimationSystem/Declarations.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

namespace
{
  plUInt64 MakeCellKey(plInt32 x, plInt32 y, plInt32 z)
  {
    return (static_cast<plUInt64>(x & 0x1FFFFF) << 42) | (static_cast<plUInt64>(y & 0x1FFFFF) << 21) | static_cast<plUInt64>(z & 0x1FFFFF);
  }

  plUInt64 MakeEdgeKey(plUInt32 a, plUInt32 b)
  {
    return a < b ? (static_cast<plUInt64>(a) << 32) | b : (static_cast<plUInt64>(b) << 32) | a;
  }

  float MapValue(plArrayPtr<const float> map, plUInt32 uiIndex, float fDefault)
  {
    return map.IsEmpty() ? fDefault : map[uiIndex];
  }

  /// Merges vertices that lie within the weld distance of each other into one particle of the descriptor.
  class ParticleWelder
  {
  public:
    ParticleWelder(float fWeldDistance, plJoltClothDescriptor& ref_desc)
      : m_Desc(ref_desc)
    {
      m_fWeldDistance = plMath::Max(fWeldDistance, 1.0e-6f);
      m_fInvCellSize = 1.0f / m_fWeldDistance;
    }

    struct Vertex
    {
      plVec3 m_vPosition;
      plVec4U16 m_BoneIndices;
      plVec4 m_BoneWeights;
      float m_fMaxDistance;
      float m_fBackstopDistance;
      float m_fBackstopRadius;
      float m_fStiffnessScale;
    };

    plUInt32 Add(const Vertex& v)
    {
      const plInt32 cx = static_cast<plInt32>(plMath::Floor(v.m_vPosition.x * m_fInvCellSize));
      const plInt32 cy = static_cast<plInt32>(plMath::Floor(v.m_vPosition.y * m_fInvCellSize));
      const plInt32 cz = static_cast<plInt32>(plMath::Floor(v.m_vPosition.z * m_fInvCellSize));

      // cells are as large as the weld distance, so a match is in this cell or one of its neighbours
      for (plInt32 z = cz - 1; z <= cz + 1; ++z)
      {
        for (plInt32 y = cy - 1; y <= cy + 1; ++y)
        {
          for (plInt32 x = cx - 1; x <= cx + 1; ++x)
          {
            const plHybridArray<plUInt32, 2>* pCell = m_Grid.GetValue(MakeCellKey(x, y, z));
            if (pCell == nullptr)
              continue;

            for (plUInt32 uiParticle : *pCell)
            {
              if ((m_Desc.m_Positions[uiParticle] - v.m_vPosition).GetLengthSquared() <= plMath::Square(m_fWeldDistance))
              {
                Merge(uiParticle, v);
                return uiParticle;
              }
            }
          }
        }
      }

      const plUInt32 uiParticle = m_Desc.m_Positions.GetCount();
      m_Desc.m_Positions.PushBack(v.m_vPosition);
      m_Desc.m_BoneIndices.PushBack(v.m_BoneIndices);
      m_Desc.m_BoneWeights.PushBack(v.m_BoneWeights);
      m_Desc.m_MaxDistance.PushBack(v.m_fMaxDistance);
      m_Desc.m_BackstopDistance.PushBack(v.m_fBackstopDistance);
      m_Desc.m_BackstopRadius.PushBack(v.m_fBackstopRadius);
      m_Desc.m_StiffnessScale.PushBack(v.m_fStiffnessScale);

      m_Grid[MakeCellKey(cx, cy, cz)].PushBack(uiParticle);
      return uiParticle;
    }

  private:
    // Two sides of a seam can carry different mask values: the particle takes the more restrictive one.
    void Merge(plUInt32 uiParticle, const Vertex& v)
    {
      m_Desc.m_MaxDistance[uiParticle] = plMath::Min(m_Desc.m_MaxDistance[uiParticle], v.m_fMaxDistance);
      m_Desc.m_BackstopDistance[uiParticle] = plMath::Min(m_Desc.m_BackstopDistance[uiParticle], v.m_fBackstopDistance);
      m_Desc.m_BackstopRadius[uiParticle] = plMath::Min(m_Desc.m_BackstopRadius[uiParticle], v.m_fBackstopRadius);
      m_Desc.m_StiffnessScale[uiParticle] = plMath::Max(m_Desc.m_StiffnessScale[uiParticle], v.m_fStiffnessScale);
    }

    plJoltClothDescriptor& m_Desc;
    float m_fWeldDistance = 0.0f;
    float m_fInvCellSize = 0.0f;
    plHashTable<plUInt64, plHybridArray<plUInt32, 2>> m_Grid;
  };

  /// Closest point to p on the triangle (a, b, c), as barycentric weights of a and b. The weight of c is the remainder.
  plVec3 ClosestPointOnTriangle(const plVec3& p, const plVec3& a, const plVec3& b, const plVec3& c, float& out_fWeightA, float& out_fWeightB)
  {
    const plVec3 ab = b - a;
    const plVec3 ac = c - a;
    const plVec3 ap = p - a;

    const float d1 = ab.Dot(ap);
    const float d2 = ac.Dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
      out_fWeightA = 1.0f;
      out_fWeightB = 0.0f;
      return a;
    }

    const plVec3 bp = p - b;
    const float d3 = ab.Dot(bp);
    const float d4 = ac.Dot(bp);
    if (d3 >= 0.0f && d4 <= d3)
    {
      out_fWeightA = 0.0f;
      out_fWeightB = 1.0f;
      return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
      const float v = d1 / (d1 - d3);
      out_fWeightA = 1.0f - v;
      out_fWeightB = v;
      return a + ab * v;
    }

    const plVec3 cp = p - c;
    const float d5 = ab.Dot(cp);
    const float d6 = ac.Dot(cp);
    if (d6 >= 0.0f && d5 <= d6)
    {
      out_fWeightA = 0.0f;
      out_fWeightB = 0.0f;
      return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
      const float w = d2 / (d2 - d6);
      out_fWeightA = 1.0f - w;
      out_fWeightB = 0.0f;
      return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
      const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      out_fWeightA = 0.0f;
      out_fWeightB = 1.0f - w;
      return b + (c - b) * w;
    }

    const float fDenom = 1.0f / (va + vb + vc);
    const float v = vb * fDenom;
    const float w = vc * fDenom;
    out_fWeightA = 1.0f - v - w;
    out_fWeightB = v;
    return a + ab * v + ac * w;
  }

  struct PieceResult
  {
    plDynamicArray<plUInt32> m_ParticleOfVertex;
    plUInt32 m_uiFirstTriangle = 0;
    plUInt32 m_uiNumTriangles = 0;
  };

  bool PieceIsValid(const plJoltClothBuildPiece& piece)
  {
    const plUInt32 uiVertices = piece.m_Positions.GetCount();

    if (uiVertices == 0 || piece.m_Indices.GetCount() % 3 != 0)
      return false;

    if (piece.m_BoneIndices.GetCount() != uiVertices || piece.m_BoneWeights.GetCount() != uiVertices)
      return false;

    for (plArrayPtr<const float> map : {piece.m_MaxDistance, piece.m_BackstopDistance, piece.m_BackstopRadius, piece.m_StiffnessScale})
    {
      if (!map.IsEmpty() && map.GetCount() != uiVertices)
        return false;
    }

    for (plUInt32 uiIndex : piece.m_Indices)
    {
      if (uiIndex >= uiVertices)
        return false;
    }

    return true;
  }

  void WeldPiece(const plJoltClothBuildPiece& piece, const plJoltClothBuildOptions& options, ParticleWelder& ref_welder, plJoltClothDescriptor& ref_desc, PieceResult& out_result, plHashSet<plUInt64>& ref_triangles, plJoltClothBuildReport& ref_report)
  {
    const plUInt32 uiVertices = piece.m_Positions.GetCount();
    out_result.m_ParticleOfVertex.SetCountUninitialized(uiVertices);

    for (plUInt32 v = 0; v < uiVertices; ++v)
    {
      ParticleWelder::Vertex vertex;
      vertex.m_vPosition = piece.m_Positions[v];
      vertex.m_BoneIndices = piece.m_BoneIndices[v];
      vertex.m_BoneWeights = piece.m_BoneWeights[v];
      vertex.m_fMaxDistance = plMath::Max(0.0f, MapValue(piece.m_MaxDistance, v, options.m_fDefaultMaxDistance));
      vertex.m_fBackstopDistance = plMath::Max(0.0f, MapValue(piece.m_BackstopDistance, v, options.m_fDefaultBackstopDistance));
      vertex.m_fBackstopRadius = plMath::Max(0.01f, MapValue(piece.m_BackstopRadius, v, options.m_fDefaultBackstopRadius));
      vertex.m_fStiffnessScale = plMath::Max(0.0f, MapValue(piece.m_StiffnessScale, v, options.m_fDefaultStiffnessScale));

      out_result.m_ParticleOfVertex[v] = ref_welder.Add(vertex);
    }

    ref_report.m_uiSourceVertices += uiVertices;
    out_result.m_uiFirstTriangle = ref_desc.m_Indices.GetCount() / 3;

    for (plUInt32 i = 0; i + 2 < piece.m_Indices.GetCount(); i += 3)
    {
      const plUInt32 a = out_result.m_ParticleOfVertex[piece.m_Indices[i]];
      const plUInt32 b = out_result.m_ParticleOfVertex[piece.m_Indices[i + 1]];
      const plUInt32 c = out_result.m_ParticleOfVertex[piece.m_Indices[i + 2]];

      if (a == b || b == c || a == c)
      {
        ++ref_report.m_uiDegenerateTriangles;
        continue;
      }

      // The inner and outer shell of a garment weld into the same particles: keep one of the two triangles.
      plUInt32 sorted[3] = {a, b, c};
      if (sorted[0] > sorted[1])
        plMath::Swap(sorted[0], sorted[1]);
      if (sorted[1] > sorted[2])
        plMath::Swap(sorted[1], sorted[2]);
      if (sorted[0] > sorted[1])
        plMath::Swap(sorted[0], sorted[1]);

      const plUInt64 uiTriangleKey = (static_cast<plUInt64>(sorted[0]) << 42) ^ (static_cast<plUInt64>(sorted[1]) << 21) ^ static_cast<plUInt64>(sorted[2]);
      if (ref_triangles.Contains(uiTriangleKey))
        continue;

      ref_triangles.Insert(uiTriangleKey);

      ref_desc.m_Indices.PushBack(a);
      ref_desc.m_Indices.PushBack(b);
      ref_desc.m_Indices.PushBack(c);
    }

    out_result.m_uiNumTriangles = ref_desc.m_Indices.GetCount() / 3 - out_result.m_uiFirstTriangle;
  }

  void BindToParticles(const plJoltClothBuildPiece& piece, const PieceResult& result, plJoltClothDescriptor& ref_desc, plJoltClothBuildReport& ref_report)
  {
    for (plUInt32 v = 0; v < piece.m_Positions.GetCount(); ++v)
    {
      const plUInt32 uiParticle = result.m_ParticleOfVertex[v];

      plClothVertexBinding& binding = ref_desc.m_Bindings[piece.m_uiFirstRenderVertex + v];
      binding.m_vOffset = piece.m_Positions[v] - ref_desc.m_Positions[uiParticle];
      binding.m_uiParticle0 = uiParticle;
      binding.m_uiParticle1 = static_cast<plUInt16>(uiParticle);
      binding.m_uiParticle2 = static_cast<plUInt16>(uiParticle);
      binding.m_fWeight0 = 1.0f;
      binding.m_fWeight1 = 0.0f;

      ++ref_report.m_uiBoundVertices;
    }
  }

  void BindToProxy(const plJoltClothBuildPiece& piece, const PieceResult& proxy, plJoltClothDescriptor& ref_desc, plJoltClothBuildReport& ref_report)
  {
    for (plUInt32 v = 0; v < piece.m_Positions.GetCount(); ++v)
    {
      const plVec3 vPosition = piece.m_Positions[v];

      float fBestDistanceSqr = plMath::MaxValue<float>();
      plClothVertexBinding best;

      for (plUInt32 t = proxy.m_uiFirstTriangle; t < proxy.m_uiFirstTriangle + proxy.m_uiNumTriangles; ++t)
      {
        const plUInt32 a = ref_desc.m_Indices[t * 3];
        const plUInt32 b = ref_desc.m_Indices[t * 3 + 1];
        const plUInt32 c = ref_desc.m_Indices[t * 3 + 2];

        float fWeightA = 0.0f;
        float fWeightB = 0.0f;
        const plVec3 vClosest = ClosestPointOnTriangle(vPosition, ref_desc.m_Positions[a], ref_desc.m_Positions[b], ref_desc.m_Positions[c], fWeightA, fWeightB);

        const float fDistanceSqr = (vPosition - vClosest).GetLengthSquared();
        if (fDistanceSqr >= fBestDistanceSqr)
          continue;

        fBestDistanceSqr = fDistanceSqr;
        best.m_vOffset = vPosition - vClosest;
        best.m_uiParticle0 = a;
        best.m_uiParticle1 = static_cast<plUInt16>(b);
        best.m_uiParticle2 = static_cast<plUInt16>(c);
        best.m_fWeight0 = fWeightA;
        best.m_fWeight1 = fWeightB;
      }

      if (best.m_uiParticle0 == plInvalidIndex)
        continue;

      ref_desc.m_Bindings[piece.m_uiFirstRenderVertex + v] = best;
      ++ref_report.m_uiBoundVertices;
    }
  }

  void AnalyzeTopology(plJoltClothDescriptor& ref_desc, plJoltClothBuildReport& ref_report)
  {
    plDynamicArray<bool> used;
    used.SetCount(ref_desc.GetParticleCount(), false);

    plHashTable<plUInt64, plUInt32> edgeUse;

    for (plUInt32 i = 0; i + 2 < ref_desc.m_Indices.GetCount(); i += 3)
    {
      const plUInt32 tri[3] = {ref_desc.m_Indices[i], ref_desc.m_Indices[i + 1], ref_desc.m_Indices[i + 2]};

      for (plUInt32 e = 0; e < 3; ++e)
      {
        used[tri[e]] = true;
        ++edgeUse[MakeEdgeKey(tri[e], tri[(e + 1) % 3])];
      }
    }

    for (auto it = edgeUse.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value() == 1)
        ++ref_report.m_uiBoundaryEdges;
      else if (it.Value() > 2)
        ++ref_report.m_uiNonManifoldEdges;
    }

    // A particle without a triangle has no constraint to hold it: pin it.
    for (plUInt32 p = 0; p < used.GetCount(); ++p)
    {
      if (!used[p])
      {
        ref_desc.m_MaxDistance[p] = 0.0f;
        ++ref_report.m_uiLooseVertices;
      }
    }
  }
} // namespace

plResult plJoltClothBuilder::Build(plArrayPtr<const plJoltClothBuildPiece> pieces, const plJoltClothBuildOptions& options, plJoltClothDescriptor& out_desc, plJoltClothBuildReport* out_pReport)
{
  plJoltClothBuildReport report;

  out_desc.m_Positions.Clear();
  out_desc.m_BoneIndices.Clear();
  out_desc.m_BoneWeights.Clear();
  out_desc.m_MaxDistance.Clear();
  out_desc.m_BackstopDistance.Clear();
  out_desc.m_BackstopRadius.Clear();
  out_desc.m_StiffnessScale.Clear();
  out_desc.m_Indices.Clear();
  out_desc.m_Bindings.Clear();
  out_desc.m_Bindings.SetCount(options.m_uiRenderVertexCount);

  plDynamicArray<bool> simulated;
  simulated.SetCount(pieces.GetCount(), false);

  for (plUInt32 p = 0; p < pieces.GetCount(); ++p)
  {
    const plJoltClothBuildPiece& piece = pieces[p];

    if (!PieceIsValid(piece))
    {
      plLog::Error("Cloth piece {} has mismatching array sizes or indices out of range.", p);
      return PL_FAILURE;
    }

    if (piece.IsProxy())
      continue;

    if (piece.m_uiFirstRenderVertex + piece.m_Positions.GetCount() > options.m_uiRenderVertexCount)
    {
      plLog::Error("Cloth piece {} covers render vertices {} to {}, the mesh has {}.", p, piece.m_uiFirstRenderVertex, piece.m_uiFirstRenderVertex + piece.m_Positions.GetCount(), options.m_uiRenderVertexCount);
      return PL_FAILURE;
    }

    if (piece.m_uiProxyPiece == plInvalidIndex)
    {
      simulated[p] = true;
      continue;
    }

    if (piece.m_uiProxyPiece >= pieces.GetCount() || !pieces[piece.m_uiProxyPiece].IsProxy())
    {
      plLog::Error("Cloth piece {} names piece {} as its proxy, which is not a proxy.", p, piece.m_uiProxyPiece);
      return PL_FAILURE;
    }

    simulated[piece.m_uiProxyPiece] = true;
  }

  ParticleWelder welder(options.m_fWeldDistance, out_desc);
  plHashSet<plUInt64> triangles;

  plDynamicArray<PieceResult> results;
  results.SetCount(pieces.GetCount());

  for (plUInt32 p = 0; p < pieces.GetCount(); ++p)
  {
    if (simulated[p])
      WeldPiece(pieces[p], options, welder, out_desc, results[p], triangles, report);
  }

  if (out_desc.m_Positions.IsEmpty() || out_desc.m_Indices.IsEmpty())
  {
    plLog::Error("The cloth pieces contain no triangle to simulate.");
    return PL_FAILURE;
  }

  if (out_desc.GetParticleCount() > plClothDeformation::MaxParticles)
  {
    plLog::Error("The cloth would have {} particles, the limit is {}. Use a proxy mesh or a lower particle budget.", out_desc.GetParticleCount(), plClothDeformation::MaxParticles);
    return PL_FAILURE;
  }

  for (plUInt32 p = 0; p < pieces.GetCount(); ++p)
  {
    const plJoltClothBuildPiece& piece = pieces[p];

    if (piece.IsProxy())
      continue;

    if (piece.m_uiProxyPiece == plInvalidIndex)
      BindToParticles(piece, results[p], out_desc, report);
    else
      BindToProxy(piece, results[piece.m_uiProxyPiece], out_desc, report);
  }

  AnalyzeTopology(out_desc, report);
  UpdateBindingBlend(out_desc, options.m_fBlendDistance);

  report.m_uiParticles = out_desc.GetParticleCount();
  report.m_uiTriangles = out_desc.GetTriangleCount();

  if (out_pReport != nullptr)
    *out_pReport = report;

  return out_desc.Validate();
}

plResult plJoltClothBuilder::BuildFromMesh(const plMeshResourceDescriptor& mesh, plArrayPtr<const plString> garmentNames, float fMaxDistance, const plJoltClothBuildOptions& options, plJoltClothDescriptor& out_desc, plJoltClothBuildReport* out_pReport)
{
  const plArrayPtr<const plMeshResourceDescriptor::ClothSubMeshData> clothPieces = mesh.GetClothSubMeshes();

  // the part of a name after its prefix, which a garment and its proxy share
  auto GetSuffix = [](plStringView sName) -> plStringView
  {
    const char* szUnderscore = sName.FindSubString("_");
    return szUnderscore != nullptr ? plStringView(szUnderscore + 1, sName.GetEndPointer()) : sName;
  };

  auto IsSelected = [&](plStringView sName) -> bool
  {
    if (garmentNames.IsEmpty())
      return true;

    // with or without the prefix: CLOTH_Cape and Cape both select the cape
    for (const plString& sGarment : garmentNames)
    {
      if (sName.IsEqual_NoCase(sGarment) || GetSuffix(sName).IsEqual_NoCase(sGarment))
        return true;
    }

    return false;
  };

  // A colour channel that has one value on every vertex was not painted: the default applies.
  auto IsPainted = [](const plDynamicArray<plUInt8>& mask, plUInt32 uiVertexCount) -> bool
  {
    if (mask.GetCount() != uiVertexCount || mask.IsEmpty())
      return false;

    for (plUInt8 uiValue : mask)
    {
      if (uiValue != mask[0])
        return true;
    }

    return false;
  };

  struct PieceMaps
  {
    plDynamicArray<float> m_MaxDistance;
    plDynamicArray<float> m_BackstopDistance;
    plDynamicArray<float> m_StiffnessScale;
  };

  plDynamicArray<plJoltClothBuildPiece> pieces;
  plDynamicArray<plUInt32> sourceOfPiece;
  plDynamicArray<PieceMaps> pieceMaps;
  pieceMaps.SetCount(clothPieces.GetCount());

  auto AddPiece = [&](plUInt32 uiSource) -> plUInt32
  {
    const auto& cloth = clothPieces[uiSource];
    const plUInt32 uiVertexCount = cloth.m_RestPositions.GetCount();

    PieceMaps& maps = pieceMaps[uiSource];

    plJoltClothBuildPiece& piece = pieces.ExpandAndGetRef();
    piece.m_Positions = cloth.m_RestPositions;
    piece.m_Indices = cloth.m_Indices;
    piece.m_BoneIndices = cloth.m_BoneIndices;
    piece.m_BoneWeights = cloth.m_BoneWeights;
    piece.m_uiFirstRenderVertex = cloth.m_bIsProxy ? plInvalidIndex : cloth.m_uiFirstVertex;

    if (options.m_bUseImportedMasks)
    {
      // red pins: 1 keeps the vertex on its skinned position, 0 lets it move the full max distance
      const plDynamicArray<plUInt8>& pinMask = cloth.m_bUseOverride && cloth.m_PinMaskOverride.GetCount() == uiVertexCount ? cloth.m_PinMaskOverride : cloth.m_PinMaskImported;

      maps.m_MaxDistance.SetCountUninitialized(uiVertexCount);
      for (plUInt32 v = 0; v < uiVertexCount; ++v)
      {
        const float fPin = v < pinMask.GetCount() ? pinMask[v] / 255.0f : 0.0f;
        maps.m_MaxDistance[v] = (1.0f - fPin) * fMaxDistance;
      }

      piece.m_MaxDistance = maps.m_MaxDistance;

      // green scales the backstop distance
      if (IsPainted(cloth.m_BackstopMaskImported, uiVertexCount))
      {
        maps.m_BackstopDistance.SetCountUninitialized(uiVertexCount);
        for (plUInt32 v = 0; v < uiVertexCount; ++v)
        {
          maps.m_BackstopDistance[v] = cloth.m_BackstopMaskImported[v] / 255.0f * options.m_fDefaultBackstopDistance;
        }

        piece.m_BackstopDistance = maps.m_BackstopDistance;
      }

      // blue scales the stiffness; a constraint never gets softer than a twentieth of the fabric
      if (IsPainted(cloth.m_StiffnessMaskImported, uiVertexCount))
      {
        maps.m_StiffnessScale.SetCountUninitialized(uiVertexCount);
        for (plUInt32 v = 0; v < uiVertexCount; ++v)
        {
          maps.m_StiffnessScale[v] = plMath::Max(0.05f, cloth.m_StiffnessMaskImported[v] / 255.0f) * options.m_fDefaultStiffnessScale;
        }

        piece.m_StiffnessScale = maps.m_StiffnessScale;
      }
    }

    sourceOfPiece.PushBack(uiSource);
    return pieces.GetCount() - 1;
  };

  for (plUInt32 g = 0; g < clothPieces.GetCount(); ++g)
  {
    if (clothPieces[g].m_bIsProxy || !IsSelected(clothPieces[g].m_sName.GetView()))
      continue;

    const plUInt32 uiGarmentPiece = AddPiece(g);

    for (plUInt32 x = 0; x < clothPieces.GetCount(); ++x)
    {
      if (!clothPieces[x].m_bIsProxy || !GetSuffix(clothPieces[x].m_sName.GetView()).IsEqual_NoCase(GetSuffix(clothPieces[g].m_sName.GetView())))
        continue;

      // AddPiece may reallocate the array, so the garment is looked up again
      const plUInt32 uiProxyPiece = AddPiece(x);
      pieces[uiGarmentPiece].m_uiProxyPiece = uiProxyPiece;
      break;
    }
  }

  if (pieces.IsEmpty())
  {
    plLog::Error("The mesh has no cloth piece to build from. Garments are meshes whose name starts with the cloth prefix (CLOTH_ by default).");
    return PL_FAILURE;
  }

  plJoltClothBuildOptions meshOptions = options;
  if (meshOptions.m_uiRenderVertexCount == 0)
    meshOptions.m_uiRenderVertexCount = mesh.MeshBufferDesc().GetVertexCount();

  return Build(pieces, meshOptions, out_desc, out_pReport);
}

void plJoltClothBuilder::UpdateBindingBlend(plJoltClothDescriptor& ref_desc, float fBlendDistance)
{
  for (plClothVertexBinding& binding : ref_desc.m_Bindings)
  {
    if (binding.m_uiParticle0 == plInvalidIndex)
      continue;

    const float fWeight2 = 1.0f - binding.m_fWeight0 - binding.m_fWeight1;
    const float fMaxDistance = ref_desc.m_MaxDistance[binding.m_uiParticle0] * binding.m_fWeight0 + ref_desc.m_MaxDistance[binding.m_uiParticle1] * binding.m_fWeight1 +
                               ref_desc.m_MaxDistance[binding.m_uiParticle2] * fWeight2;

    if (fBlendDistance > 0.0f)
      binding.m_fBlend = plMath::Saturate(fMaxDistance / fBlendDistance);
    else
      binding.m_fBlend = fMaxDistance > 0.0f ? 1.0f : 0.0f;
  }
}

namespace
{
  /// The rest pose of every joint in model space, which is the bind space of the cloth. Parents come before their children.
  void ComputeRestPose(const plSkeleton& skeleton, plDynamicArray<plMat4>& out_restPose)
  {
    const plUInt16 uiJointCount = skeleton.GetJointCount();
    out_restPose.SetCount(uiJointCount);

    for (plUInt16 j = 0; j < uiJointCount; ++j)
    {
      const plSkeletonJoint& joint = skeleton.GetJointByIndex(j);
      const plMat4 mLocal = joint.GetRestPoseLocalTransform().GetAsMat4();

      out_restPose[j] = joint.GetParentIndex() < j ? out_restPose[joint.GetParentIndex()] * mLocal : mLocal;
    }
  }

  /// A joint's pose the way the simulation sees it: position and a rotation without scale.
  plTransform ToJointTransform(const plMat4& mJoint)
  {
    plMat4 mFull;
    plQuat qRotation;
    plMsgAnimationPoseUpdated::ComputeFullBoneTransform(plMat4::MakeIdentity(), mJoint, mFull, qRotation);

    return plTransform(mFull.GetTranslationVector(), qRotation);
  }

  /// Whether a free particle of the cloth can come within the margin of the shape, standing in its bind pose. The
  /// shape is taken as a capsule around its long axis; a box as the sphere around it.
  bool IsColliderInReach(const plJoltClothDescriptor& desc, const plJoltClothCollider& collider, float fMargin)
  {
    const plTransform shape = collider.GetShapeTransform(collider.m_JointBindPose);

    const plVec3 vCenter = shape.m_vPosition;
    const plVec3 vAxis = shape.m_qRotation * plVec3(1, 0, 0);

    float fHalfLength = 0.0f;
    float fRadius = collider.m_fRadius;

    if (collider.m_Type == plJoltClothCollider::Type::Capsule)
      fHalfLength = collider.m_fLength * 0.5f;
    else if (collider.m_Type == plJoltClothCollider::Type::Box)
      fRadius = collider.m_vHalfExtents.GetLength();

    for (plUInt32 p = 0; p < desc.m_Positions.GetCount(); ++p)
    {
      if (desc.m_MaxDistance[p] <= 0.0f)
        continue;

      const plVec3 vToParticle = desc.m_Positions[p] - vCenter;
      const float fAlong = plMath::Clamp(vToParticle.Dot(vAxis), -fHalfLength, fHalfLength);
      const float fDistance = (vToParticle - vAxis * fAlong).GetLength() - fRadius;

      if (fDistance <= desc.m_MaxDistance[p] + desc.m_Fabric.m_fThickness + fMargin)
        return true;
    }

    return false;
  }
} // namespace

plUInt32 plJoltClothBuilder::FitCollidersFromSkin(plArrayPtr<const plVec3> positions, plArrayPtr<const plVec4U16> boneIndices, plArrayPtr<const plVec4> boneWeights, plArrayPtr<const plHashedString> boneNames, plArrayPtr<const plMat4> inverseBindPoses, const plJoltClothSkinFitOptions& options, const plJoltClothDescriptor* pReach, plDynamicArray<plJoltClothCollider>& out_colliders)
{
  const plUInt32 uiBones = plMath::Min(boneNames.GetCount(), inverseBindPoses.GetCount());

  // the vertices of every bone, in the space of its joint
  plDynamicArray<plDynamicArray<plVec3>> perBone;
  perBone.SetCount(uiBones);

  for (plUInt32 v = 0; v < positions.GetCount(); ++v)
  {
    const plVec4U16& indices = boneIndices[v];
    const plVec4& weights = boneWeights[v];

    const plUInt16 uiIndex[4] = {indices.x, indices.y, indices.z, indices.w};
    const float fWeight[4] = {weights.x, weights.y, weights.z, weights.w};

    plUInt32 uiStrongest = 0;
    for (plUInt32 i = 1; i < 4; ++i)
    {
      if (fWeight[i] > fWeight[uiStrongest])
        uiStrongest = i;
    }

    if (fWeight[uiStrongest] < options.m_fMinWeight || uiIndex[uiStrongest] >= uiBones)
      continue;

    perBone[uiIndex[uiStrongest]].PushBack(inverseBindPoses[uiIndex[uiStrongest]].TransformPosition(positions[v]));
  }

  plUInt32 uiAdded = 0;

  for (plUInt32 b = 0; b < uiBones; ++b)
  {
    const plDynamicArray<plVec3>& points = perBone[b];
    if (points.GetCount() < options.m_uiMinVertices)
      continue;

    plVec3 vMean = plVec3::MakeZero();
    for (const plVec3& vPoint : points)
    {
      vMean += vPoint;
    }
    vMean /= static_cast<float>(points.GetCount());

    // The long axis of the part is the direction its vertices spread most in: the largest eigenvector of their
    // covariance, found by multiplying a vector with it until it stops turning.
    plMat3 mCovariance = plMat3::MakeZero();
    for (const plVec3& vPoint : points)
    {
      const plVec3 d = vPoint - vMean;

      for (plUInt32 r = 0; r < 3; ++r)
      {
        for (plUInt32 c = 0; c < 3; ++c)
        {
          mCovariance.Element(c, r) += d.GetData()[r] * d.GetData()[c];
        }
      }
    }

    plVec3 vAxis(1.0f, 0.7f, 0.4f);
    for (plUInt32 i = 0; i < 24; ++i)
    {
      vAxis = mCovariance * vAxis;

      if (vAxis.NormalizeIfNotZero(plVec3(1, 0, 0), 1.0e-20f).Failed())
        break;
    }

    float fMin = plMath::MaxValue<float>();
    float fMax = -plMath::MaxValue<float>();
    double fAlongSqr = 0.0;
    double fAcrossSqr = 0.0;

    for (const plVec3& vPoint : points)
    {
      const plVec3 d = vPoint - vMean;
      const float fAlong = d.Dot(vAxis);

      fMin = plMath::Min(fMin, fAlong);
      fMax = plMath::Max(fMax, fAlong);
      fAlongSqr += fAlong * fAlong;
      fAcrossSqr += (d - vAxis * fAlong).GetLengthSquared();
    }

    fAlongSqr /= points.GetCount();
    fAcrossSqr /= points.GetCount();

    plJoltClothCollider collider;
    collider.m_sJoint = boneNames[b];
    collider.m_JointBindPose = ToJointTransform(inverseBindPoses[b].GetInverse());

    // The vertices lie on the skin. A part that spreads about as far along its axis as across it is a ball, and its
    // radius their distance from the middle. Any other part is a limb, and its radius their distance from the axis.
    if (fAlongSqr < 1.5 * fAcrossSqr * 0.5)
    {
      collider.m_Type = plJoltClothCollider::Type::Sphere;
      collider.m_fRadius = plMath::Sqrt(static_cast<float>(fAlongSqr + fAcrossSqr)) * options.m_fRadiusScale;
      collider.m_fLength = 0.0f;
      collider.m_Transform.m_vPosition = vMean;
    }
    else
    {
      collider.m_Type = plJoltClothCollider::Type::Capsule;
      collider.m_fRadius = plMath::Sqrt(static_cast<float>(fAcrossSqr)) * options.m_fRadiusScale;
      collider.m_fLength = plMath::Max(0.0f, (fMax - fMin) - 2.0f * collider.m_fRadius);
      collider.m_Transform.m_vPosition = vMean + vAxis * ((fMin + fMax) * 0.5f);
      collider.m_Transform.m_qRotation = plQuat::MakeShortestRotation(plVec3(1, 0, 0), vAxis);
    }

    if (collider.m_fRadius < options.m_fMinRadius)
      continue;

    if (pReach != nullptr && !IsColliderInReach(*pReach, collider, options.m_fReachMargin))
      continue;

    out_colliders.PushBack(collider);
    ++uiAdded;
  }

  return uiAdded;
}

plUInt32 plJoltClothBuilder::FitCollidersFromSkin(const plMeshResourceDescriptor& mesh, const plJoltClothSkinFitOptions& options, const plJoltClothDescriptor* pReach, plDynamicArray<plJoltClothCollider>& out_colliders)
{
  const plMeshBufferResourceDescriptor& buffer = mesh.MeshBufferDesc();
  const plUInt32 uiVertices = buffer.GetVertexCount();

  if (uiVertices == 0 || mesh.m_Bones.IsEmpty() || !buffer.GetVertexStreamConfig().HasSkinningData())
    return 0;

  // garments hang on the body; they are not the body
  plDynamicArray<bool> isGarment;
  isGarment.SetCount(uiVertices, false);

  for (const auto& garment : mesh.GetClothSubMeshes())
  {
    if (garment.m_bIsProxy)
      continue;

    for (plUInt32 v = 0; v < garment.m_uiVertexCount && garment.m_uiFirstVertex + v < uiVertices; ++v)
    {
      isGarment[garment.m_uiFirstVertex + v] = true;
    }
  }

  plDynamicArray<plVec3> positions;
  plDynamicArray<plVec4U16> boneIndices;
  plDynamicArray<plVec4> boneWeights;

  for (plUInt32 v = 0; v < uiVertices; ++v)
  {
    if (isGarment[v])
      continue;

    positions.PushBack(buffer.GetPosition(v));
    boneIndices.PushBack(buffer.GetBoneIndices(v));
    boneWeights.PushBack(buffer.GetBoneWeights(v));
  }

  plUInt32 uiBones = 0;
  for (auto it = mesh.m_Bones.GetIterator(); it.IsValid(); ++it)
  {
    uiBones = plMath::Max<plUInt32>(uiBones, it.Value().m_uiBoneIndex + 1u);
  }

  plDynamicArray<plHashedString> boneNames;
  plDynamicArray<plMat4> inverseBindPoses;
  boneNames.SetCount(uiBones);
  inverseBindPoses.SetCount(uiBones, plMat4::MakeIdentity());

  for (auto it = mesh.m_Bones.GetIterator(); it.IsValid(); ++it)
  {
    boneNames[it.Value().m_uiBoneIndex] = it.Key();
    inverseBindPoses[it.Value().m_uiBoneIndex] = it.Value().m_GlobalInverseRestPoseMatrix;
  }

  return FitCollidersFromSkin(positions, boneIndices, boneWeights, boneNames, inverseBindPoses, options, pReach, out_colliders);
}

void plJoltClothBuilder::SetJointBindPoses(const plSkeletonResourceDescriptor& skeleton, plJoltClothDescriptor& ref_desc)
{
  plDynamicArray<plMat4> restPose;
  ComputeRestPose(skeleton.m_Skeleton, restPose);

  for (plJoltClothCollider& collider : ref_desc.m_Colliders)
  {
    const plUInt16 uiJoint = skeleton.m_Skeleton.FindJointByName(collider.m_sJoint);

    if (uiJoint != plInvalidJointIndex)
      collider.m_JointBindPose = ToJointTransform(restPose[uiJoint]);
  }
}

plUInt32 plJoltClothBuilder::AddSkeletonColliders(const plSkeletonResourceDescriptor& skeleton, const plJoltClothSkeletonColliderOptions& options, plJoltClothDescriptor& ref_desc)
{
  const plSkeleton& skel = skeleton.m_Skeleton;
  const plUInt16 uiJointCount = skel.GetJointCount();

  plDynamicArray<plMat4> restPose;
  ComputeRestPose(skel, restPose);

  // shapes are authored along the skeleton's bone direction, colliders along X
  const plQuat qBoneDirection = plBasisAxis::GetBasisRotation(plBasisAxis::PositiveX, skel.m_BoneDirection);
  const plQuat qXToY = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(90));

  auto IsListed = [&](plStringView sJoint) -> bool
  {
    for (const plString& sListed : options.m_Joints)
    {
      if (sJoint.IsEqual_NoCase(sListed))
        return true;
    }

    return false;
  };

  plUInt32 uiAdded = 0;
  plUInt32 uiConvex = 0;

  for (const plSkeletonResourceGeometry& geo : skeleton.m_Geometry)
  {
    if (geo.m_Type == plSkeletonJointGeometryType::None || geo.m_uiAttachedToJoint >= uiJointCount)
      continue;

    if (geo.m_Type == plSkeletonJointGeometryType::ConvexMesh)
    {
      ++uiConvex;
      continue;
    }

    const plSkeletonJoint& joint = skel.GetJointByIndex(geo.m_uiAttachedToJoint);
    const plVec3& vSize = geo.m_Transform.m_vScale;

    plJoltClothCollider collider;
    collider.m_sJoint = joint.GetName();
    collider.m_JointBindPose = ToJointTransform(restPose[geo.m_uiAttachedToJoint]);
    collider.m_Transform.m_vPosition = qBoneDirection * geo.m_Transform.m_vPosition;
    collider.m_Transform.m_qRotation = qBoneDirection * geo.m_Transform.m_qRotation;

    // sphere, box and capsule sit as they do on a ragdoll or hitbox: a box and a capsule start at the joint
    switch (geo.m_Type)
    {
      case plSkeletonJointGeometryType::Sphere:
        collider.m_Type = plJoltClothCollider::Type::Sphere;
        collider.m_fRadius = vSize.z;
        break;

      case plSkeletonJointGeometryType::Box:
        collider.m_Type = plJoltClothCollider::Type::Box;
        collider.m_vHalfExtents = vSize * 0.5f;
        collider.m_Transform.m_vPosition += qBoneDirection * plVec3(vSize.x * 0.5f, 0, 0);
        break;

      case plSkeletonJointGeometryType::Capsule:
        collider.m_Type = plJoltClothCollider::Type::Capsule;
        collider.m_fRadius = vSize.z;
        collider.m_fLength = vSize.x;
        collider.m_Transform.m_vPosition += qBoneDirection * plVec3(vSize.x * 0.5f, 0, 0);
        break;

      case plSkeletonJointGeometryType::CapsuleSideways:
        collider.m_Type = plJoltClothCollider::Type::Capsule;
        collider.m_fRadius = vSize.z;
        collider.m_fLength = vSize.x;
        collider.m_Transform.m_qRotation = collider.m_Transform.m_qRotation * qXToY;
        break;

      default:
        continue;
    }

    collider.m_fRadius = plMath::Max(0.005f, collider.m_fRadius + options.m_fInflate);
    collider.m_vHalfExtents = (collider.m_vHalfExtents + plVec3(options.m_fInflate)).CompMax(plVec3(0.005f));
    collider.m_fInflate = plMath::Max(0.0f, options.m_fInflate);

    if (!options.m_bAllShapes)
    {
      const bool bTake = options.m_Joints.IsEmpty() ? IsColliderInReach(ref_desc, collider, options.m_fReachMargin) : IsListed(joint.GetName().GetView());
      if (!bTake)
        continue;
    }

    ref_desc.m_Colliders.PushBack(collider);
    ++uiAdded;
  }

  if (uiConvex > 0)
    plLog::Info("{} convex shape(s) of the skeleton are not used as cloth colliders. Cloth collides with spheres, capsules and boxes.", uiConvex);

  return uiAdded;
}

void plJoltClothBuilder::ApplyPaintedMaps(const plJoltClothPaintedMaps& maps, float fBlendDistance, plJoltClothDescriptor& ref_desc)
{
  const plUInt32 uiSamples = maps.m_Positions.GetCount();
  if (maps.IsEmpty())
    return;

  auto Apply = [uiSamples](const plDynamicArray<float>& painted, plDynamicArray<float>& ref_map, plUInt32 uiParticle, plUInt32 uiSample)
  {
    if (painted.GetCount() == uiSamples)
      ref_map[uiParticle] = painted[uiSample];
  };

  for (plUInt32 p = 0; p < ref_desc.m_Positions.GetCount(); ++p)
  {
    const plVec3& vPosition = ref_desc.m_Positions[p];

    plUInt32 uiClosest = 0;

    // while the sim mesh is the one that was painted on, a particle and its sample share an index
    if (p < uiSamples && (maps.m_Positions[p] - vPosition).GetLengthSquared() <= 1.0e-10f)
    {
      uiClosest = p;
    }
    else
    {
      float fClosestSqr = plMath::MaxValue<float>();

      for (plUInt32 s = 0; s < uiSamples; ++s)
      {
        const float fDistanceSqr = (maps.m_Positions[s] - vPosition).GetLengthSquared();
        if (fDistanceSqr < fClosestSqr)
        {
          fClosestSqr = fDistanceSqr;
          uiClosest = s;
        }
      }
    }

    Apply(maps.m_MaxDistance, ref_desc.m_MaxDistance, p, uiClosest);
    Apply(maps.m_BackstopDistance, ref_desc.m_BackstopDistance, p, uiClosest);
    Apply(maps.m_BackstopRadius, ref_desc.m_BackstopRadius, p, uiClosest);
    Apply(maps.m_StiffnessScale, ref_desc.m_StiffnessScale, p, uiClosest);
  }

  UpdateBindingBlend(ref_desc, fBlendDistance);
}

void plJoltClothBuilder::ExtractPaintedMaps(const plJoltClothDescriptor& desc, plJoltClothPaintedMaps& out_maps)
{
  out_maps.m_Positions = desc.m_Positions;
  out_maps.m_MaxDistance = desc.m_MaxDistance;
  out_maps.m_BackstopDistance = desc.m_BackstopDistance;
  out_maps.m_BackstopRadius = desc.m_BackstopRadius;
  out_maps.m_StiffnessScale = desc.m_StiffnessScale;
}

//////////////////////////////////////////////////////////////////////////

void plJoltClothPaintedMaps::Clear()
{
  m_Positions.Clear();
  m_MaxDistance.Clear();
  m_BackstopDistance.Clear();
  m_BackstopRadius.Clear();
  m_StiffnessScale.Clear();
}

plResult plJoltClothPaintedMaps::Serialize(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_Positions));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_MaxDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BackstopDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BackstopRadius));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_StiffnessScale));

  return PL_SUCCESS;
}

plResult plJoltClothPaintedMaps::Deserialize(plStreamReader& inout_stream)
{
  Clear();

  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion != 1)
    return PL_FAILURE;

  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Positions));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_MaxDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BackstopDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BackstopRadius));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_StiffnessScale));

  // a map is either unpainted or has a value for every sample
  for (const plDynamicArray<float>* pMap : {&m_MaxDistance, &m_BackstopDistance, &m_BackstopRadius, &m_StiffnessScale})
  {
    if (!pMap->IsEmpty() && pMap->GetCount() != m_Positions.GetCount())
    {
      Clear();
      return PL_FAILURE;
    }
  }

  return PL_SUCCESS;
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothBuilder);
