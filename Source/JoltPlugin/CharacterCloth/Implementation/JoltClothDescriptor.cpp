#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

namespace
{
  constexpr plUInt8 s_uiDescriptorVersion = 1;
} // namespace

plResult plJoltClothFabric::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << m_fDensity;
  inout_stream << m_fStretch;
  inout_stream << m_fShear;
  inout_stream << m_fBend;
  inout_stream << m_fDamping;
  inout_stream << m_fDrag;
  inout_stream << m_fLift;
  inout_stream << m_fFriction;
  inout_stream << m_fThickness;

  return PL_SUCCESS;
}

plResult plJoltClothFabric::Deserialize(plStreamReader& inout_stream)
{
  inout_stream >> m_fDensity;
  inout_stream >> m_fStretch;
  inout_stream >> m_fShear;
  inout_stream >> m_fBend;
  inout_stream >> m_fDamping;
  inout_stream >> m_fDrag;
  inout_stream >> m_fLift;
  inout_stream >> m_fFriction;
  inout_stream >> m_fThickness;

  return PL_SUCCESS;
}

bool plJoltClothCollider::PushOut(const plTransform& shapeTransform, float fScale, float fMargin, plVec3& ref_vPoint) const
{
  const plVec3 vLocal = shapeTransform.m_qRotation.GetInverse() * (ref_vPoint - shapeTransform.m_vPosition);

  plVec3 vPushed = vLocal;

  if (m_Type == Type::Box)
  {
    const plVec3 vHalf = m_vHalfExtents * fScale + plVec3(fMargin);

    const plVec3 vInside(vHalf.x - plMath::Abs(vLocal.x), vHalf.y - plMath::Abs(vLocal.y), vHalf.z - plMath::Abs(vLocal.z));
    if (vInside.x <= 0.0f || vInside.y <= 0.0f || vInside.z <= 0.0f)
      return false;

    // out through the nearest face
    if (vInside.x <= vInside.y && vInside.x <= vInside.z)
      vPushed.x = vLocal.x < 0.0f ? -vHalf.x : vHalf.x;
    else if (vInside.y <= vInside.z)
      vPushed.y = vLocal.y < 0.0f ? -vHalf.y : vHalf.y;
    else
      vPushed.z = vLocal.z < 0.0f ? -vHalf.z : vHalf.z;
  }
  else
  {
    // a sphere is a capsule without length; a capsule runs along the local X axis
    const float fHalfLength = m_Type == Type::Capsule ? m_fLength * fScale * 0.5f : 0.0f;
    const float fDistance = m_fRadius * fScale + fMargin;

    const plVec3 vOnAxis(plMath::Clamp(vLocal.x, -fHalfLength, fHalfLength), 0, 0);
    plVec3 vOut = vLocal - vOnAxis;

    const float fLength = vOut.GetLength();
    if (fLength >= fDistance)
      return false;

    // a point on the axis has no way out of its own: it leaves sideways
    if (fLength < 1.0e-6f)
      vOut.Set(0, 0, 1);
    else
      vOut /= fLength;

    vPushed = vOnAxis + vOut * fDistance;
  }

  ref_vPoint = shapeTransform.m_vPosition + shapeTransform.m_qRotation * vPushed;
  return true;
}

plResult plJoltClothCollider::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << m_sJoint;
  inout_stream << static_cast<plUInt8>(m_Type);
  inout_stream << m_Transform;
  inout_stream << m_fRadius;
  inout_stream << m_fLength;
  inout_stream << m_vHalfExtents;
  inout_stream << m_JointBindPose;
  inout_stream << m_fInflate;

  return PL_SUCCESS;
}

plResult plJoltClothCollider::Deserialize(plStreamReader& inout_stream)
{
  plUInt8 uiType = 0;

  inout_stream >> m_sJoint;
  inout_stream >> uiType;
  inout_stream >> m_Transform;
  inout_stream >> m_fRadius;
  inout_stream >> m_fLength;
  inout_stream >> m_vHalfExtents;
  inout_stream >> m_JointBindPose;
  inout_stream >> m_fInflate;

  if (uiType > static_cast<plUInt8>(Type::Box))
    return PL_FAILURE;

  m_Type = static_cast<Type>(uiType);
  return PL_SUCCESS;
}

plResult plJoltClothSimulationSettings::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << m_fStepRate;
  inout_stream << m_uiIterations;
  inout_stream << m_bTethers;
  inout_stream << m_fTetherSlack;
  inout_stream << m_WorldCollision.GetValue();
  inout_stream << m_fGravityFactor;
  inout_stream << m_fFullRateDistance;
  inout_stream << m_fSimulateDistance;
  inout_stream << m_fStretchLimit;

  return PL_SUCCESS;
}

