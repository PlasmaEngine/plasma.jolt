#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <JoltPlugin/CharacterCloth/JoltClothStressTest.h>
#include <JoltPlugin/System/JoltCollisionFiltering.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <RendererCore/AnimationSystem/Declarations.h>

namespace
{
  /// How deep a point is inside a shape, in metres. Negative outside. vPoint and shape are in the same space.
  float GetPenetration(const plJoltClothCollider& collider, const plTransform& shape, float fScale, const plVec3& vPoint)
  {
    const plVec3 vLocal = shape.m_qRotation.GetInverse() * (vPoint - shape.m_vPosition);

    switch (collider.m_Type)
    {
      case plJoltClothCollider::Type::Sphere:
        return collider.m_fRadius * fScale - vLocal.GetLength();

      case plJoltClothCollider::Type::Capsule:
      {
        // along the local X axis
        const float fHalfLength = collider.m_fLength * fScale * 0.5f;
        const plVec3 vOnAxis(plMath::Clamp(vLocal.x, -fHalfLength, fHalfLength), 0, 0);

        return collider.m_fRadius * fScale - (vLocal - vOnAxis).GetLength();
      }

      case plJoltClothCollider::Type::Box:
      {
        // the distance to the nearest face; only meaningful inside the box, which is all that is asked
        const plVec3 vHalf = collider.m_vHalfExtents * fScale;
        return plMath::Min(vHalf.x - plMath::Abs(vLocal.x), vHalf.y - plMath::Abs(vLocal.y), vHalf.z - plMath::Abs(vLocal.z));
      }
    }

    return -1.0f;
  }

  constexpr plUInt8 s_uiBaselineVersion = 1;
} // namespace

plUInt32 plJoltClothStressResult::FindFirstFailure(plJoltClothStressGate::Enum gate) const
{
  for (plUInt32 f = 0; f < m_Frames.GetCount(); ++f)
  {
    if ((m_Frames[f].m_uiFailedGates & gate) != 0)
      return f;
  }

  return plInvalidIndex;
}

void plJoltClothStressResult::GetSuggestions(plDynamicArray<plString>& out_suggestions) const
{
  out_suggestions.Clear();

  const plUInt8 uiFailed = m_Worst.m_uiFailedGates;

  if (uiFailed & plJoltClothStressGate::NotFinite)
    out_suggestions.PushBack("The cloth blew up. Lower Max Distance where the garment is tight, and check the sim mesh for loose or doubled vertices.");

  if (uiFailed & plJoltClothStressGate::InsideBody)
    out_suggestions.PushBack("The cloth goes through the body. Run Auto Select so that the shapes it can reach collide, or raise Collider Inflate. Where no shape covers the body, add a collider or paint a backstop.");

  if (uiFailed & plJoltClothStressGate::Stretch)
    out_suggestions.PushBack("The cloth stretches. Keep Stretch Limit and Tethers on. Where it still stretches, the cloth is held two ways: against a body shape, or against its max distance. The Strain view shows where: lower Collider Inflate, move the garment off the body there, or let Max Distance grow more gently from the pinned edge.");

  if (uiFailed & plJoltClothStressGate::Pop)
    out_suggestions.PushBack("The cloth pops. Set Teleport Mode to Reset or Keep Shape with a Teleport Distance below the jump, and lower Linear Inertia if it happens on a hard stop.");

  if (uiFailed & plJoltClothStressGate::Overshoot)
    out_suggestions.PushBack("Particles leave their max distance. That is a fault in the solver, not in the asset: please report it with the cloth file.");

  if (uiFailed & plJoltClothStressGate::Cost)
    out_suggestions.PushBack("A step costs too much. Raise Weld Distance or simulate a CLOTHPROXY_ mesh to get fewer particles, or lower Iterations.");
}

plResult plJoltClothStressResult::SerializeBaseline(plStreamWriter& inout_stream) const
{
  inout_stream << s_uiBaselineVersion;
  inout_stream << m_Worst.m_fStretch;
  inout_stream << m_Worst.m_fOvershoot;
  inout_stream << m_Worst.m_fRelativeSpeed;
  inout_stream << m_Worst.m_fSolveMs;
  inout_stream << m_Worst.m_uiInsideBody;
  inout_stream << m_Worst.m_uiFailedGates;

  return PL_SUCCESS;
}

