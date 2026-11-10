#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/Time/Stopwatch.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <JoltPlugin/CharacterCloth/JoltClothInstance.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothTeleportMode, 1)
  PL_ENUM_CONSTANTS(plJoltClothTeleportMode::Reset, plJoltClothTeleportMode::KeepShape)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

namespace
{
  constexpr float s_fAirDensity = 1.2f;         // kg per cubic metre
  constexpr float s_fLargestInertiaKick = 10.0f; // metres per second a change in the character's speed may give the cloth

  JPH::Mat44 ToJolt(const plMat4& m)
  {
    return JPH::Mat44::sLoadFloat4x4(reinterpret_cast<const JPH::Float4*>(m.m_fElementsCM));
  }

  /// Turns a resistance from 0 to 1 into an XPBD compliance. 1 is rigid, 0 is as soft as fLogSoftest (a power of ten).
  float ToCompliance(float fResistance, float fLogSoftest, float fLogStiffest, float fStiffnessScale)
  {
    fResistance = plMath::Saturate(fResistance);
    if (fResistance >= 0.999f)
      return 0.0f;

    const float fCompliance = plMath::Pow(10.0f, plMath::Lerp(fLogSoftest, fLogStiffest, fResistance));
    return fCompliance / plMath::Max(fStiffnessScale, 0.01f);
  }

  /// A right-handed frame at a surface point: X along the tangent, Z along the normal.
  plQuat MakeFrame(const plVec3& vNormal, const plVec3& vToNeighbor)
  {
    plVec3 n = vNormal;
    if (n.NormalizeIfNotZero(plVec3::MakeZero(), 1.0e-12f).Failed())
      return plQuat::MakeIdentity();

    plVec3 t = vToNeighbor - n * n.Dot(vToNeighbor);
    if (t.NormalizeIfNotZero(plVec3::MakeZero(), 1.0e-12f).Failed())
      t = n.GetOrthogonalVector().GetNormalized();

    plMat3 m;
    m.SetColumn(0, t);
    m.SetColumn(1, n.CrossRH(t));
    m.SetColumn(2, n);

    return plQuat::MakeFromMat3(m);
  }