plResult plJoltClothSimulationSettings::Deserialize(plStreamReader& inout_stream)
{
  plUInt8 uiWorldCollision = 0;

  inout_stream >> m_fStepRate;
  inout_stream >> m_uiIterations;
  inout_stream >> m_bTethers;
  inout_stream >> m_fTetherSlack;
  inout_stream >> uiWorldCollision;
  inout_stream >> m_fGravityFactor;
  inout_stream >> m_fFullRateDistance;
  inout_stream >> m_fSimulateDistance;
  inout_stream >> m_fStretchLimit;

  if (uiWorldCollision > plJoltClothWorldCollision::All)
    return PL_FAILURE;

  m_WorldCollision = static_cast<plJoltClothWorldCollision::Enum>(uiWorldCollision);
  return PL_SUCCESS;
}

plResult plJoltClothDescriptor::Validate() const
{
  const plUInt32 uiParticles = m_Positions.GetCount();

  if (uiParticles == 0 || m_Indices.IsEmpty())
  {
    plLog::Error("The cloth has no particles or no triangles.");
    return PL_FAILURE;
  }

  if (uiParticles > plClothDeformation::MaxParticles)
  {
    plLog::Error("The cloth has {} particles, the limit is {}.", uiParticles, plClothDeformation::MaxParticles);
    return PL_FAILURE;
  }

  if (m_BoneIndices.GetCount() != uiParticles || m_BoneWeights.GetCount() != uiParticles || m_MaxDistance.GetCount() != uiParticles ||
      m_BackstopDistance.GetCount() != uiParticles || m_BackstopRadius.GetCount() != uiParticles || m_StiffnessScale.GetCount() != uiParticles)
  {
    plLog::Error("The cloth's particle arrays have different sizes.");
    return PL_FAILURE;
  }

  if (m_Indices.GetCount() % 3 != 0)
  {
    plLog::Error("The cloth's index count {} is not a multiple of three.", m_Indices.GetCount());
    return PL_FAILURE;
  }

  for (plUInt32 uiIndex : m_Indices)
  {
    if (uiIndex >= uiParticles)
    {
      plLog::Error("A cloth triangle uses particle {}, there are {}.", uiIndex, uiParticles);
      return PL_FAILURE;
    }
  }

  for (const plClothVertexBinding& binding : m_Bindings)
  {
    if (binding.m_uiParticle0 == plInvalidIndex)
      continue;

    if (binding.m_uiParticle0 >= uiParticles || binding.m_uiParticle1 >= uiParticles || binding.m_uiParticle2 >= uiParticles)
    {
      plLog::Error("A render vertex is bound to a particle outside the cloth's {} particles.", uiParticles);
      return PL_FAILURE;
    }
  }

  return PL_SUCCESS;
}

plResult plJoltClothDescriptor::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << s_uiDescriptorVersion;

  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_Positions));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BoneIndices));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BoneWeights));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_MaxDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BackstopDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_BackstopRadius));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_StiffnessScale));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_Indices));

  // bindings are plain data, written as one block
  inout_stream << m_Bindings.GetCount();
  if (!m_Bindings.IsEmpty())
  {
    PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(m_Bindings.GetData(), m_Bindings.GetCount() * sizeof(plClothVertexBinding)));
  }

  PL_SUCCEED_OR_RETURN(m_Fabric.Serialize(inout_stream));
  PL_SUCCEED_OR_RETURN(m_Simulation.Serialize(inout_stream));
  PL_SUCCEED_OR_RETURN(inout_stream.WriteArray(m_Colliders));

  inout_stream << m_sMesh;
  inout_stream << m_RootTransform;

  return PL_SUCCESS;
}

plResult plJoltClothDescriptor::Deserialize(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion != s_uiDescriptorVersion)
  {
    plLog::Error("Cloth data has version {}, this build reads version {}. Transform the cloth asset again.", uiVersion, s_uiDescriptorVersion);
    return PL_FAILURE;
  }

  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Positions));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BoneIndices));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BoneWeights));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_MaxDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BackstopDistance));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_BackstopRadius));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_StiffnessScale));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Indices));

  plUInt32 uiBindings = 0;
  inout_stream >> uiBindings;
  m_Bindings.SetCountUninitialized(uiBindings);
  if (uiBindings > 0)
  {
    const plUInt64 uiBytes = plUInt64(uiBindings) * sizeof(plClothVertexBinding);
    if (inout_stream.ReadBytes(m_Bindings.GetData(), uiBytes) != uiBytes)
      return PL_FAILURE;
  }

  PL_SUCCEED_OR_RETURN(m_Fabric.Deserialize(inout_stream));
  PL_SUCCEED_OR_RETURN(m_Simulation.Deserialize(inout_stream));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Colliders));

  inout_stream >> m_sMesh;
  inout_stream >> m_RootTransform;

  return Validate();
}

plUInt64 plJoltClothDescriptor::GetHeapMemoryUsage() const
{
  return m_Positions.GetHeapMemoryUsage() + m_BoneIndices.GetHeapMemoryUsage() + m_BoneWeights.GetHeapMemoryUsage() + m_MaxDistance.GetHeapMemoryUsage() +
         m_BackstopDistance.GetHeapMemoryUsage() + m_BackstopRadius.GetHeapMemoryUsage() + m_StiffnessScale.GetHeapMemoryUsage() +
         m_Indices.GetHeapMemoryUsage() + m_Bindings.GetHeapMemoryUsage() + m_Colliders.GetHeapMemoryUsage();
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothDescriptor);
