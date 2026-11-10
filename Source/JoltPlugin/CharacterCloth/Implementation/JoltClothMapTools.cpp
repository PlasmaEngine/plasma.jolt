#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashSet.h>
#include <JoltPlugin/CharacterCloth/JoltClothMapTools.h>

void plJoltClothMapTopology::Build(plArrayPtr<const plVec3> positions, plArrayPtr<const plUInt32> indices)
{
  m_Positions = positions;
  m_Indices = indices;

  const plUInt32 uiParticles = m_Positions.GetCount();

  // every edge once, as the pair of its particles with the smaller index first
  plHashSet<plUInt64> edges;
  edges.Reserve(m_Indices.GetCount());

  plDynamicArray<plUInt32> neighbourCount;
  neighbourCount.SetCount(uiParticles);

  for (plUInt32 i = 0; i + 2 < m_Indices.GetCount(); i += 3)
  {
    for (plUInt32 e = 0; e < 3; ++e)
    {
      const plUInt32 a = m_Indices[i + e];
      const plUInt32 b = m_Indices[i + (e + 1) % 3];

      if (a == b || a >= uiParticles || b >= uiParticles)
        continue;

      const plUInt64 uiKey = a < b ? (static_cast<plUInt64>(a) << 32) | b : (static_cast<plUInt64>(b) << 32) | a;

      if (!edges.Insert(uiKey))
      {
        ++neighbourCount[a];
        ++neighbourCount[b];
      }
    }
  }

  m_NeighbourStart.SetCountUninitialized(uiParticles + 1);

  plUInt32 uiTotal = 0;
  for (plUInt32 p = 0; p < uiParticles; ++p)
  {
    m_NeighbourStart[p] = uiTotal;
    uiTotal += neighbourCount[p];
  }
  m_NeighbourStart[uiParticles] = uiTotal;

  m_Neighbours.SetCountUninitialized(uiTotal);

  plDynamicArray<plUInt32> filled;
  filled.SetCount(uiParticles);

  for (plUInt64 uiKey : edges)
  {
    const plUInt32 a = static_cast<plUInt32>(uiKey >> 32);
    const plUInt32 b = static_cast<plUInt32>(uiKey & 0xFFFFFFFFu);

    m_Neighbours[m_NeighbourStart[a] + filled[a]++] = b;
    m_Neighbours[m_NeighbourStart[b] + filled[b]++] = a;
  }

  // connected pieces, by flooding from every particle that has none yet
  m_Island.Clear();
  m_Island.SetCount(uiParticles, plInvalidIndex);
  m_uiIslandCount = 0;

  plDynamicArray<plUInt32> stack;

  for (plUInt32 uiSeed = 0; uiSeed < uiParticles; ++uiSeed)
  {
    if (m_Island[uiSeed] != plInvalidIndex)
      continue;

    m_Island[uiSeed] = m_uiIslandCount;
    stack.PushBack(uiSeed);

    while (!stack.IsEmpty())
    {
      const plUInt32 p = stack.PeekBack();
      stack.PopBack();

      for (plUInt32 n : GetNeighbours(p))
      {
        if (m_Island[n] != plInvalidIndex)
          continue;

        m_Island[n] = m_uiIslandCount;
        stack.PushBack(n);
      }
    }

    ++m_uiIslandCount;
  }
}

float plJoltClothBrush::GetWeight(const plVec3& vPosition) const
{
  if (m_fRadius <= 0.0f)
    return 0.0f;

  const float fDistance = (vPosition - m_vCenter).GetLength() / m_fRadius;
  if (fDistance >= 1.0f)
    return 0.0f;

  const float fHardness = plMath::Clamp(m_fHardness, 0.0f, 0.99f);
  if (fDistance <= fHardness)
    return m_fStrength;

  // smooth fade from the hard core to the rim
  const float t = 1.0f - (fDistance - fHardness) / (1.0f - fHardness);
  return m_fStrength * t * t * (3.0f - 2.0f * t);
}