  /// Area-weighted vertex normals of a triangle list.
  void ComputeNormals(plArrayPtr<const plVec3> positions, plArrayPtr<const plUInt32> indices, plDynamicArray<plVec3>& out_normals)
  {
    out_normals.Clear();
    out_normals.SetCount(positions.GetCount(), plVec3::MakeZero());

    for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
    {
      const plUInt32 a = indices[i];
      const plUInt32 b = indices[i + 1];
      const plUInt32 c = indices[i + 2];

      const plVec3 vFaceNormal = (positions[b] - positions[a]).CrossRH(positions[c] - positions[a]);
      out_normals[a] += vFaceNormal;
      out_normals[b] += vFaceNormal;
      out_normals[c] += vFaceNormal;
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

plJoltClothSolverSetup::plJoltClothSolverSetup() = default;

plJoltClothSolverSetup::~plJoltClothSolverSetup()
{
  if (m_pSettings != nullptr)
  {
    m_pSettings->Release();
    m_pSettings = nullptr;
  }
}

plResult plJoltClothSolverSetup::Build(const plJoltClothDescriptor& desc, const plMat4& mBindToSim)
{
  PL_SUCCEED_OR_RETURN(desc.Validate());

  m_mBindToSim = mBindToSim;
  m_mSimToBind = mBindToSim;
  if (m_mSimToBind.Invert(0.0f).Failed())
  {
    plLog::Error("The cloth's character has a zero scale, the cloth cannot be simulated.");
    return PL_FAILURE;
  }

  const plUInt32 uiParticles = desc.GetParticleCount();
  const plJoltClothFabric& fabric = desc.m_Fabric;

  // a mirroring transform flips which side of a triangle is its front
  const plMat3 mRotation = mBindToSim.GetRotationalPart();
  const float fDeterminant = mRotation.GetColumn(0).CrossRH(mRotation.GetColumn(1)).Dot(mRotation.GetColumn(2));
  const bool bMirrored = fDeterminant < 0.0f;

  m_fUniformScale = plMath::Pow(plMath::Abs(fDeterminant), 1.0f / 3.0f);

  plDynamicArray<plVec3> simPositions;
  simPositions.SetCountUninitialized(uiParticles);
  for (plUInt32 p = 0; p < uiParticles; ++p)
  {
    simPositions[p] = mBindToSim.TransformPosition(desc.m_Positions[p]);
  }

  // each particle carries a third of the area of its triangles
  plDynamicArray<float> areas;
  areas.SetCount(uiParticles, 0.0f);
  for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
  {
    const plUInt32 a = desc.m_Indices[i];
    const plUInt32 b = desc.m_Indices[i + 1];
    const plUInt32 c = desc.m_Indices[i + 2];

    const float fThird = (simPositions[b] - simPositions[a]).CrossRH(simPositions[c] - simPositions[a]).GetLength() / 6.0f;
    areas[a] += fThird;
    areas[b] += fThird;
    areas[c] += fThird;
  }

  JPH::Ref<JPH::SoftBodySharedSettings> pSettings = new JPH::SoftBodySharedSettings();

  pSettings->mVertices.resize(uiParticles);
  for (plUInt32 p = 0; p < uiParticles; ++p)
  {
    const float fMass = plMath::Max(areas[p] * fabric.m_fDensity, 1.0e-5f);

    JPH::SoftBodySharedSettings::Vertex& vertex = pSettings->mVertices[p];
    vertex.mPosition = plJoltConversionUtils::ToFloat3(simPositions[p]);
    vertex.mInvMass = desc.m_MaxDistance[p] > 0.0f ? 1.0f / fMass : 0.0f; // a pinned particle is kinematic
  }

  for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
  {
    const plUInt32 a = desc.m_Indices[i];
    const plUInt32 b = desc.m_Indices[bMirrored ? i + 2 : i + 1];
    const plUInt32 c = desc.m_Indices[bMirrored ? i + 1 : i + 2];

    pSettings->AddFace(JPH::SoftBodySharedSettings::Face(a, b, c));
  }

  // edges, shear and bend constraints, and tethers to the nearest pinned particle
  {
    const JPH::SoftBodySharedSettings::ELRAType tether = desc.m_Simulation.m_bTethers ? JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance : JPH::SoftBodySharedSettings::ELRAType::None;

    JPH::Array<JPH::SoftBodySharedSettings::VertexAttributes> attributes;
    attributes.resize(uiParticles);

    for (plUInt32 p = 0; p < uiParticles; ++p)
    {
      const float fScale = desc.m_StiffnessScale[p];

      JPH::SoftBodySharedSettings::VertexAttributes& a = attributes[p];
      a.mCompliance = ToCompliance(fabric.m_fStretch, 0.0f, -7.0f, fScale);
      a.mShearCompliance = ToCompliance(fabric.m_fShear, 0.0f, -6.0f, fScale);
      a.mBendCompliance = ToCompliance(fabric.m_fBend, 1.0f, -5.0f, fScale);
      a.mLRAType = tether;
      a.mLRAMaxDistanceMultiplier = plMath::Max(1.0f, desc.m_Simulation.m_fTetherSlack);
    }

    pSettings->CreateConstraints(attributes.data(), static_cast<JPH::uint>(attributes.size()), JPH::SoftBodySharedSettings::EBendType::Distance);
  }

  // Skinning. Every bone shares one inverse bind matrix, because the skin transforms handed to Step() already
  // contain the bone's own: all that is left is to take the baked simulation space back to bind space.
  {
    m_Bones.Clear();

    plHashTable<plUInt32, plUInt32> boneToJoint;
    auto GetJoint = [&](plUInt32 uiBone) -> plUInt32
    {
      plUInt32 uiJoint = 0;
      if (!boneToJoint.TryGetValue(uiBone, uiJoint))
      {
        uiJoint = m_Bones.GetCount();
        m_Bones.PushBack(static_cast<plUInt16>(uiBone));
        boneToJoint.Insert(uiBone, uiJoint);

        pSettings->mInvBindMatrices.push_back(JPH::SoftBodySharedSettings::InvBind(uiJoint, ToJolt(m_mSimToBind)));
      }
      return uiJoint;
    };

    pSettings->mSkinnedConstraints.reserve(uiParticles);

    for (plUInt32 p = 0; p < uiParticles; ++p)
    {
      JPH::SoftBodySharedSettings::Skinned skinned(p, desc.m_MaxDistance[p], desc.m_BackstopDistance[p], desc.m_BackstopRadius[p]);

      // Jolt reads the weights up to the first zero
      plUInt32 uiUsed = 0;
      for (plUInt32 w = 0; w < 4; ++w)
      {
        const float fWeight = desc.m_BoneWeights[p].GetData()[w];
        if (fWeight <= 0.0f)
          continue;

        skinned.mWeights[uiUsed] = JPH::SoftBodySharedSettings::SkinWeight(GetJoint(desc.m_BoneIndices[p].GetData()[w]), fWeight);
        ++uiUsed;
      }

      if (uiUsed == 0)
      {
        plLog::Error("Cloth particle {} is not skinned to any bone.", p);
        return PL_FAILURE;
      }

      skinned.NormalizeWeights();
      pSettings->mSkinnedConstraints.push_back(skinned);
    }

    pSettings->CalculateSkinnedConstraintNormals();
  }

  pSettings->Optimize();

  m_uiNumConstraints = static_cast<plUInt32>(pSettings->mEdgeConstraints.size() + pSettings->mLRAConstraints.size() + pSettings->mSkinnedConstraints.size());

  // The edges for the stretch limit, sorted by how many edges their near end is away from a pinned particle.
  {
    m_LimitEdges.Clear();

    plHashSet<plUInt64> seen;
    plDynamicArray<plDynamicArray<plUInt32>> neighbors;
    neighbors.SetCount(uiParticles);

    for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        const plUInt32 a = desc.m_Indices[i + e];
        const plUInt32 b = desc.m_Indices[i + (e + 1) % 3];

        if (a == b || seen.Insert((static_cast<plUInt64>(plMath::Min(a, b)) << 32) | plMath::Max(a, b)))
          continue;

        neighbors[a].PushBack(b);
        neighbors[b].PushBack(a);
      }
    }

    // breadth first from the pinned particles; a piece without a pin has nothing to hang from and is left alone
    plDynamicArray<plUInt32> depth;
    depth.SetCount(uiParticles, plInvalidIndex);

    plDynamicArray<plUInt32> queue;
    for (plUInt32 p = 0; p < uiParticles; ++p)
    {
      if (desc.m_MaxDistance[p] <= 0.0f)
      {
        depth[p] = 0;
        queue.PushBack(p);
      }
    }

    for (plUInt32 q = 0; q < queue.GetCount(); ++q)
    {
      const plUInt32 p = queue[q];

      for (plUInt32 n : neighbors[p])
      {
        if (depth[n] == plInvalidIndex)
        {
          depth[n] = depth[p] + 1;
          queue.PushBack(n);
        }
      }
    }

    // Ring by ring outward from the pins. A ring is the particles of one depth: first the edges that reach it from
    // the ring before, then the edges along the ring, then the edges from the ring before once more, because the
    // edges along the ring move both of their ends.
    plDynamicArray<plDynamicArray<LimitEdge>> inward;
    plDynamicArray<plDynamicArray<LimitEdge>> along;

    for (plUInt32 p : queue)
    {
      for (plUInt32 n : neighbors[p])
      {
        if (depth[n] < depth[p] || (depth[n] == depth[p] && n < p) || desc.m_MaxDistance[n] <= 0.0f)
          continue;

        LimitEdge edge;
        edge.m_uiNear = p;
        edge.m_uiFar = n;
        edge.m_fRestLength = (simPositions[p] - simPositions[n]).GetLength();
        edge.m_bSameDepth = depth[n] == depth[p];

        plDynamicArray<plDynamicArray<LimitEdge>>& rings = edge.m_bSameDepth ? along : inward;
        if (rings.GetCount() <= depth[n])
          rings.SetCount(depth[n] + 1);

        rings[depth[n]].PushBack(edge);
      }
    }

    for (plUInt32 uiRing = 0; uiRing < plMath::Max(inward.GetCount(), along.GetCount()); ++uiRing)
    {
      if (uiRing < inward.GetCount())
        m_LimitEdges.PushBackRange(inward[uiRing]);

      if (uiRing < along.GetCount() && !along[uiRing].IsEmpty())
      {
        // a ring that is too long all the way round takes a few passes to draw in
        for (plUInt32 uiPass = 0; uiPass < 3; ++uiPass)
        {
          m_LimitEdges.PushBackRange(along[uiRing]);

          if (uiRing < inward.GetCount())
            m_LimitEdges.PushBackRange(inward[uiRing]);
        }
      }
    }
  }

  if (m_pSettings != nullptr)
    m_pSettings->Release();

  m_pSettings = pSettings.GetPtr();
  m_pSettings->AddRef();

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////

plJoltClothInstance::plJoltClothInstance() = default;

plJoltClothInstance::~plJoltClothInstance()
{
  Deinitialize();
}

plResult plJoltClothInstance::Initialize(plJoltWorldModule& ref_module, const plJoltClothDescriptor& desc, const plJoltClothSolverSetup& setup, plUInt16 uiObjectLayer, plUInt32 uiCollisionGroupId, float fColliderScale)
{
  Deinitialize();

  if (setup.m_pSettings == nullptr)
    return PL_FAILURE;

  const plJoltClothFabric& fabric = desc.m_Fabric;

  JPH::SoftBodyCreationSettings settings(setup.m_pSettings, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), uiObjectLayer);
  settings.mNumIterations = plMath::Max<plUInt32>(1, desc.m_Simulation.m_uiIterations);
  settings.mLinearDamping = plMath::Saturate(fabric.m_fDamping) * 3.0f;
  settings.mMaxLinearVelocity = 50.0f;
  settings.mRestitution = 0.0f;
  settings.mFriction = fabric.m_fFriction;
  settings.mPressure = 0.0f;
  settings.mVertexRadius = fabric.m_fThickness;
  settings.mUpdatePosition = false;       // the body follows its character, not its particles
  settings.mMakeRotationIdentity = false; // the rotation is the identity already
  settings.mAllowSleeping = false;
  settings.mCollisionGroup = JPH::CollisionGroup(ref_module.GetGroupFilterCloth(), uiCollisionGroupId, 0);

  JPH::BodyInterface& bodies = ref_module.GetJoltSystem()->GetBodyInterface();

  m_pBody = bodies.CreateSoftBody(settings);
  if (m_pBody == nullptr)
  {
    plLog::Error("Jolt has no body left for a cloth garment.");
    return PL_FAILURE;
  }

  m_pModule = &ref_module;
  m_pDesc = &desc;
  m_pSetup = &setup;
  m_uiNumParticles = desc.GetParticleCount();
  m_bHasState = false;
  m_bHasOwnerVelocity = false;
  m_fColliderScale = plMath::Max(fColliderScale, 0.001f);

  // The body colliders: kinematic bodies that only this garment collides with. They rest far below the world until
  // the first pose places them.
  {
    const JPH::ObjectLayer colliderLayer = plJoltCollisionFiltering::ConstructObjectLayer(0, plJoltBroadphaseLayer::ClothCollider);

    m_ColliderBodies.SetCount(desc.m_Colliders.GetCount(), JPH::BodyID::cInvalidBodyID);

    for (plUInt32 c = 0; c < desc.m_Colliders.GetCount(); ++c)
    {
      const plJoltClothCollider& collider = desc.m_Colliders[c];
      const float fRadius = plMath::Max(collider.m_fRadius * m_fColliderScale, 0.001f);

      JPH::Ref<JPH::Shape> pShape;
      switch (collider.m_Type)
      {
        case plJoltClothCollider::Type::Sphere:
          pShape = new JPH::SphereShape(fRadius);
          break;

        case plJoltClothCollider::Type::Capsule:
          pShape = new JPH::CapsuleShape(plMath::Max(collider.m_fLength * m_fColliderScale * 0.5f, 0.001f), fRadius);
          break;

        case plJoltClothCollider::Type::Box:
        {
          const plVec3 vHalfExtents = (collider.m_vHalfExtents * m_fColliderScale).CompMax(plVec3(0.005f));
          pShape = new JPH::BoxShape(plJoltConversionUtils::ToVec3(vHalfExtents), 0.0f);
          break;
        }
      }

      JPH::BodyCreationSettings bodySettings(pShape, JPH::RVec3(0, 0, -100000.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, colliderLayer);
      bodySettings.mCollisionGroup = JPH::CollisionGroup(ref_module.GetGroupFilterCloth(), uiCollisionGroupId, 1);
      bodySettings.mAllowSleeping = true;

      JPH::Body* pCollider = bodies.CreateBody(bodySettings);
      if (pCollider == nullptr)
      {
        plLog::Warning("Jolt has no body left for a cloth collider. The garment simulates without it.");
        continue;
      }

      bodies.AddBody(pCollider->GetID(), JPH::EActivation::DontActivate);
      m_ColliderBodies[c] = pCollider->GetID().GetIndexAndSequenceNumber();
    }
  }

  // the solver joints of each particle, for skinning it here the way the solver does
  {
    plHashTable<plUInt32, plUInt32> jointOfBone;
    for (plUInt32 j = 0; j < setup.m_Bones.GetCount(); ++j)
    {
      jointOfBone.Insert(setup.m_Bones[j], j);
    }

    m_SkinJoints.SetCount(m_uiNumParticles, plVec4U16(0, 0, 0, 0));
    m_SkinWeights.SetCount(m_uiNumParticles, plVec4::MakeZero());

    for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
    {
      float fTotal = 0.0f;

      for (plUInt32 w = 0; w < 4; ++w)
      {
        const float fWeight = desc.m_BoneWeights[p].GetData()[w];
        if (fWeight <= 0.0f)
          continue;

        m_SkinJoints[p].GetData()[w] = static_cast<plUInt16>(*jointOfBone.GetValue(static_cast<plUInt32>(desc.m_BoneIndices[p].GetData()[w])));
        m_SkinWeights[p].GetData()[w] = fWeight;
        fTotal += fWeight;
      }

      m_SkinWeights[p] /= fTotal;
    }
  }

  // the frame of each particle in the bind pose, to measure its rotation against later
  {
    plDynamicArray<plVec3> normals;
    ComputeNormals(desc.m_Positions, desc.m_Indices, normals);

    m_FrameNeighbor.SetCount(m_uiNumParticles);
    for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
    {
      m_FrameNeighbor[p] = static_cast<plUInt16>(p);
    }

    for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        const plUInt32 uiParticle = desc.m_Indices[i + e];
        if (m_FrameNeighbor[uiParticle] == uiParticle)
          m_FrameNeighbor[uiParticle] = static_cast<plUInt16>(desc.m_Indices[i + (e + 1) % 3]);
      }
    }

    m_RestFrameInverse.SetCountUninitialized(m_uiNumParticles);
    for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
    {
      m_RestFrameInverse[p] = MakeFrame(normals[p], desc.m_Positions[m_FrameNeighbor[p]] - desc.m_Positions[p]).GetInverse();
    }
  }