plResult plJoltClothStressResult::DeserializeBaseline(plStreamReader& inout_stream, plJoltClothStressFrame& out_worst)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion != s_uiBaselineVersion)
    return PL_FAILURE;

  inout_stream >> out_worst.m_fStretch;
  inout_stream >> out_worst.m_fOvershoot;
  inout_stream >> out_worst.m_fRelativeSpeed;
  inout_stream >> out_worst.m_fSolveMs;
  inout_stream >> out_worst.m_uiInsideBody;
  inout_stream >> out_worst.m_uiFailedGates;

  return PL_SUCCESS;
}

bool plJoltClothStressResult::IsWorseThan(const plJoltClothStressFrame& baseline, float fTolerance, plStringBuilder& out_sWhy) const
{
  out_sWhy.Clear();

  // a measure is worse when it grew by more than the tolerance and by more than what two runs differ by anyway
  auto Compare = [&](const char* szWhat, float fNow, float fBefore, float fNoise, const char* szUnit)
  {
    if (fNow > fBefore * (1.0f + fTolerance) + fNoise)
      out_sWhy.AppendFormat("{} went from {} to {}{}. ", szWhat, plArgF(fBefore, 3), plArgF(fNow, 3), szUnit);
  };

  Compare("Stretch", m_Worst.m_fStretch * 100.0f, baseline.m_fStretch * 100.0f, 0.5f, " %");
  Compare("Distance beyond the max distance", m_Worst.m_fOvershoot, baseline.m_fOvershoot, 0.001f, " m");
  Compare("Speed against the body", m_Worst.m_fRelativeSpeed, baseline.m_fRelativeSpeed, 0.5f, " m/s");

  if (m_Worst.m_uiInsideBody > baseline.m_uiInsideBody)
    out_sWhy.AppendFormat("Particles inside the body went from {} to {}. ", baseline.m_uiInsideBody, m_Worst.m_uiInsideBody);

  if ((m_Worst.m_uiFailedGates & ~baseline.m_uiFailedGates) != 0)
    out_sWhy.Append("A gate fails that passed before. ");

  return !out_sWhy.IsEmpty();
}

//////////////////////////////////////////////////////////////////////////

void plJoltClothStressTest::Begin(const plJoltClothDescriptor& desc, const plJoltClothSolverSetup& setup, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, float fShapeScale)
{
  m_pDesc = &desc;
  m_pSetup = &setup;
  m_BodyShapes = bodyShapes;
  m_Gates = gates;
  m_fShapeScale = fShapeScale;

  m_Result = plJoltClothStressResult();
  m_bHasLastOffsets = false;
  m_fTime = 0.0f;
  m_fStretchedFor = 0.0f;

  // rest lengths in the units the simulation runs in
  const plDynamicArray<plUInt32>& indices = desc.m_Indices;
  m_RestLengths.SetCountUninitialized(indices.GetCount());

  for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
  {
    for (plUInt32 e = 0; e < 3; ++e)
    {
      const plVec3 a = setup.m_mBindToSim.TransformPosition(desc.m_Positions[indices[i + e]]);
      const plVec3 b = setup.m_mBindToSim.TransformPosition(desc.m_Positions[indices[i + (e + 1) % 3]]);

      m_RestLengths[i + e] = (a - b).GetLength();
    }
  }
}

void plJoltClothStressTest::AddEmptyFrame(float fTimeStep)
{
  plJoltClothStressFrame frame;
  frame.m_fTime = m_fTime;

  m_fTime += fTimeStep;
  m_bHasLastOffsets = false;
  m_fStretchedFor = 0.0f;

  m_Result.m_Frames.PushBack(frame);
}

