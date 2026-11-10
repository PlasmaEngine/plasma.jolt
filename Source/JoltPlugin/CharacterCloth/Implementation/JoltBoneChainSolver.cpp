#include <JoltPlugin/JoltPluginPCH.h>

#include <JoltPlugin/CharacterCloth/JoltBoneChainSolver.h>

namespace
{
  constexpr float s_fSubStepRate = 120.0f;
  constexpr plUInt32 s_uiMaxSubSteps = 4;
  constexpr plUInt32 s_uiIterations = 2;
  constexpr float s_fSingleBoneLength = 0.1f; // metres a chain of one joint is taken to reach
} // namespace

void plJoltBoneChainSolver::SetChain(plArrayPtr<const plUInt16> joints, plArrayPtr<const Follower> followers)
{
  m_Joints = joints;
  m_Followers = followers;
  m_bHasState = false;
}

void plJoltBoneChainSolver::Step(const plJoltBoneChainSettings& settings, const StepInput& input, plArrayPtr<plMat4> ref_modelPose)
{
  const plUInt32 uiJoints = m_Joints.GetCount();
  if (uiJoints == 0 || input.m_fTimeStep <= 0.0f)
    return;

  for (plUInt16 uiJoint : m_Joints)
  {
    if (uiJoint >= ref_modelPose.GetCount())
      return;
  }

  // One point per joint, and one for the end of the last bone, so that the last joint can turn as well.
  const plUInt32 uiPoints = uiJoints + 1;
  m_Animated.SetCountUninitialized(uiPoints);

  for (plUInt32 i = 0; i < uiJoints; ++i)
  {
    m_Animated[i] = input.m_mModelToWorld.TransformPosition(ref_modelPose[m_Joints[i]].GetTranslationVector());
  }

  if (uiJoints >= 2)
  {
    // the last bone is taken to be as long as the one before it, and to go on in its direction
    m_Animated[uiJoints] = m_Animated[uiJoints - 1] + (m_Animated[uiJoints - 1] - m_Animated[uiJoints - 2]);
  }
  else
  {
    plVec3 vAlongBone = input.m_mModelToWorld.TransformDirection(ref_modelPose[m_Joints[0]].TransformDirection(plVec3(1, 0, 0)));
    vAlongBone.NormalizeIfNotZero(plVec3(0, 0, -1)).IgnoreResult();

    m_Animated[1] = m_Animated[0] + vAlongBone * s_fSingleBoneLength;
  }

  const plVec3 vAnchor = m_Animated[0];
  const float fInertia = plMath::Saturate(settings.m_fLinearInertia);

  // how far the chain has to travel by its own momentum in this step to keep up with the character
  plVec3 vKeepUp = plVec3::MakeZero();

  if (!m_bHasState || input.m_bReset || m_Points.GetCount() != uiPoints || (vAnchor - m_vLastAnchor).GetLength() > settings.m_fTeleportDistance)
  {
    m_Points = m_Animated;
    m_PreviousPoints = m_Animated;
  }
  else
  {
    // The chain is carried along for the share of the character's movement that inertia does not leave it behind by.
    const plVec3 vMoved = vAnchor - m_vLastAnchor;
    const plVec3 vCarried = vMoved * (1.0f - fInertia);

    for (plUInt32 i = 1; i < uiPoints; ++i)
    {
      m_Points[i] += vCarried;
      m_PreviousPoints[i] += vCarried;
    }

    vKeepUp = vMoved * fInertia;
  }

  m_vLastAnchor = vAnchor;
  m_bHasState = true;

  // the body's shapes where the pose has them
  const plUInt32 uiShapes = plMath::Min(input.m_Shapes.GetCount(), input.m_ShapeJoints.GetCount());

  plHybridArray<plTransform, 32> shapes;
  shapes.SetCount(uiShapes);

  for (plUInt32 s = 0; s < uiShapes; ++s)
  {
    shapes[s] = input.m_Shapes[s].GetShapeTransform(input.m_ShapeJoints[s], input.m_fShapeScale);
  }

  const plUInt32 uiSubSteps = plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(input.m_fTimeStep * s_fSubStepRate)), 1, s_uiMaxSubSteps);
  const float fStep = input.m_fTimeStep / static_cast<float>(uiSubSteps);

  const plVec3 vKeepUpPerStep = vKeepUp / static_cast<float>(uiSubSteps);
  const float fKeptVelocity = plMath::Exp(-plMath::Max(0.0f, settings.m_fDamping) * 8.0f * fStep);
  const float fPull = 1.0f - plMath::Pow(1.0f - plMath::Saturate(settings.m_fStiffness), fStep * 30.0f);
  const float fDrag = plMath::Max(0.0f, settings.m_fDrag) * 2.0f;
  const plVec3 vGravity = input.m_vGravity * settings.m_fGravityFactor;
  const plAngle maxAngle = plMath::Clamp(settings.m_MaxAngle, plAngle::MakeFromDegree(0), plAngle::MakeFromDegree(180));

  for (plUInt32 uiSubStep = 0; uiSubStep < uiSubSteps; ++uiSubStep)
  {
    m_Points[0] = vAnchor;
    m_PreviousPoints[0] = vAnchor;

    for (plUInt32 i = 1; i < uiPoints; ++i)
    {
      const plVec3 vMoved = m_Points[i] - m_PreviousPoints[i];

      // damping takes away the swinging, not the speed the chain needs to keep up with its character
      const plVec3 vDamped = vKeepUpPerStep + (vMoved - vKeepUpPerStep) * fKeptVelocity;

      // air that moves against the point pushes it along
      const plVec3 vAcceleration = vGravity + (input.m_vWind - vMoved / fStep) * fDrag;

      m_PreviousPoints[i] = m_Points[i];
      m_Points[i] += vDamped + vAcceleration * (fStep * fStep);
    }

    for (plUInt32 uiIteration = 0; uiIteration < s_uiIterations; ++uiIteration)
    {
      for (plUInt32 i = 1; i < uiPoints; ++i)
      {
        const plVec3 vAnimatedBone = m_Animated[i] - m_Animated[i - 1];
        const float fLength = vAnimatedBone.GetLength();
        if (fLength < 1.0e-6f)
        {
          m_Points[i] = m_Points[i - 1];
          continue;
        }

        const plVec3 vAnimatedDirection = vAnimatedBone / fLength;

        // back towards the animated shape, carried by the simulated parent
        m_Points[i] = plMath::Lerp(m_Points[i], m_Points[i - 1] + vAnimatedBone, fPull);

        // the bone keeps its length
        plVec3 vDirection = m_Points[i] - m_Points[i - 1];
        if (vDirection.NormalizeIfNotZero(vAnimatedDirection).Failed())
          vDirection = vAnimatedDirection;

        // and does not bend further from its animated direction than it may
        const plAngle angle = plMath::ACos(plMath::Clamp(vDirection.Dot(vAnimatedDirection), -1.0f, 1.0f));
        if (angle > maxAngle)
        {
          plVec3 vAxis = vAnimatedDirection.CrossRH(vDirection);
          if (vAxis.NormalizeIfNotZero(plVec3::MakeZero()).Succeeded())
            vDirection = plQuat::MakeFromAxisAndAngle(vAxis, maxAngle) * vAnimatedDirection;
          else
            vDirection = vAnimatedDirection;
        }

        m_Points[i] = m_Points[i - 1] + vDirection * fLength;

        for (plUInt32 s = 0; s < uiShapes; ++s)
        {
          input.m_Shapes[s].PushOut(shapes[s], input.m_fShapeScale, settings.m_fRadius, m_Points[i]);
        }
      }
    }
  }

  // Back to the pose. Every joint of the chain moves to its point and turns the way its bone turned; the joints
  // that hang off the chain are carried by the joint they hang from.
  const plMat4 mWorldToModel = input.m_mModelToWorld.GetInverse();

  plHybridArray<plMat4, 16> carried;
  carried.SetCount(uiJoints);

  plVec3 vPoint = mWorldToModel.TransformPosition(m_Points[0]);
  plVec3 vAnimated = mWorldToModel.TransformPosition(m_Animated[0]);

  for (plUInt32 i = 0; i < uiJoints; ++i)
  {
    const plVec3 vNextPoint = mWorldToModel.TransformPosition(m_Points[i + 1]);
    const plVec3 vNextAnimated = mWorldToModel.TransformPosition(m_Animated[i + 1]);

    plVec3 vFrom = vNextAnimated - vAnimated;
    plVec3 vTo = vNextPoint - vPoint;

    plQuat qTurn = plQuat::MakeIdentity();
    if (vFrom.NormalizeIfNotZero(plVec3::MakeZero()).Succeeded() && vTo.NormalizeIfNotZero(plVec3::MakeZero()).Succeeded())
      qTurn = plQuat::MakeShortestRotation(vFrom, vTo);

    plMat4& mJoint = ref_modelPose[m_Joints[i]];
    const plMat4 mBefore = mJoint;

    mJoint.SetRotationalPart(qTurn.GetAsMat3() * mBefore.GetRotationalPart());
    mJoint.SetTranslationVector(vPoint);

    carried[i] = mJoint * mBefore.GetInverse();

    vPoint = vNextPoint;
    vAnimated = vNextAnimated;
  }

  for (const Follower& follower : m_Followers)
  {
    if (follower.m_uiJoint < ref_modelPose.GetCount() && follower.m_uiChainIndex < uiJoints)
      ref_modelPose[follower.m_uiJoint] = carried[follower.m_uiChainIndex] * ref_modelPose[follower.m_uiJoint];
  }
}

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_CharacterCloth_Implementation_JoltBoneChainSolver);