  return PL_SUCCESS;
}

void plJoltClothInstance::Deinitialize()
{
  if (m_pBody != nullptr)
  {
    JPH::BodyInterface& bodies = m_pModule->GetJoltSystem()->GetBodyInterface();

    for (plUInt32 uiCollider : m_ColliderBodies)
    {
      const JPH::BodyID id(uiCollider);
      if (id.IsInvalid())
        continue;

      bodies.RemoveBody(id);
      bodies.DestroyBody(id);
    }

    bodies.DestroyBody(m_pBody->GetID());
    m_pBody = nullptr;
  }

  m_ColliderBodies.Clear();

  m_pModule = nullptr;
  m_pDesc = nullptr;
  m_pSetup = nullptr;
  m_uiNumParticles = 0;
  m_bHasState = false;
  m_BoneToSim.Clear();
  m_Skinned.Clear();
}

void plJoltClothInstance::SetColliderJointTransforms(plArrayPtr<const plTransform> jointTransforms)
{
  if (m_pBody == nullptr || jointTransforms.GetCount() != m_ColliderBodies.GetCount())
    return;

  JPH::BodyInterface& bodies = m_pModule->GetJoltSystem()->GetBodyInterface();

  // a capsule shape runs along Jolt's Y axis, a collider's capsule along its own X axis
  const plQuat qCapsuleToX = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(-90));

  for (plUInt32 c = 0; c < m_ColliderBodies.GetCount(); ++c)
  {
    const JPH::BodyID id(m_ColliderBodies[c]);
    if (id.IsInvalid())
      continue;

    const plJoltClothCollider& collider = m_pDesc->m_Colliders[c];
    const plTransform& joint = jointTransforms[c];

    const plVec3 vPosition = joint.m_vPosition + joint.m_qRotation * (collider.m_Transform.m_vPosition * m_fColliderScale);
    plQuat qRotation = joint.m_qRotation * collider.m_Transform.m_qRotation;
    if (collider.m_Type == plJoltClothCollider::Type::Capsule)
      qRotation = qRotation * qCapsuleToX;

    qRotation.Normalize();

    bodies.SetPositionAndRotation(id, JPH::RVec3(plJoltConversionUtils::ToVec3(vPosition)), plJoltConversionUtils::ToQuat(qRotation), JPH::EActivation::DontActivate);
  }
}

