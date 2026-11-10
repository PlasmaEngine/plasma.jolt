#include <JoltPlugin/JoltPluginPCH.h>

#include <JoltPlugin/CharacterCloth/JoltCharacterClothComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>

// The cloth views of the viewport's view menu (cvar Jolt.Cloth.View): each draws over the garments in the scene.

namespace
{
  enum ClothView : plInt32
  {
    SimMesh = 1,
    MaxDistance = 2,
    Backstop = 3,
    Colliders = 4,
    Strain = 5,
    State = 6,
    Cost = 7,
  };

  /// Blue for 0 through teal and amber to red for 1.
  plColor Heat(float fValue)
  {
    static const plColor s_Stops[4] = {plColor(0.23f, 0.36f, 0.86f), plColor(0.17f, 0.7f, 0.64f), plColor(0.91f, 0.69f, 0.29f), plColor(0.9f, 0.28f, 0.3f)};

    const float fScaled = plMath::Saturate(fValue) * 3.0f;
    const plUInt32 uiStop = plMath::Min<plUInt32>(2, static_cast<plUInt32>(fScaled));

    return plMath::Lerp(s_Stops[uiStop], s_Stops[uiStop + 1], fScaled - static_cast<float>(uiStop));
  }

  plColor StateColor(plJoltClothState state)
  {
    switch (state)
    {
      case plJoltClothState::Full:
        return plColor(0.56f, 0.84f, 0.58f);
      case plJoltClothState::Reduced:
        return plColor(0.91f, 0.69f, 0.29f);
      case plJoltClothState::Blending:
        return plColor(0.43f, 0.66f, 1.0f);
      case plJoltClothState::Skinned:
        return plColor(0.54f, 0.54f, 0.58f);
      default:
        return plColor(0.35f, 0.35f, 0.38f);
    }
  }

  const char* StateName(plJoltClothState state)
  {
    switch (state)
    {
      case plJoltClothState::Full:
        return "Full";
      case plJoltClothState::Reduced:
        return "Reduced";
      case plJoltClothState::Blending:
        return "Blending";
      case plJoltClothState::Skinned:
        return "Skinned";
      default:
        return "Inactive";
    }
  }

  /// The triangle on both sides of the garment's surface, lifted off it so that it does not flicker against the mesh.
  void AddSurfaceTriangle(plDynamicArray<plDebugRenderer::Triangle>& ref_triangles, const plVec3& a, const plVec3& b, const plVec3& c, const plColor& color)
  {
    plVec3 vNormal = (b - a).CrossRH(c - a);
    if (vNormal.NormalizeIfNotZero(plVec3::MakeZero(), 1.0e-12f).Failed())
      return;

    const plVec3 vLift = vNormal * 0.004f;

    plDebugRenderer::Triangle& front = ref_triangles.ExpandAndGetRef();
    front = plDebugRenderer::Triangle(a + vLift, b + vLift, c + vLift);
    front.m_color = color;

    plDebugRenderer::Triangle& back = ref_triangles.ExpandAndGetRef();
    back = plDebugRenderer::Triangle(a - vLift, c - vLift, b - vLift);
    back.m_color = color;
  }
} // namespace

plColor plJoltCharacterClothComponent::GetHeatColor(float fValue)
{
  return Heat(fValue);
}