void plJoltClothStressTest::AddFrame(const plJoltClothInstance& instance, const plVec3& vOwnerPosition, plArrayPtr<const plTransform> bodyJoints, float fTimeStep, float fMaxDistanceScale)
{
  plJoltClothStressFrame frame;
  frame.m_fTime = m_fTime;
  frame.m_fSolveMs = static_cast<float>(instance.GetLastStepDuration().GetMilliseconds());

  m_fTime += fTimeStep;

  instance.GetSimPositions(m_Positions);
  instance.GetSkinnedPositions(m_Skinned);

  const plUInt32 uiParticles = m_Positions.GetCount();
  bool bFinite = uiParticles == m_pDesc->GetParticleCount() && m_Skinned.GetCount() == uiParticles;

  for (plUInt32 p = 0; p < uiParticles && bFinite; ++p)
  {
    if (!m_Positions[p].IsValid())
      bFinite = false;
  }

  if (!bFinite)
  {
    frame.m_uiFailedGates = plJoltClothStressGate::NotFinite;
  }
  else
  {
    // how far beyond its max distance, and how fast against the body
    const bool bCompareOffsets = m_bHasLastOffsets && !instance.DidResetInLastStep() && fTimeStep > 0.0f;
    m_LastOffsets.SetCount(uiParticles, plVec3::MakeZero());

    for (plUInt32 p = 0; p < uiParticles; ++p)
    {
      const plVec3 vOffset = m_Positions[p] - m_Skinned[p];

      frame.m_fOvershoot = plMath::Max(frame.m_fOvershoot, vOffset.GetLength() - m_pDesc->m_MaxDistance[p] * fMaxDistanceScale);

      if (bCompareOffsets)
        frame.m_fRelativeSpeed = plMath::Max(frame.m_fRelativeSpeed, (vOffset - m_LastOffsets[p]).GetLength() / fTimeStep);

      m_LastOffsets[p] = vOffset;
    }

    m_bHasLastOffsets = true;

    // the longest edge against its rest length
    const plDynamicArray<plUInt32>& indices = m_pDesc->m_Indices;

    for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        const float fRest = m_RestLengths[i + e];
        if (fRest <= 0.0f)
          continue;

        // an edge between two pinned particles is as long as skinning makes it: no cloth setting changes that
        if (m_pDesc->m_MaxDistance[indices[i + e]] <= 0.0f && m_pDesc->m_MaxDistance[indices[i + (e + 1) % 3]] <= 0.0f)
          continue;

        const float fNow = (m_Positions[indices[i + e]] - m_Positions[indices[i + (e + 1) % 3]]).GetLength();

        if (fNow / fRest - 1.0f > frame.m_fStretch)
        {
          frame.m_fStretch = fNow / fRest - 1.0f;
          frame.m_uiStretchedEdge[0] = indices[i + e];
          frame.m_uiStretchedEdge[1] = indices[i + (e + 1) % 3];
        }
      }
    }

    // particles inside the body. The particles are relative to the owner, the joints are in the world.
    const plUInt32 uiShapes = plMath::Min(m_BodyShapes.GetCount(), bodyJoints.GetCount());

    plHybridArray<plTransform, 32> shapes;
    shapes.SetCount(uiShapes);

    for (plUInt32 s = 0; s < uiShapes; ++s)
    {
      shapes[s] = m_BodyShapes[s].GetShapeTransform(bodyJoints[s], m_fShapeScale);
      shapes[s].m_vPosition -= vOwnerPosition;
    }

    for (plUInt32 p = 0; p < uiParticles; ++p)
    {
      // a pinned particle is skinned: it is where the garment was modelled, which no cloth setting changes
      if (m_pDesc->m_MaxDistance[p] <= 0.0f)
        continue;

      for (plUInt32 s = 0; s < uiShapes; ++s)
      {
        // a shape that was inflated for the garment is larger than the body: the body ends that much further in
        if (GetPenetration(m_BodyShapes[s], shapes[s], m_fShapeScale, m_Positions[p]) - m_BodyShapes[s].m_fInflate * m_fShapeScale > m_Gates.m_fMaxPenetration)
        {
          if (frame.m_uiInsideBody == 0)
          {
            frame.m_uiInsideParticle = p;
            frame.m_uiInsideShape = s;
          }

          ++frame.m_uiInsideBody;
          break;
        }
      }
    }

    // cloth that whips is stretched for a few frames; cloth that is set up badly stays stretched
    m_fStretchedFor = frame.m_fStretch > m_Gates.m_fMaxStretch ? m_fStretchedFor + fTimeStep : 0.0f;

    if (m_fStretchedFor > m_Gates.m_fStretchGraceTime || frame.m_fStretch > m_Gates.m_fMaxStretchPeak)
      frame.m_uiFailedGates |= plJoltClothStressGate::Stretch;
    if (frame.m_fOvershoot > m_Gates.m_fMaxOvershoot)
      frame.m_uiFailedGates |= plJoltClothStressGate::Overshoot;
    if (frame.m_uiInsideBody > m_Gates.m_uiMaxInsideBody)
      frame.m_uiFailedGates |= plJoltClothStressGate::InsideBody;
    if (frame.m_fRelativeSpeed > m_Gates.m_fMaxRelativeSpeed)
      frame.m_uiFailedGates |= plJoltClothStressGate::Pop;
  }

  if (m_Gates.m_fMaxSolveMs > 0.0f && frame.m_fSolveMs > m_Gates.m_fMaxSolveMs)
    frame.m_uiFailedGates |= plJoltClothStressGate::Cost;

  plJoltClothStressFrame& worst = m_Result.m_Worst;
  worst.m_fStretch = plMath::Max(worst.m_fStretch, frame.m_fStretch);
  worst.m_fOvershoot = plMath::Max(worst.m_fOvershoot, frame.m_fOvershoot);
  worst.m_fRelativeSpeed = plMath::Max(worst.m_fRelativeSpeed, frame.m_fRelativeSpeed);
  worst.m_fSolveMs = plMath::Max(worst.m_fSolveMs, frame.m_fSolveMs);
  worst.m_uiInsideBody = plMath::Max(worst.m_uiInsideBody, frame.m_uiInsideBody);
  worst.m_uiFailedGates |= frame.m_uiFailedGates;

  if (frame.m_uiFailedGates != 0)
    ++m_Result.m_uiFailedFrames;

  m_Result.m_Frames.PushBack(frame);
}