void plJoltClothInstance::ComputeSkinnedPositions(float fTimeStep, bool bReset)
{
  const bool bHasPrevious = !bReset && m_Skinned.GetCount() == m_uiNumParticles;

  m_Skinned.SetCount(m_uiNumParticles);
  m_SkinnedVelocity.SetCount(m_uiNumParticles);

  for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
  {
    const plVec3 vBind = m_pDesc->m_Positions[p];
    plVec3 vSkinned = plVec3::MakeZero();

    for (plUInt32 w = 0; w < 4; ++w)
    {
      const float fWeight = m_SkinWeights[p].GetData()[w];
      if (fWeight > 0.0f)
        vSkinned += m_BoneToSim[m_SkinJoints[p].GetData()[w]].TransformPosition(vBind) * fWeight;
    }

    m_SkinnedVelocity[p] = bHasPrevious ? (vSkinned - m_Skinned[p]) / fTimeStep : plVec3::MakeZero();
    m_Skinned[p] = vSkinned;
  }
}

void plJoltClothInstance::ClampToEnvelope(float fMaxDistanceScale)
{
  // The solver applies the max distance in the middle of each sub step and other constraints move the particles
  // afterwards. This makes the envelope exact: no particle ends a step further from its skinned place than it may be.
  auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  JPH::Array<JPH::SoftBodyVertex>& vertices = pMotion->GetVertices();

  for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
  {
    const JPH::Vec3 skinned = plJoltConversionUtils::ToVec3(m_Skinned[p]);
    const float fMaxDistance = m_pDesc->m_MaxDistance[p] * fMaxDistanceScale;

    const JPH::Vec3 offset = vertices[p].mPosition - skinned;
    const float fDistanceSqr = offset.LengthSq();

    if (fMaxDistance <= 0.0f)
    {
      vertices[p].mPosition = skinned;
    }
    else if (fDistanceSqr > fMaxDistance * fMaxDistance)
    {
      const JPH::Vec3 outward = offset / plMath::Sqrt(fDistanceSqr);
      vertices[p].mPosition = skinned + outward * fMaxDistance;

      // the particle may not keep moving out of the envelope: drop that part of its velocity, measured against the
      // velocity of the envelope itself
      const JPH::Vec3 envelopeVelocity = plJoltConversionUtils::ToVec3(m_SkinnedVelocity[p]);
      const JPH::Vec3 relative = vertices[p].mVelocity - envelopeVelocity;
      const float fOutwardSpeed = relative.Dot(outward);
      if (fOutwardSpeed > 0.0f)
        vertices[p].mVelocity = envelopeVelocity + relative - outward * fOutwardSpeed;
    }
  }
}