bool plJoltClothMapTools::RayCast(const plJoltClothMapTopology& topology, const plVec3& vStart, const plVec3& vDirection, RayHit& out_hit)
{
  out_hit = RayHit();
  float fClosest = plMath::MaxValue<float>();

  const plDynamicArray<plVec3>& positions = topology.m_Positions;
  const plDynamicArray<plUInt32>& indices = topology.m_Indices;

  for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
  {
    const plVec3& a = positions[indices[i]];
    const plVec3 vEdge1 = positions[indices[i + 1]] - a;
    const plVec3 vEdge2 = positions[indices[i + 2]] - a;

    const plVec3 vP = vDirection.CrossRH(vEdge2);
    const float fDeterminant = vEdge1.Dot(vP);

    // parallel to the triangle; both sides of it count
    if (plMath::Abs(fDeterminant) < 1.0e-12f)
      continue;

    const float fInvDeterminant = 1.0f / fDeterminant;
    const plVec3 vT = vStart - a;

    const float u = vT.Dot(vP) * fInvDeterminant;
    if (u < 0.0f || u > 1.0f)
      continue;

    const plVec3 vQ = vT.CrossRH(vEdge1);

    const float v = vDirection.Dot(vQ) * fInvDeterminant;
    if (v < 0.0f || u + v > 1.0f)
      continue;

    const float fDistance = vEdge2.Dot(vQ) * fInvDeterminant;
    if (fDistance < 0.0f || fDistance >= fClosest)
      continue;

    fClosest = fDistance;

    out_hit.m_fDistance = fDistance;
    out_hit.m_vPosition = vStart + vDirection * fDistance;
    out_hit.m_uiTriangle = i / 3;

    // the corner with the largest barycentric weight
    const float w = 1.0f - u - v;
    out_hit.m_uiClosestParticle = w >= u && w >= v ? indices[i] : (u >= v ? indices[i + 1] : indices[i + 2]);
  }

  return out_hit.m_uiTriangle != plInvalidIndex;
}

bool plJoltClothMapTools::AccumulateStroke(const plJoltClothMapTopology& topology, const plJoltClothBrush& brush, plArrayPtr<float> ref_weights)
{
  bool bChanged = false;

  const plUInt32 uiCount = plMath::Min(topology.m_Positions.GetCount(), ref_weights.GetCount());

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    const float fWeight = brush.GetWeight(topology.m_Positions[p]);

    if (fWeight > ref_weights[p])
    {
      ref_weights[p] = fWeight;
      bChanged = true;
    }
  }

  return bChanged;
}

void plJoltClothMapTools::ApplyPaint(plArrayPtr<const float> startValues, plArrayPtr<const float> weights, float fValue, plArrayPtr<float> ref_map)
{
  const plUInt32 uiCount = plMath::Min(startValues.GetCount(), weights.GetCount(), ref_map.GetCount());

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    ref_map[p] = plMath::Lerp(startValues[p], fValue, plMath::Saturate(weights[p]));
  }
}

void plJoltClothMapTools::Smooth(const plJoltClothMapTopology& topology, const plJoltClothBrush& brush, plArrayPtr<float> ref_map)
{
  const plUInt32 uiCount = plMath::Min(topology.m_Positions.GetCount(), ref_map.GetCount());

  // averages come from the values before this call, so the result does not depend on the particle order
  plDynamicArray<float> before;
  before = ref_map;

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    const float fWeight = brush.GetWeight(topology.m_Positions[p]);
    if (fWeight <= 0.0f)
      continue;

    const plArrayPtr<const plUInt32> neighbours = topology.GetNeighbours(p);
    if (neighbours.IsEmpty())
      continue;

    float fSum = 0.0f;
    for (plUInt32 n : neighbours)
    {
      fSum += before[n];
    }

    ref_map[p] = plMath::Lerp(before[p], fSum / static_cast<float>(neighbours.GetCount()), plMath::Saturate(fWeight));
  }
}