plResult plJoltClothStressTest::RunScript(plJoltWorldModule& ref_module, const plJoltClothDescriptor& desc, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, const plJoltClothStressScriptOptions& options, plJoltClothStressResult& out_result)
{
  out_result = plJoltClothStressResult();

  // the character stands in its bind pose, which the skeleton's root transform turns into its object space
  const plMat4 mBindToOwner = desc.m_RootTransform.GetAsMat4();

  plJoltClothSolverSetup setup;
  PL_SUCCEED_OR_RETURN(setup.Build(desc, mBindToOwner));

  if (bodyShapes.IsEmpty())
    bodyShapes = desc.m_Colliders;

  plUInt32 uiFilterID = ref_module.CreateObjectFilterID();
  const plUInt16 uiLayer = plJoltCollisionFiltering::ConstructCharacterClothLayer(0, plJoltClothWorldCollision::None);

  plJoltClothInstance instance;
  if (instance.Initialize(ref_module, desc, setup, uiLayer, uiFilterID, setup.m_fUniformScale).Failed())
  {
    ref_module.DeleteObjectFilterID(uiFilterID);
    return PL_FAILURE;
  }

  // every bone the cloth is skinned to stays where the bind pose has it
  plUInt32 uiBones = 1;
  for (const plVec4U16& bones : desc.m_BoneIndices)
  {
    uiBones = plMath::Max<plUInt32>(uiBones, plMath::Max(plMath::Max(bones.x, bones.y), plMath::Max(bones.z, bones.w)) + 1u);
  }

  plDynamicArray<plMat4> skinTransforms;
  skinTransforms.SetCount(uiBones, plMat4::MakeIdentity());

  plDynamicArray<plTransform> colliderJoints;
  plDynamicArray<plTransform> bodyJoints;
  colliderJoints.SetCount(desc.m_Colliders.GetCount());
  bodyJoints.SetCount(bodyShapes.GetCount());

  plJoltClothStressTest test;
  test.Begin(desc, setup, bodyShapes, gates, setup.m_fUniformScale);

  const float fTimeStep = 1.0f / plMath::Max(options.m_fFrameRate, 10.0f);

  plVec3 vPosition = plVec3::MakeZero();
  float fYawDegree = 0.0f;

  auto Frame = [&]()
  {
    const plQuat qOwner = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(fYawDegree));
    const plMat4 mBindToWorld = plTransform(vPosition, qOwner).GetAsMat4() * mBindToOwner;

    // a joint in the bind pose, where the character stands now
    auto JointInWorld = [&](const plJoltClothCollider& collider) -> plTransform
    {
      plMat4 mFull;
      plQuat qRotation;
      plMsgAnimationPoseUpdated::ComputeFullBoneTransform(mBindToWorld, collider.m_JointBindPose.GetAsMat4(), mFull, qRotation);

      return plTransform(mFull.GetTranslationVector(), qRotation);
    };

    for (plUInt32 c = 0; c < colliderJoints.GetCount(); ++c)
    {
      colliderJoints[c] = JointInWorld(desc.m_Colliders[c]);
    }

    for (plUInt32 s = 0; s < bodyJoints.GetCount(); ++s)
    {
      bodyJoints[s] = JointInWorld(bodyShapes[s]);
    }

    instance.SetColliderJointTransforms(colliderJoints);

    plJoltClothInstance::StepInput input;
    input.m_vOwnerPosition = vPosition;
    input.m_qOwnerRotation = qOwner;
    input.m_SkinTransforms = skinTransforms;
    input.m_fTimeStep = fTimeStep;
    input.m_vWind = options.m_vWind;
    input.m_fLinearInertia = options.m_fLinearInertia;
    input.m_fAngularInertia = options.m_fAngularInertia;
    input.m_fGravityFactor = desc.m_Simulation.m_fGravityFactor;
    input.m_fTeleportDistance = options.m_fTeleportDistance;
    input.m_TeleportMode = options.m_TeleportMode;

    instance.Step(input);
    test.AddFrame(instance, vPosition, bodyJoints, fTimeStep);
  };

  auto Rest = [&](float fSeconds)
  {
    for (float t = 0.0f; t < fSeconds; t += fTimeStep)
    {
      Frame();
    }
  };

  // Runs along the direction the character looks in, or against it: up to speed in a quarter of a second, then a dead stop.
  auto Sprint = [&](float fSeconds, float fDirection)
  {
    for (float t = 0.0f; t < fSeconds; t += fTimeStep)
    {
      const float fSpeed = options.m_fSprintSpeed * plMath::Saturate(t / 0.25f);
      const plVec3 vForward = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(fYawDegree)) * plVec3(1, 0, 0);

      vPosition += vForward * (fDirection * fSpeed * fTimeStep);
      Frame();
    }
  };

  Rest(1.0f);

  Sprint(1.5f, 1.0f);
  Rest(0.75f);

  Sprint(1.0f, -1.0f);
  Rest(0.75f);

  for (float t = 0.0f; t < 1.5f; t += fTimeStep)
  {
    fYawDegree += options.m_fSpinSpeed * fTimeStep;
    Frame();
  }
  Rest(0.5f);

  vPosition += plVec3(options.m_fTeleportStep, 0, 0);
  Rest(0.5f);

  out_result = test.GetResult();

  instance.Deinitialize();
  ref_module.DeleteObjectFilterID(uiFilterID);

  return PL_SUCCESS;
}

plResult plJoltClothStressTest::RunScript(plWorld& ref_world, const plJoltClothDescriptor& desc, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, const plJoltClothStressScriptOptions& options, plJoltClothStressResult& out_result)
{
  return RunScript(*ref_world.GetOrCreateModule<plJoltWorldModule>(), desc, bodyShapes, gates, options, out_result);
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltClothStressTest);