void plJoltClothInstance::LimitStretch(float fLimit)
{
  // A solver with a handful of iterations lets a long garment stretch like rubber when it is pulled hard: in a
  // sprint, a spin or a hard stop. Walking the edges once outward from the pins and pulling the far end of every
  // edge that is too long back in ends that, because each edge is put right after the ones that hold its near end.
  if (fLimit <= 0.0f)
    return;

  auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  JPH::Array<JPH::SoftBodyVertex>& vertices = pMotion->GetVertices();

  for (const plJoltClothSolverSetup::LimitEdge& edge : m_pSetup->m_LimitEdges)
  {
    JPH::SoftBodyVertex& nearEnd = vertices[edge.m_uiNear];
    JPH::SoftBodyVertex& farEnd = vertices[edge.m_uiFar];

    const JPH::Vec3 delta = farEnd.mPosition - nearEnd.mPosition;
    const float fLength = delta.Length();
    const float fLongest = edge.m_fRestLength * (1.0f + fLimit);

    if (fLength <= fLongest || fLength < 1.0e-6f)
      continue;

    const JPH::Vec3 along = delta / fLength;
    const float fTooLong = fLength - fLongest;

    // the ends may not go on moving apart either
    const float fApart = (farEnd.mVelocity - nearEnd.mVelocity).Dot(along);

    if (edge.m_bSameDepth && nearEnd.mInvMass > 0.0f)
    {
      nearEnd.mPosition += along * (fTooLong * 0.5f);
      farEnd.mPosition -= along * (fTooLong * 0.5f);

      if (fApart > 0.0f)
      {
        nearEnd.mVelocity += along * (fApart * 0.5f);
        farEnd.mVelocity -= along * (fApart * 0.5f);
      }
    }
    else
    {
      farEnd.mPosition -= along * fTooLong;

      if (fApart > 0.0f)
        farEnd.mVelocity -= along * fApart;
    }
  }
}