void plJoltClothMapTools::Fill(const plJoltClothMapTopology& topology, plUInt32 uiIsland, float fValue, plArrayPtr<float> ref_map)
{
  const plUInt32 uiCount = plMath::Min(topology.m_Positions.GetCount(), ref_map.GetCount());

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    if (uiIsland == plInvalidIndex || topology.m_Island[p] == uiIsland)
      ref_map[p] = fValue;
  }
}

void plJoltClothMapTools::Gradient(const plJoltClothMapTopology& topology, plUInt32 uiIsland, const plVec3& vFrom, const plVec3& vTo, float fFromValue, float fToValue, plArrayPtr<float> ref_map)
{
  const plVec3 vAxis = vTo - vFrom;
  const float fLengthSqr = vAxis.GetLengthSquared();
  if (fLengthSqr <= 1.0e-10f)
    return;

  const plUInt32 uiCount = plMath::Min(topology.m_Positions.GetCount(), ref_map.GetCount());

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    if (uiIsland != plInvalidIndex && topology.m_Island[p] != uiIsland)
      continue;

    const float t = plMath::Saturate((topology.m_Positions[p] - vFrom).Dot(vAxis) / fLengthSqr);
    ref_map[p] = plMath::Lerp(fFromValue, fToValue, t);
  }
}

void plJoltClothMapTools::AutoMask(const plJoltClothMapTopology& topology, const plVec3& vUp, float fPinnedFraction, float fMaxDistance, plArrayPtr<float> ref_map)
{
  const plUInt32 uiCount = plMath::Min(topology.m_Positions.GetCount(), ref_map.GetCount());
  if (uiCount == 0)
    return;

  // the height range of every piece
  plDynamicArray<float> top, bottom;
  top.SetCount(topology.m_uiIslandCount, -plMath::MaxValue<float>());
  bottom.SetCount(topology.m_uiIslandCount, plMath::MaxValue<float>());

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    const float fHeight = topology.m_Positions[p].Dot(vUp);
    const plUInt32 uiIsland = topology.m_Island[p];

    top[uiIsland] = plMath::Max(top[uiIsland], fHeight);
    bottom[uiIsland] = plMath::Min(bottom[uiIsland], fHeight);
  }

  // distance along the mesh from the pinned top of the piece
  plDynamicArray<float> distance;
  distance.SetCount(uiCount, plMath::MaxValue<float>());

  plDeque<plUInt32> queue;
  plDynamicArray<bool> queued;
  queued.SetCount(uiCount, false);

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    const plUInt32 uiIsland = topology.m_Island[p];
    const float fPinnedBelow = top[uiIsland] - plMath::Saturate(fPinnedFraction) * (top[uiIsland] - bottom[uiIsland]);

    if (topology.m_Positions[p].Dot(vUp) >= fPinnedBelow)
    {
      distance[p] = 0.0f;
      queue.PushBack(p);
      queued[p] = true;
    }
  }

  while (!queue.IsEmpty())
  {
    const plUInt32 p = queue.PeekFront();
    queue.PopFront();
    queued[p] = false;

    for (plUInt32 n : topology.GetNeighbours(p))
    {
      if (n >= uiCount)
        continue;

      const float fThrough = distance[p] + (topology.m_Positions[n] - topology.m_Positions[p]).GetLength();

      if (fThrough < distance[n])
      {
        distance[n] = fThrough;

        if (!queued[n])
        {
          queue.PushBack(n);
          queued[n] = true;
        }
      }
    }
  }

  plDynamicArray<float> furthest;
  furthest.SetCount(topology.m_uiIslandCount, 0.0f);

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    if (distance[p] < plMath::MaxValue<float>())
      furthest[topology.m_Island[p]] = plMath::Max(furthest[topology.m_Island[p]], distance[p]);
  }

  for (plUInt32 p = 0; p < uiCount; ++p)
  {
    const float fFurthest = furthest[topology.m_Island[p]];

    // a slow start keeps the cloth near the collar close to the body
    const float t = fFurthest > 0.0f ? plMath::Saturate(distance[p] / fFurthest) : 0.0f;
    ref_map[p] = fMaxDistance * plMath::Pow(t, 1.5f);
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothMapTools);