void plJoltCharacterClothComponent::DrawView(plInt32 iView) const
{
  const plWorld* pWorld = GetWorld();
  const plVec3 vOrigin = GetOwner()->GetGlobalPosition();

  // State and Cost say something about every garment, also the ones that are not simulating
  if (iView == ClothView::State || iView == ClothView::Cost)
  {
    plStringBuilder sLabel;
    if (iView == ClothView::State)
      sLabel.SetFormat("{} · {} m", StateName(m_State), plArgF(m_fLodDistance, 0));
    else
      sLabel.SetFormat("{} ms · {} particles", plArgF(m_State >= plJoltClothState::Blending ? GetLastStepDuration().GetMilliseconds() : 0.0, 2), m_Instance.GetParticleCount());

    plDebugRenderer::Draw3DText(pWorld, sLabel.GetData(), vOrigin + plVec3(0, 0, 2.0f), StateColor(m_State), 14);
  }

  if (m_pData == nullptr || !m_Instance.IsInitialized() || m_State < plJoltClothState::Blending)
    return;

  const plJoltClothDescriptor& desc = m_pData->GetDescriptor();

  // simulation space has the owner's position as origin and the world's axes
  plDynamicArray<plVec3> positions;
  m_Instance.GetSimPositions(positions);
  if (positions.GetCount() != desc.GetParticleCount())
    return;

  for (plVec3& vPosition : positions)
  {
    vPosition += vOrigin;
  }

  const plArrayPtr<const plUInt32> indices = desc.m_Indices;

  if (iView == ClothView::SimMesh || iView == ClothView::Colliders || iView == ClothView::Strain)
  {
    plDynamicArray<plVec3> restPositions;
    if (iView == ClothView::Strain)
    {
      restPositions.SetCountUninitialized(positions.GetCount());
      for (plUInt32 p = 0; p < positions.GetCount(); ++p)
      {
        restPositions[p] = m_pSetup->m_mBindToSim.TransformPosition(desc.m_Positions[p]);
      }
    }

    auto ParticleColor = [&](plUInt32 uiParticle) -> plColor
    {
      if (iView == ClothView::Colliders)
        return plColor(0.5f, 0.5f, 0.55f);

      return desc.m_MaxDistance[uiParticle] > 0.0f ? plColor::White : plColor::DodgerBlue;
    };

    plDynamicArray<plDebugRenderer::Line> lines;
    lines.Reserve(indices.GetCount());

    for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        const plUInt32 a = indices[i + e];
        const plUInt32 b = indices[i + (e + 1) % 3];

        plDebugRenderer::Line& line = lines.ExpandAndGetRef();
        line.m_start = positions[a];
        line.m_end = positions[b];

        if (iView == ClothView::Strain)
        {
          // green at rest length, red at 10 percent longer
          const float fRest = (restPositions[a] - restPositions[b]).GetLength();
          const float fStretch = fRest > 0.0f ? (positions[a] - positions[b]).GetLength() / fRest - 1.0f : 0.0f;
          line.m_startColor = line.m_endColor = plMath::Lerp(plColor(0.3f, 0.8f, 0.4f), plColor(0.95f, 0.25f, 0.25f), plMath::Saturate(fStretch / 0.1f));
        }
        else
        {
          line.m_startColor = ParticleColor(a);
          line.m_endColor = ParticleColor(b);
        }
      }
    }

    plDebugRenderer::DrawLines(pWorld, lines, plColor::White);
  }

  if (iView == ClothView::Colliders)
  {
    const plColor colliderColor(0.84f, 0.51f, 0.47f); // the Physics category colour
    const float fScale = m_pSetup->m_fUniformScale;

    // the debug capsule runs along Z, a collider's capsule along its own X axis
    const plQuat qZToX = plQuat::MakeShortestRotation(plVec3(0, 0, 1), plVec3(1, 0, 0));

    for (plUInt32 c = 0; c < desc.m_Colliders.GetCount() && c < m_ColliderTransforms.GetCount(); ++c)
    {
      const plJoltClothCollider& collider = desc.m_Colliders[c];
      const plTransform& joint = m_ColliderTransforms[c];

      plTransform shape;
      shape.m_vPosition = joint.m_vPosition + joint.m_qRotation * (collider.m_Transform.m_vPosition * fScale);
      shape.m_qRotation = joint.m_qRotation * collider.m_Transform.m_qRotation;
      shape.m_vScale.Set(1.0f);

      switch (collider.m_Type)
      {
        case plJoltClothCollider::Type::Sphere:
          plDebugRenderer::DrawLineSphere(pWorld, plBoundingSphere::MakeFromCenterAndRadius(plVec3::MakeZero(), collider.m_fRadius * fScale), colliderColor, shape);
          break;

        case plJoltClothCollider::Type::Capsule:
          shape.m_qRotation = shape.m_qRotation * qZToX;
          plDebugRenderer::DrawLineCapsuleZ(pWorld, collider.m_fLength * fScale, collider.m_fRadius * fScale, colliderColor, shape);
          break;

        case plJoltClothCollider::Type::Box:
          plDebugRenderer::DrawLineBox(pWorld, plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), collider.m_vHalfExtents * fScale), colliderColor, shape);
          break;
      }
    }
  }

  if (iView == ClothView::MaxDistance || iView == ClothView::Backstop || iView == ClothView::State || iView == ClothView::Cost)
  {
    // the largest max distance of the garment is the red end of its heat map
    float fLargestMaxDistance = 0.01f;
    for (float fMaxDistance : desc.m_MaxDistance)
    {
      fLargestMaxDistance = plMath::Max(fLargestMaxDistance, fMaxDistance);
    }

    plColor garmentColor = StateColor(m_State);
    if (iView == ClothView::Cost)
      garmentColor = Heat(static_cast<float>(GetLastStepDuration().GetMilliseconds()) / 0.6f);

    plDynamicArray<plDebugRenderer::Triangle> triangles;
    triangles.Reserve(indices.GetCount() / 3 * 2);

    for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
    {
      const plUInt32 a = indices[i];
      const plUInt32 b = indices[i + 1];
      const plUInt32 c = indices[i + 2];

      plColor color = garmentColor;
      if (iView == ClothView::MaxDistance)
        color = Heat((desc.m_MaxDistance[a] + desc.m_MaxDistance[b] + desc.m_MaxDistance[c]) / (3.0f * fLargestMaxDistance));
      else if (iView == ClothView::Backstop)
        color = Heat((desc.m_BackstopDistance[a] + desc.m_BackstopDistance[b] + desc.m_BackstopDistance[c]) / (3.0f * 0.2f));

      AddSurfaceTriangle(triangles, positions[a], positions[b], positions[c], color);
    }

    plDebugRenderer::DrawSolidTriangles(pWorld, triangles, plColor::White);
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltCharacterClothViews);