void plJoltClothInstance::FollowOwner(const StepInput& input, bool bKeepShape)
{
  // The body origin moved with the character, so the particles went along. What the cloth feels of that is the
  // character speeding up, slowing down and turning, for the share that inertia lets through.
  const float fLinear = bKeepShape ? 0.0f : plMath::Saturate(input.m_fLinearInertia);
  const float fAngular = bKeepShape ? 0.0f : plMath::Saturate(input.m_fAngularInertia);

  // Speeding up or slowing down pushes the cloth the other way. At a steady speed nothing pushes: the air the
  // character runs through is what the aerodynamics are for.
  plVec3 vKick = plVec3::MakeZero();

  if (!bKeepShape)
  {
    const plVec3 vOwnerVelocity = (input.m_vOwnerPosition - m_vLastOwnerPosition) / input.m_fTimeStep;

    // the first step after a start or a teleport only learns how fast the character is
    if (m_bHasOwnerVelocity)
    {
      vKick = -(vOwnerVelocity - m_vLastOwnerVelocity) * fLinear;

      // a character that is snapped into place for one frame must not fling its cloth away
      const float fKick = vKick.GetLength();
      if (fKick > s_fLargestInertiaKick)
        vKick *= s_fLargestInertiaKick / fKick;
    }

    m_vLastOwnerVelocity = vOwnerVelocity;
    m_bHasOwnerVelocity = true;
  }
  else
  {
    m_bHasOwnerVelocity = false;
  }

  // Turning: the particles keep their place in the world for the share that inertia lets through.
  const plQuat qDelta = input.m_qOwnerRotation * m_qLastOwnerRotation.GetInverse();
  const plQuat qCarried = plQuat::MakeSlerp(plQuat::MakeIdentity(), qDelta, 1.0f - fAngular);

  const JPH::Vec3 kick = plJoltConversionUtils::ToVec3(vKick);
  const JPH::Quat carried = plJoltConversionUtils::ToQuat(qCarried).Normalized();

  auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  for (JPH::SoftBodyVertex& vertex : pMotion->GetVertices())
  {
    vertex.mPosition = carried * vertex.mPosition;
    vertex.mVelocity = carried * vertex.mVelocity;

    if (vertex.mInvMass > 0.0f)
      vertex.mVelocity += kick;
  }
}

void plJoltClothInstance::ApplyAerodynamics(const StepInput& input, const plVec3& vOwnerVelocity)
{
  const plJoltClothFabric& fabric = m_pDesc->m_Fabric;

  const float fDrag = fabric.m_fDrag * 2.0f;
  const float fLift = fabric.m_fLift * 1.5f;
  if (fDrag <= 0.0f && fLift <= 0.0f)
    return;

  // particle velocities are relative to the body, which moves with the character
  const JPH::Vec3 air = plJoltConversionUtils::ToVec3(input.m_vWind - vOwnerVelocity);
  const float fTimeStep = input.m_fTimeStep;

  auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  JPH::Array<JPH::SoftBodyVertex>& vertices = pMotion->GetVertices();

  for (const JPH::SoftBodySharedSettings::Face& face : pMotion->GetFaces())
  {
    JPH::SoftBodyVertex& v0 = vertices[face.mVertex[0]];
    JPH::SoftBodyVertex& v1 = vertices[face.mVertex[1]];
    JPH::SoftBodyVertex& v2 = vertices[face.mVertex[2]];

    const JPH::Vec3 relative = air - (v0.mVelocity + v1.mVelocity + v2.mVelocity) / 3.0f;
    const float fSpeed = relative.Length();
    if (fSpeed < 0.01f)
      continue;

    JPH::Vec3 normal = (v1.mPosition - v0.mPosition).Cross(v2.mPosition - v0.mPosition);
    const float fDoubleArea = normal.Length();
    if (fDoubleArea < 1.0e-10f)
      continue;

    normal /= fDoubleArea;

    const JPH::Vec3 direction = relative / fSpeed;
    const float fCos = direction.Dot(normal);
    const float fFacing = plMath::Abs(fCos);

    // drag pushes along the flow, lift pushes across it, both in proportion to the area the flow sees
    const float fPressure = 0.5f * s_fAirDensity * (fDoubleArea * 0.5f) * fFacing * fSpeed * fSpeed;
    const JPH::Vec3 force = (direction * fDrag + (normal * plMath::Sign(fCos) - direction * fFacing) * fLift) * fPressure;

    // never push a particle past the speed of the air that pushes it
    const float fMaxDeltaSpeed = fSpeed * 0.5f;

    for (JPH::SoftBodyVertex* pVertex : {&v0, &v1, &v2})
    {
      if (pVertex->mInvMass <= 0.0f)
        continue;

      JPH::Vec3 delta = force * (pVertex->mInvMass * fTimeStep / 3.0f);
      const float fDeltaSpeed = delta.Length();
      if (fDeltaSpeed > fMaxDeltaSpeed)
        delta *= fMaxDeltaSpeed / fDeltaSpeed;

      pVertex->mVelocity += delta;
    }
  }
}

void plJoltClothInstance::Step(const StepInput& input)
{
  if (m_pBody == nullptr || input.m_fTimeStep <= 0.0f)
    return;

  plStopwatch timer;

  auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  const plJoltClothSimulationSettings& simulation = m_pDesc->m_Simulation;

  const float fMaxDistanceScale = plMath::Max(0.0f, input.m_fMaxDistanceScale);
  const plVec3 vOwnerDelta = input.m_vOwnerPosition - m_vLastOwnerPosition;
  const bool bTeleported = m_bHasState && vOwnerDelta.GetLength() > input.m_fTeleportDistance;
  const bool bReset = !m_bHasState || input.m_bReset || (bTeleported && input.m_TeleportMode == plJoltClothTeleportMode::Reset);

  m_pBody->SetPositionAndRotationInternal(JPH::RVec3(plJoltConversionUtils::ToVec3(input.m_vOwnerPosition)), JPH::Quat::sIdentity());

  if (!bReset)
  {
    FollowOwner(input, bTeleported);
  }
  else
  {
    // A reset on the spot, such as after a camera cut, keeps track of the character's speed. A start or a teleport
    // says nothing about it.
    m_bHasOwnerVelocity = m_bHasState && !bTeleported;

    if (m_bHasOwnerVelocity)
      m_vLastOwnerVelocity = vOwnerDelta / input.m_fTimeStep;
  }

  // the pose: bind space to simulation space for every bone the cloth is skinned to
  {
    const plMat4 mToSim = input.m_qOwnerRotation.GetAsMat4() * m_pSetup->m_mBindToSim;

    m_BoneToSim.SetCountUninitialized(m_pSetup->m_Bones.GetCount());
    for (plUInt32 j = 0; j < m_BoneToSim.GetCount(); ++j)
    {
      const plUInt16 uiBone = m_pSetup->m_Bones[j];
      m_BoneToSim[j] = uiBone < input.m_SkinTransforms.GetCount() ? mToSim * input.m_SkinTransforms[uiBone] : mToSim;
    }
  }

  {
    JPH::Array<JPH::Mat44> joints(m_BoneToSim.GetCount());
    for (plUInt32 j = 0; j < m_BoneToSim.GetCount(); ++j)
    {
      joints[j] = ToJolt(m_BoneToSim[j]);
    }

    JPH::TempAllocatorMalloc allocator;
    pMotion->SetSkinnedMaxDistanceMultiplier(fMaxDistanceScale);
    pMotion->SkinVertices(m_pBody->GetCenterOfMassTransform(), joints.data(), static_cast<JPH::uint>(joints.size()), bReset, allocator);
  }

  ComputeSkinnedPositions(input.m_fTimeStep, bReset);

  if (!bReset)
  {
    // a teleport is not motion through the air
    const plVec3 vOwnerVelocity = bTeleported ? plVec3::MakeZero() : vOwnerDelta / input.m_fTimeStep;
    ApplyAerodynamics(input, vOwnerVelocity);
  }

  // sub steps keep their length, so the cloth behaves the same at every frame rate
  const float fSubStep = 1.0f / (plMath::Max(simulation.m_fStepRate, 1.0f) * plMath::Max<plUInt32>(1, simulation.m_uiIterations));
  const plUInt32 uiMaxIterations = plMath::Max<plUInt32>(1, simulation.m_uiIterations) * 3;
  m_uiLastIterations = plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Round(input.m_fTimeStep / fSubStep)), 1, uiMaxIterations);

  pMotion->SetNumIterations(m_uiLastIterations);
  pMotion->SetGravityFactor(input.m_fGravityFactor * simulation.m_fGravityFactor);
  pMotion->CustomUpdate(input.m_fTimeStep, *m_pBody, *m_pModule->GetJoltSystem());

  // The envelope has the last word, so that no particle ends a step outside it. Pulling a particle back into its
  // envelope can lengthen an edge again, and so can an edge between two particles that are equally far from the
  // pins; a few rounds settle most of that.
  for (plUInt32 uiRound = 0; uiRound < 4; ++uiRound)
  {
    LimitStretch(simulation.m_fStretchLimit);
    ClampToEnvelope(fMaxDistanceScale);
  }

  m_vLastOwnerPosition = input.m_vOwnerPosition;
  m_qLastOwnerRotation = input.m_qOwnerRotation;
  m_bHasState = true;
  m_bResetInLastStep = bReset;
  m_LastStepDuration = timer.GetRunningTotal();
}

void plJoltClothInstance::GetSimPositions(plDynamicArray<plVec3>& out_positions) const
{
  out_positions.Clear();
  if (m_pBody == nullptr)
    return;

  const auto* pMotion = static_cast<const JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());

  out_positions.Reserve(m_uiNumParticles);
  for (const JPH::SoftBodyVertex& vertex : pMotion->GetVertices())
  {
    out_positions.PushBack(plJoltConversionUtils::ToVec3(vertex.mPosition));
  }
}

void plJoltClothInstance::GetSkinnedPositions(plDynamicArray<plVec3>& out_positions) const
{
  out_positions = m_Skinned;
}

plBoundingBox plJoltClothInstance::GetSimBounds() const
{
  if (m_pBody == nullptr)
    return plBoundingBox::MakeInvalid();

  const JPH::AABox& bounds = static_cast<const JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties())->GetLocalBounds();
  return plBoundingBox::MakeFromMinMax(plJoltConversionUtils::ToVec3(bounds.mMin), plJoltConversionUtils::ToVec3(bounds.mMax));
}

void plJoltClothInstance::GetRenderParticles(plDynamicArray<plClothParticleState>& out_particles) const
{
  out_particles.Clear();
  if (m_pBody == nullptr)
    return;

  const auto* pMotion = static_cast<const JPH::SoftBodyMotionProperties*>(m_pBody->GetMotionProperties());
  const JPH::Array<JPH::SoftBodyVertex>& vertices = pMotion->GetVertices();

  // simulation space back to the space skinning writes to
  const plMat4 mToSkinSpace = m_pSetup->m_mSimToBind * m_qLastOwnerRotation.GetInverse().GetAsMat4();

  plDynamicArray<plVec3> positions;
  positions.SetCountUninitialized(m_uiNumParticles);
  for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
  {
    positions[p] = mToSkinSpace.TransformPosition(plJoltConversionUtils::ToVec3(vertices[p].mPosition));
  }

  plDynamicArray<plVec3> normals;
  ComputeNormals(positions, m_pDesc->m_Indices, normals);

  out_particles.SetCountUninitialized(m_uiNumParticles);
  for (plUInt32 p = 0; p < m_uiNumParticles; ++p)
  {
    plClothParticleState& particle = out_particles[p];
    particle.m_vPosition = positions[p];
    particle.m_fPadding = 0.0f;
    particle.m_qRotation = MakeFrame(normals[p], positions[m_FrameNeighbor[p]] - positions[p]) * m_RestFrameInverse[p];
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothInstance);
