#include <EnginePluginJolt/EnginePluginJoltPCH.h>

#include <EnginePluginJolt/ClothAsset/JoltClothContext.h>
#include <EnginePluginJolt/ClothAsset/JoltClothView.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <GameEngine/Animation/Skeletal/AnimatedMeshComponent.h>
#include <GameEngine/Animation/Skeletal/SimpleAnimationComponent.h>
#include <GameEngine/Effects/Wind/SimpleWindWorldModule.h>
#include <Foundation/IO/MemoryStream.h>
#include <JoltPlugin/CharacterCloth/JoltCharacterClothComponent.h>
#include <JoltPlugin/CharacterCloth/JoltClothBuilder.h>
#include <JoltPlugin/CharacterCloth/JoltClothStressTest.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Meshes/MeshResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothContext, 1, plRTTIDefaultAllocator<plJoltClothContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Jolt_Cloth"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plJoltClothContext::plJoltClothContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::CreateWorld)
{
}

void plJoltClothContext::HandleMessage(const plEditorEngineDocumentMsg* pDocMsg)
{
  if (auto pMsg = plDynamicCast<const plQuerySelectionBBoxMsgToEngine*>(pDocMsg))
  {
    QuerySelectionBBox(pMsg);
    return;
  }

  if (auto pMsg = plDynamicCast<const plSimpleDocumentConfigMsgToEngine*>(pDocMsg))
  {
    if (pMsg->m_sWhatToDo == "CommonAssetUiState")
    {
      if (pMsg->m_sPayload == "Grid")
      {
        m_bDisplayGrid = pMsg->m_PayloadValue.ConvertTo<float>() > 0;
      }
      else if (pMsg->m_sPayload == "Restart")
      {
        // the clip starts over and the cloth settles from the pose again
        PL_LOCK(m_pWorld->GetWriteMarker());

        plSimpleAnimationComponent* pAnimation = nullptr;
        if (m_pWorld->TryGetComponent(m_hAnimationComponent, pAnimation))
          pAnimation->SetNormalizedPlaybackPosition(0.0f);

        if (plJoltCharacterClothComponent* pCloth = GetCloth())
          pCloth->Reset();
      }
    }
    else if (pMsg->m_sWhatToDo == "PreviewMesh")
    {
      if (m_sMesh != pMsg->m_sPayload)
      {
        m_sMesh = pMsg->m_sPayload;

        PL_LOCK(m_pWorld->GetWriteMarker());
        RebuildPreview();
      }
    }
    else if (pMsg->m_sWhatToDo == "PreviewClip")
    {
      if (m_sClip != pMsg->m_sPayload)
      {
        m_sClip = pMsg->m_sPayload;

        PL_LOCK(m_pWorld->GetWriteMarker());
        RebuildPreview();
      }
    }
    else if (pMsg->m_sWhatToDo == "PaintMode")
    {
      const bool bPaintMode = pMsg->m_PayloadValue.ConvertTo<bool>();

      if (m_bPaintMode != bPaintMode)
      {
        m_bPaintMode = bPaintMode;

        PL_LOCK(m_pWorld->GetWriteMarker());
        RebuildPreview();
      }
    }
    else if (pMsg->m_sWhatToDo == "ClothView")
    {
      m_iView = pMsg->m_PayloadValue.ConvertTo<plInt32>();

      PL_LOCK(m_pWorld->GetWriteMarker());

      if (plJoltCharacterClothComponent* pCloth = GetCloth())
        pCloth->SetDebugView(m_iView);
    }
    else if (pMsg->m_sWhatToDo == "PaintValues")
    {
      if (pMsg->m_PayloadValue.IsA<plDataBuffer>())
        m_PaintValues = pMsg->m_PayloadValue.Get<plDataBuffer>();
    }
    else if (pMsg->m_sWhatToDo == "PaintBrush")
    {
      double fValues[4] = {};
      const char* szText = pMsg->m_sPayload.GetData();

      m_bBrushVisible = !pMsg->m_sPayload.IsEmpty();

      for (plUInt32 i = 0; i < 4 && m_bBrushVisible; ++i)
      {
        if (plConversionUtils::StringToFloat(szText, fValues[i], &szText).Failed())
          m_bBrushVisible = false;
      }

      m_vBrushPosition.Set(static_cast<float>(fValues[0]), static_cast<float>(fValues[1]), static_cast<float>(fValues[2]));
      m_fBrushRadius = static_cast<float>(fValues[3]);
    }
    else if (pMsg->m_sWhatToDo == "Wind")
    {
      const float fWind = pMsg->m_PayloadValue.ConvertTo<float>();

      if (m_fWind != fWind)
      {
        m_fWind = fWind;

        // The character looks along +X, so wind from the front blows towards -X and takes a cape with it.
        PL_LOCK(m_pWorld->GetWriteMarker());
        m_pWorld->GetOrCreateModule<plSimpleWindWorldModule>()->SetFallbackWind(plVec3(-m_fWind, 0, 0));
      }
    }
    else if (pMsg->m_sWhatToDo == "SimulationSpeed")
    {
      // a frame of the stress test stands still, whatever the play button says
      PL_LOCK(m_pWorld->GetWriteMarker());
      const double fSpeed = m_uiStressFrame != plInvalidIndex ? 0.0 : pMsg->m_PayloadValue.ConvertTo<double>();

      // A clock at speed zero still takes its smallest step every frame, so standing still means pausing it.
      m_pWorld->GetClock().SetPaused(fSpeed <= 0.0);

      if (fSpeed > 0.0)
        m_pWorld->GetClock().SetSpeed(fSpeed);
    }
    else if (pMsg->m_sWhatToDo == "StressTest")
    {
      RunStressTest(pMsg->m_sPayload);
    }
    else if (pMsg->m_sWhatToDo == "StressFrame")
    {
      // where on the timeline, from 0 at its left end to 1 at its right end
      const float fWhere = plMath::Saturate(pMsg->m_PayloadValue.ConvertTo<float>());
      const plUInt32 uiFrames = m_StressResult.m_Frames.GetCount();

      if (m_bStressShown && uiFrames > 0)
        ShowStressFrame(plMath::Min(uiFrames - 1, static_cast<plUInt32>(fWhere * uiFrames)));

      // the window waits for this before it asks for the next frame
      plSimpleDocumentConfigMsgToEditor shown;
      shown.m_DocumentGuid = GetDocumentGuid();
      shown.m_sWhatToDo = "StressFrameShown";
      SendProcessMessage(&shown);
    }
    else if (pMsg->m_sWhatToDo == "StressLeave")
    {
      LeaveStress();
    }
    else if (pMsg->m_sWhatToDo == "DragOffset")
    {
      double fX = 0.0, fY = 0.0;
      const char* szText = pMsg->m_sPayload.GetData();

      if (plConversionUtils::StringToFloat(szText, fX, &szText).Succeeded() && plConversionUtils::StringToFloat(szText, fY, &szText).Succeeded())
        m_vDragTarget.Set(static_cast<float>(fX), static_cast<float>(fY), 0.0f);
    }
    else if (pMsg->m_sWhatToDo == "Teleport")
    {
      m_uiTeleportFrames = 2;
    }
    else if (pMsg->m_sWhatToDo == "Turn")
    {
      m_fTurnLeft = 360.0f;
    }
    else if (pMsg->m_sWhatToDo == "JointTransforms")
    {
      // the window places the handles of the asset's own colliders on these
      plStringBuilder sReply;
      GetJointTransformsText(pMsg->m_sPayload, sReply);

      plSimpleDocumentConfigMsgToEditor reply;
      reply.m_DocumentGuid = GetDocumentGuid();
      reply.m_sWhatToDo = "JointTransforms";
      reply.m_sPayload = sReply;
      SendProcessMessage(&reply);
    }

    return;
  }

  if (plDynamicCast<const plViewRedrawMsgToEngine*>(pDocMsg) != nullptr)
  {
    UpdatePreviewMotion();

    if (m_bPaintMode)
      DrawPaintOverlay();
  }

  plEngineProcessDocumentContext::HandleMessage(pDocMsg);
}

void plJoltClothContext::OnInitialize()
{
  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  plGameObjectDesc obj;
  obj.m_bDynamic = true;
  obj.m_sName.Assign("ClothPreview");
  pWorld->CreateObject(obj, m_pGameObject);

  const plTag& tagCastShadows = plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow");
  m_pGameObject->SetTag(tagCastShadows);

  plStringBuilder sClothGuid;
  plConversionUtils::ToString(GetDocumentGuid(), sClothGuid);
  m_hCloth = plResourceManager::LoadResource<plJoltClothResource>(sClothGuid);
}

void plJoltClothContext::RebuildPreview()
{
  // the paint tools work on the rest pose: no clip, no simulation
  RebuildPreview(m_bPaintMode ? plStringView() : m_sClip.GetView(), !m_bPaintMode);

  // The cloth on disk may be older than the document, or not there yet: the window answers with the current cook.
  plSimpleDocumentConfigMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();
  msg.m_sWhatToDo = "ClothPreviewReady";
  SendProcessMessage(&msg);
}

void plJoltClothContext::RebuildPreview(plStringView sClip, bool bSimulate)
{
  plWorld* pWorld = m_pWorld;

  plJoltCharacterClothComponent* pCloth = nullptr;
  if (pWorld->TryGetComponent(m_hClothComponent, pCloth))
    pCloth->DeleteComponent();

  plSimpleAnimationComponent* pAnimation = nullptr;
  if (pWorld->TryGetComponent(m_hAnimationComponent, pAnimation))
    pAnimation->DeleteComponent();

  // a new mesh component starts in the rest pose, whatever the old one last played
  plAnimatedMeshComponent* pMesh = nullptr;
  if (pWorld->TryGetComponent(m_hMeshComponent, pMesh))
    pMesh->DeleteComponent();

  m_hClothComponent.Invalidate();
  m_hAnimationComponent.Invalidate();
  m_hMeshComponent.Invalidate();

  if (m_sMesh.IsEmpty())
    return;

  m_hMeshComponent = plAnimatedMeshComponent::CreateComponent(m_pGameObject, pMesh);
  pMesh->SetMeshFile(m_sMesh);

  if (!sClip.IsEmpty())
  {
    plStringBuilder sClipFile = sClip;

    m_hAnimationComponent = plSimpleAnimationComponent::CreateComponent(m_pGameObject, pAnimation);
    pAnimation->SetAnimationClipFile(sClipFile.GetData());
  }

  m_hClothComponent = plJoltCharacterClothComponent::CreateComponent(m_pGameObject, pCloth);
  pCloth->SetCloth(m_hCloth);
  pCloth->m_Importance = plJoltClothImportance::Always;
  pCloth->SetSimulate(bSimulate, plTime::MakeZero());
  pCloth->SetDebugView(m_iView);

  pWorld->SetWorldSimulationEnabled(true);
}

void plJoltClothContext::UpdatePreviewMotion()
{
  const plTime now = plTime::Now();
  const float fTimeStep = plMath::Clamp(static_cast<float>((now - m_LastMotionUpdate).GetSeconds()), 0.0f, 0.1f);
  m_LastMotionUpdate = now;

  // a frame of the stress test stands as the test left it
  if (m_pGameObject == nullptr || m_uiStressFrame != plInvalidIndex)
    return;

  // The drag test: the character follows the cursor at once and swings back to its place when it is let go, fast
  // enough to show the cloth lag and slow enough not to count as a teleport.
  if (m_vDragTarget.IsZero())
    m_vDragPosition = plMath::Lerp(m_vDragPosition, m_vDragTarget, plMath::Min(1.0f, fTimeStep * 8.0f));
  else
    m_vDragPosition = m_vDragTarget;

  // the turn test: once around in a second
  const float fTurn = plMath::Min(m_fTurnLeft, 360.0f * fTimeStep);
  m_fTurnLeft -= fTurn;
  m_fYaw = m_fTurnLeft > 0.0f ? m_fYaw + fTurn : 0.0f;

  // Teleport: one frame far away, then back, like a respawn that returns to where the camera looks.
  plVec3 vPosition = m_vDragPosition;
  if (m_uiTeleportFrames > 0)
  {
    --m_uiTeleportFrames;

    if (m_uiTeleportFrames == 1)
      vPosition += plVec3(50.0f, 0, 0);
  }

  const plQuat qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(m_fYaw));

  if (vPosition == m_vAppliedPosition && m_fYaw == m_fAppliedYaw)
    return;

  m_vAppliedPosition = vPosition;
  m_fAppliedYaw = m_fYaw;

  PL_LOCK(m_pWorld->GetWriteMarker());
  m_pGameObject->SetGlobalPosition(vPosition);
  m_pGameObject->SetGlobalRotation(qRotation);
}

plJoltCharacterClothComponent* plJoltClothContext::GetCloth()
{
  plJoltCharacterClothComponent* pCloth = nullptr;
  m_pWorld->TryGetComponent(m_hClothComponent, pCloth);
  return pCloth;
}

void plJoltClothContext::DrawPaintOverlay()
{
  // through a const world: the lookup of a world that can be written to asks for the write lock
  const plWorld* pWorld = m_pWorld;
  PL_LOCK(pWorld->GetReadMarker());

  const plAnimatedMeshComponent* pMesh = nullptr;
  if (!pWorld->TryGetComponent(m_hMeshComponent, pMesh))
    return;

  plResourceLock<plJoltClothResource> pCloth(m_hCloth, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pCloth.GetAcquireResult() != plResourceAcquireResult::Final || pCloth->GetData() == nullptr)
    return;

  const plSharedPtr<plJoltClothData> pData = pCloth->GetData();
  const plJoltClothDescriptor& desc = pData->GetDescriptor();

  // the sim mesh in its bind pose, where the mesh stands while it is painted
  const plMat4 mBindToWorld = m_pGameObject->GetGlobalTransform().GetAsMat4() * pMesh->GetRootTransform().GetAsMat4();

  plDynamicArray<plVec3> positions;
  positions.SetCountUninitialized(desc.GetParticleCount());

  for (plUInt32 p = 0; p < positions.GetCount(); ++p)
  {
    positions[p] = mBindToWorld.TransformPosition(desc.m_Positions[p]);
  }

  // Values that do not fit the sim mesh belong to a cook the preview has not loaded yet.
  if (m_PaintValues.GetCount() == positions.GetCount())
  {
    plDynamicArray<plVec3> offsets;
    plJoltCharacterClothComponent::GetOverlayOffsets(positions, desc.m_Indices, offsets);

    plDynamicArray<plDebugRenderer::Triangle> triangles;
    triangles.Reserve(desc.m_Indices.GetCount() / 3 * 2);

    for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
    {
      const plUInt32 a = desc.m_Indices[i];
      const plUInt32 b = desc.m_Indices[i + 1];
      const plUInt32 c = desc.m_Indices[i + 2];

      const float fValue = (m_PaintValues[a] + m_PaintValues[b] + m_PaintValues[c]) / (3.0f * 255.0f);
      const plColor color = plJoltCharacterClothComponent::GetHeatColor(fValue);

      // on both sides of the cloth
      plDebugRenderer::Triangle& front = triangles.ExpandAndGetRef();
      front = plDebugRenderer::Triangle(positions[a] + offsets[a], positions[b] + offsets[b], positions[c] + offsets[c]);
      front.m_color = color;

      plDebugRenderer::Triangle& back = triangles.ExpandAndGetRef();
      back = plDebugRenderer::Triangle(positions[a] - offsets[a], positions[c] - offsets[c], positions[b] - offsets[b]);
      back.m_color = color;
    }

    plDebugRenderer::DrawSolidTriangles(m_pWorld, triangles, plColor::White);
  }

  // the edges of the sim mesh, so that the particles a stroke moves can be told apart
  {
    plDynamicArray<plDebugRenderer::Line> lines;
    lines.Reserve(desc.m_Indices.GetCount());

    for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        plDebugRenderer::Line& line = lines.ExpandAndGetRef();
        line.m_start = positions[desc.m_Indices[i + e]];
        line.m_end = positions[desc.m_Indices[i + (e + 1) % 3]];
      }
    }

    plDebugRenderer::DrawLines(m_pWorld, lines, plColor(1.0f, 1.0f, 1.0f, 0.25f));
  }

  if (m_bBrushVisible)
    plDebugRenderer::DrawLineSphere(m_pWorld, plBoundingSphere::MakeFromCenterAndRadius(m_vBrushPosition, m_fBrushRadius), plColor::White);
}

bool plJoltClothContext::GetBodyJoints(plDynamicArray<plTransform>& out_joints)
{
  const plDynamicArray<plJoltClothCollider>& shapes = m_StressBody.m_Colliders;
  out_joints.SetCount(shapes.GetCount());

  plAnimatedMeshComponent* pMesh = nullptr;
  if (!m_pWorld->TryGetComponent(m_hMeshComponent, pMesh) || !pMesh->GetMesh().IsValid())
    return false;

  const plArrayPtr<const plShaderTransform> skinning = pMesh->GetSkinningTransforms();
  if (skinning.IsEmpty())
    return false;

  plResourceLock<plMeshResource> pMeshResource(pMesh->GetMesh(), plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pMeshResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  const plMat4 mSkinSpaceToWorld = m_pGameObject->GetGlobalTransform().GetAsMat4() * pMesh->GetRootTransform().GetAsMat4();

  for (plUInt32 s = 0; s < shapes.GetCount(); ++s)
  {
    // a joint the mesh is not skinned to stays where the bind pose has it
    plMat4 mJoint = shapes[s].m_JointBindPose.GetAsMat4();

    plMeshResourceDescriptor::BoneData bone;
    if (pMeshResource->m_Bones.TryGetValue(shapes[s].m_sJoint, bone) && bone.m_uiBoneIndex < skinning.GetCount())
    {
      // skinning matrix = joint pose * inverse bind pose
      mJoint = skinning[bone.m_uiBoneIndex].GetAsMat4() * bone.m_GlobalInverseRestPoseMatrix.GetInverse();
    }

    plMat4 mFull;
    plQuat qRotation;
    plMsgAnimationPoseUpdated::ComputeFullBoneTransform(mSkinSpaceToWorld, mJoint, mFull, qRotation);

    out_joints[s] = plTransform(mFull.GetTranslationVector(), qRotation);
  }

  return true;
}

void plJoltClothContext::GetJointTransformsText(plStringView sJoints, plStringBuilder& out_sText)
{
  out_sText.Clear();

  PL_LOCK(m_pWorld->GetWriteMarker());

  plAnimatedMeshComponent* pMesh = nullptr;
  if (m_pGameObject == nullptr || !m_pWorld->TryGetComponent(m_hMeshComponent, pMesh) || !pMesh->GetMesh().IsValid())
    return;

  const plArrayPtr<const plShaderTransform> skinning = pMesh->GetSkinningTransforms();
  if (skinning.IsEmpty())
    return;

  plResourceLock<plMeshResource> pMeshResource(pMesh->GetMesh(), plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pMeshResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  const plTransform owner = m_pGameObject->GetGlobalTransform();
  const plMat4 mSkinSpaceToWorld = owner.GetAsMat4() * pMesh->GetRootTransform().GetAsMat4();

  plHybridArray<plStringView, 16> joints;
  sJoints.Split(false, joints, ";");

  for (plStringView sJoint : joints)
  {
    // a joint the mesh is not skinned to has no pose here: its handle sits at the character's feet
    plTransform joint = owner;

    plHashedString sName;
    sName.Assign(sJoint);

    plMeshResourceDescriptor::BoneData bone;
    if (pMeshResource->m_Bones.TryGetValue(sName, bone) && bone.m_uiBoneIndex < skinning.GetCount())
    {
      // skinning matrix = joint pose * inverse bind pose
      const plMat4 mJoint = skinning[bone.m_uiBoneIndex].GetAsMat4() * bone.m_GlobalInverseRestPoseMatrix.GetInverse();

      plMat4 mFull;
      plQuat qRotation;
      plMsgAnimationPoseUpdated::ComputeFullBoneTransform(mSkinSpaceToWorld, mJoint, mFull, qRotation);

      joint = plTransform(mFull.GetTranslationVector(), qRotation);
    }

    out_sText.AppendFormat("{}|{} {} {} {} {} {} {};", sJoint, plArgF(joint.m_vPosition.x, 5), plArgF(joint.m_vPosition.y, 5), plArgF(joint.m_vPosition.z, 5), plArgF(joint.m_qRotation.x, 6), plArgF(joint.m_qRotation.y, 6), plArgF(joint.m_qRotation.z, 6), plArgF(joint.m_qRotation.w, 6));
  }
}

void plJoltClothContext::SimulateStress(plUInt32 uiStopAtFrame, plJoltClothStressTest* pTest, plDynamicArray<StressPart>* out_pParts)
{
  plWorld* pWorld = m_pWorld;

  const plJoltClothStressScriptOptions options;
  const float fTimeStep = 1.0f / options.m_fFrameRate;

  // every frame is one step of the world at a fixed length, so that the same frame shows the same cloth every time
  pWorld->GetClock().SetPaused(false);
  pWorld->GetClock().SetSpeed(1.0);
  pWorld->GetClock().SetFixedTimeStep(plTime::MakeFromSeconds(fTimeStep));
  PL_SCOPE_EXIT(pWorld->GetClock().SetFixedTimeStep());

  plGameObject* pObject = m_pGameObject;
  plVec3 vPosition = plVec3::MakeZero();
  float fYaw = 0.0f;

  plUInt32 uiFrame = 0;
  plUInt32 uiFramesBeforeBegin = 0;
  bool bBegun = false;
  bool bStop = false;

  plDynamicArray<plTransform> bodyJoints;

  // bMeasure is off for the frames in which the cloth settles into the first pose of a clip
  auto Frame = [&](bool bMeasure = true)
  {
    pObject->SetGlobalPosition(vPosition);
    pObject->SetGlobalRotation(plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(fYaw)));

    pWorld->Update();

    if (pTest != nullptr)
    {
      const plJoltCharacterClothComponent* pCloth = GetCloth();

      if (bMeasure && pCloth != nullptr && pCloth->GetInstance().IsInitialized() && pCloth->GetSolverSetup() != nullptr && GetBodyJoints(bodyJoints))
      {
        if (!bBegun)
        {
          bBegun = true;

          // an editor build is not optimized, so its solve time says nothing: it is reported, not gated
          plJoltClothStressGates gates;
          gates.m_fMaxSolveMs = 0.0f;

          pTest->Begin(pCloth->GetClothData()->GetDescriptor(), *pCloth->GetSolverSetup(), m_StressBody.m_Colliders, gates, pCloth->GetSolverSetup()->m_fUniformScale);

          // the frames before the cloth had a pose are counted, so that the timeline has the frames the test played
          for (plUInt32 i = 0; i < uiFramesBeforeBegin; ++i)
          {
            pTest->AddEmptyFrame(fTimeStep);
          }
        }

        pTest->AddFrame(pCloth->GetInstance(), vPosition, bodyJoints, fTimeStep);
      }
      else if (bBegun)
      {
        pTest->AddEmptyFrame(fTimeStep);
      }
      else
      {
        ++uiFramesBeforeBegin;
      }
    }

    if (uiFrame == uiStopAtFrame)
      bStop = true;

    ++uiFrame;
  };

  auto BeginPart = [&](plStringView sName)
  {
    if (out_pParts != nullptr)
    {
      StressPart& part = out_pParts->ExpandAndGetRef();
      part.m_sName = sName;
      part.m_uiFirstFrame = uiFrame;
    }
  };

  auto EndPart = [&]()
  {
    if (out_pParts != nullptr)
      out_pParts->PeekBack().m_uiNumFrames = uiFrame - out_pParts->PeekBack().m_uiFirstFrame;
  };

  // The motion script, on the bind pose: stand, sprint, hard stop, sprint backwards, hard stop, spin, teleport.
  RebuildPreview(plStringView(), true);

  auto Rest = [&](plStringView sName, float fSeconds)
  {
    BeginPart(sName);
    for (float t = 0.0f; t < fSeconds && !bStop; t += fTimeStep)
    {
      Frame();
    }
    EndPart();
  };

  // along the direction the character looks in, or against it: up to speed in a quarter of a second, then a dead stop
  auto Sprint = [&](plStringView sName, float fSeconds, float fDirection)
  {
    BeginPart(sName);
    for (float t = 0.0f; t < fSeconds && !bStop; t += fTimeStep)
    {
      const float fSpeed = options.m_fSprintSpeed * plMath::Saturate(t / 0.25f);
      const plVec3 vForward = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(fYaw)) * plVec3(1, 0, 0);

      vPosition += vForward * (fDirection * fSpeed * fTimeStep);
      Frame();
    }
    EndPart();
  };

  Rest("stand", 1.0f);
  Sprint("sprint", 1.5f, 1.0f);
  Rest("hard stop", 0.75f);
  Sprint("sprint back", 1.0f, -1.0f);
  Rest("hard stop", 0.75f);

  BeginPart("spin");
  for (float t = 0.0f; t < 1.5f && !bStop; t += fTimeStep)
  {
    fYaw += options.m_fSpinSpeed * fTimeStep;
    Frame();
  }
  EndPart();

  Rest("settle", 0.5f);

  if (!bStop)
    vPosition += plVec3(options.m_fTeleportStep, 0, 0);
  Rest("teleport", 0.5f);

  // Every clip once through, on the spot.
  for (plUInt32 c = 0; c < m_StressClips.GetCount() && !bStop; ++c)
  {
    vPosition.SetZero();
    fYaw = 0.0f;
    pObject->SetGlobalPosition(vPosition);
    pObject->SetGlobalRotation(plQuat::MakeIdentity());

    RebuildPreview(m_StressClips[c], true);

    BeginPart(m_StressClipNames[c]);

    // Until the clip starts over, and never longer than eight seconds. The character jumps from its bind pose into
    // the clip, which no game does: the cloth gets a quarter of a second to settle before it is measured.
    float fLastPosition = 0.0f;
    for (plUInt32 uiClipFrame = 0; uiClipFrame < 480 && !bStop; ++uiClipFrame)
    {
      Frame(uiClipFrame >= 15);

      plSimpleAnimationComponent* pAnimation = nullptr;
      if (!pWorld->TryGetComponent(m_hAnimationComponent, pAnimation))
        break;

      const float fClipPosition = pAnimation->GetNormalizedPlaybackPosition();
      if (uiClipFrame > 10 && fClipPosition < fLastPosition)
        break;

      fLastPosition = fClipPosition;
    }

    EndPart();
  }
}

void plJoltClothContext::ShowStressFrame(plUInt32 uiFrame)
{
  PL_LOCK(m_pWorld->GetWriteMarker());

  // the test again, up to the frame: the same steps give the same cloth
  SimulateStress(uiFrame, nullptr, nullptr);

  m_uiStressFrame = uiFrame;

  // The character stays turned as it was, and comes back to where the camera looks. The cloth is kept relative to
  // the character, so it comes along as it is.
  m_pGameObject->SetGlobalPosition(plVec3::MakeZero());
  m_pWorld->GetClock().SetPaused(true);

  m_vDragTarget.SetZero();
  m_vDragPosition.SetZero();
  m_vAppliedPosition.SetZero();
  m_fTurnLeft = 0.0f;
  m_uiTeleportFrames = 0;
}

void plJoltClothContext::LeaveStress()
{
  if (!m_bStressShown)
    return;

  m_bStressShown = false;
  m_uiStressFrame = plInvalidIndex;

  PL_LOCK(m_pWorld->GetWriteMarker());

  m_pGameObject->SetGlobalPosition(plVec3::MakeZero());
  m_pGameObject->SetGlobalRotation(plQuat::MakeIdentity());
  m_fYaw = 0.0f;
  m_fAppliedYaw = 0.0f;

  RebuildPreview();
}

void plJoltClothContext::DrawStressTimeline(const plViewHandle& hView, float fWidth, float fHeight)
{
  const plUInt32 uiFrames = m_StressResult.m_Frames.GetCount();
  if (!m_bStressShown || uiFrames == 0 || fWidth < 50.0f)
    return;

  // a strip along the top of the view, one percent in from its sides
  const float fLeft = fWidth * 0.01f;
  const float fStripWidth = fWidth * 0.98f;
  const float fTop = fHeight * 0.02f;
  const float fStripHeight = fHeight * 0.045f;

  auto FrameColor = [](plUInt8 uiFailed) -> plColor
  {
    if (uiFailed & (plJoltClothStressGate::NotFinite | plJoltClothStressGate::InsideBody))
      return plColor(0.90f, 0.22f, 0.24f);
    if (uiFailed & plJoltClothStressGate::Pop)
      return plColor(0.70f, 0.40f, 0.95f);
    if (uiFailed & plJoltClothStressGate::Stretch)
      return plColor(0.95f, 0.66f, 0.20f);
    if (uiFailed & plJoltClothStressGate::Overshoot)
      return plColor(0.30f, 0.55f, 0.95f);

    return plColor(0.20f, 0.55f, 0.30f);
  };

  plDebugRenderer::Draw2DRectangle(hView, plRectFloat(fLeft - 2.0f, fTop - 2.0f, fStripWidth + 4.0f, fStripHeight + 4.0f), 0.0f, plColor(0.05f, 0.05f, 0.06f, 0.85f));

  // frames that failed the same gates in a row are one rectangle
  for (plUInt32 f = 0; f < uiFrames;)
  {
    plUInt32 uiEnd = f + 1;
    while (uiEnd < uiFrames && m_StressResult.m_Frames[uiEnd].m_uiFailedGates == m_StressResult.m_Frames[f].m_uiFailedGates)
      ++uiEnd;

    const float x0 = fLeft + fStripWidth * f / uiFrames;
    const float x1 = fLeft + fStripWidth * uiEnd / uiFrames;

    plDebugRenderer::Draw2DRectangle(hView, plRectFloat(x0, fTop, x1 - x0, fStripHeight), 0.0f, FrameColor(m_StressResult.m_Frames[f].m_uiFailedGates));
    f = uiEnd;
  }

  // the parts of the test, named below the strip
  const plUInt32 uiTextSize = static_cast<plUInt32>(plMath::Clamp(fHeight * 0.022f, 10.0f, 24.0f));

  for (const StressPart& part : m_StressParts)
  {
    const float x = fLeft + fStripWidth * part.m_uiFirstFrame / uiFrames;

    plDebugRenderer::Draw2DRectangle(hView, plRectFloat(x, fTop - 2.0f, 1.5f, fStripHeight + 4.0f), 0.0f, plColor(0.05f, 0.05f, 0.06f));
    plDebugRenderer::Draw2DText(hView, part.m_sName.GetData(), plVec2I32(static_cast<plInt32>(x + 3.0f), static_cast<plInt32>(fTop + fStripHeight + 5.0f)), plColor(0.85f, 0.85f, 0.88f), uiTextSize);
  }

  // the frame the preview stands still in, and what was measured in it
  if (m_uiStressFrame < uiFrames)
  {
    const plJoltClothStressFrame& frame = m_StressResult.m_Frames[m_uiStressFrame];
    const float x = fLeft + fStripWidth * (m_uiStressFrame + 0.5f) / uiFrames;

    plDebugRenderer::Draw2DRectangle(hView, plRectFloat(x - 1.5f, fTop - 5.0f, 3.0f, fStripHeight + 10.0f), 0.0f, plColor::White);

    plStringBuilder sFrame;
    sFrame.SetFormat("{} s   stretch {} %   inside the body {}   speed against the body {} m/s", plArgF(frame.m_fTime, 2), plArgF(frame.m_fStretch * 100.0f, 1), frame.m_uiInsideBody, plArgF(frame.m_fRelativeSpeed, 1));

    plDebugRenderer::Draw2DText(hView, sFrame.GetData(), plVec2I32(static_cast<plInt32>(fLeft), static_cast<plInt32>(fTop + fStripHeight + 8.0f + uiTextSize)), plColor::White, uiTextSize);
  }
}

void plJoltClothContext::RunStressTest(plStringView sClips)
{
  plSimpleDocumentConfigMsgToEditor reply;
  reply.m_DocumentGuid = GetDocumentGuid();
  reply.m_sWhatToDo = "StressResult";

  auto Fail = [&](plStringView sWhy)
  {
    reply.m_sPayload = sWhy;
    SendProcessMessage(&reply);
  };

  plSharedPtr<plJoltClothData> pData;

  {
    plResourceLock<plJoltClothResource> pCloth(m_hCloth, plResourceAcquireMode::BlockTillLoaded_NeverFail);

    if (pCloth.GetAcquireResult() == plResourceAcquireResult::Final)
      pData = pCloth->GetData();
  }

  if (pData == nullptr)
  {
    Fail("The stress test needs a cloth: set the Mesh and wait for the preview to show its garments.");
    return;
  }

  const plJoltClothDescriptor& desc = pData->GetDescriptor();

  PL_LOCK(m_pWorld->GetWriteMarker());

  // The cloth is checked against every shape of the body, also the ones it does not collide with: that is how a
  // collider that is missing shows.
  plJoltClothDescriptor body;
  body.m_Fabric = desc.m_Fabric;

  const plAnimatedMeshComponent* pMesh = nullptr;
  if (m_pWorld->TryGetComponent(m_hMeshComponent, pMesh) && pMesh->GetMesh().IsValid())
  {
    plResourceLock<plMeshResource> pMeshResource(pMesh->GetMesh(), plResourceAcquireMode::BlockTillLoaded_NeverFail);

    if (pMeshResource.GetAcquireResult() == plResourceAcquireResult::Final && pMeshResource->m_hDefaultSkeleton.IsValid())
    {
      plResourceLock<plSkeletonResource> pSkeleton(pMeshResource->m_hDefaultSkeleton, plResourceAcquireMode::BlockTillLoaded_NeverFail);

      if (pSkeleton.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        plJoltClothSkeletonColliderOptions all;
        all.m_bAllShapes = true;

        plJoltClothBuilder::AddSkeletonColliders(pSkeleton->GetDescriptor(), all, body);
      }
    }
  }

  m_StressBody = body;

  // the clips to play after the motion script: "clip|name" entries
  m_StressClips.Clear();
  m_StressClipNames.Clear();

  plHybridArray<plStringView, 8> entries;
  sClips.Split(false, entries, ";");

  for (plStringView sEntry : entries)
  {
    const char* szName = sEntry.FindSubString("|");

    m_StressClips.PushBack(szName != nullptr ? plStringView(sEntry.GetStartPointer(), szName) : sEntry);
    m_StressClipNames.PushBack(szName != nullptr ? plStringView(szName + 1, sEntry.GetEndPointer()) : plStringView("clip"));
  }

  const plJoltClothStressGates gates;

  plJoltClothStressTest test;
  m_StressParts.Clear();
  SimulateStress(plInvalidIndex, &test, &m_StressParts);

  m_StressResult = test.GetResult();
  const plJoltClothStressResult& result = m_StressResult;

  // the preview plays again, with the timeline over it
  m_uiStressFrame = plInvalidIndex;
  m_bStressShown = !result.m_Frames.IsEmpty();

  m_pGameObject->SetGlobalPosition(plVec3::MakeZero());
  m_pGameObject->SetGlobalRotation(plQuat::MakeIdentity());
  RebuildPreview();

  if (result.m_Frames.IsEmpty())
  {
    Fail("The stress test could not simulate the cloth. The log has the details.");
    return;
  }

  const plJoltClothStressFrame& worst = result.m_Worst;

  auto Verdict = [&](plJoltClothStressGate::Enum gate, plStringBuilder& ref_sText)
  {
    const plUInt32 uiFrame = result.FindFirstFailure(gate);

    if (uiFrame == plInvalidIndex)
      ref_sText.Append("  ok\n");
    else
      ref_sText.AppendFormat("  FAILS, first at {} s\n", plArgF(result.m_Frames[uiFrame].m_fTime, 2));
  };

  plStringBuilder sText;
  sText.AppendFormat("{} over {} frames: stand, sprint, hard stop, sprint backwards, spin, teleport", result.Passed() ? "Passed" : "FAILED", result.m_Frames.GetCount());

  for (const plString& sClip : m_StressClipNames)
  {
    sText.Append(", ", sClip);
  }
  sText.Append(".\n\n");

  // the parts that failed, by name
  for (const StressPart& part : m_StressParts)
  {
    plUInt32 uiFailed = 0;
    for (plUInt32 f = part.m_uiFirstFrame; f < part.m_uiFirstFrame + part.m_uiNumFrames && f < result.m_Frames.GetCount(); ++f)
    {
      if (result.m_Frames[f].m_uiFailedGates != 0)
        ++uiFailed;
    }

    if (uiFailed > 0)
      sText.AppendFormat("{}: {} of {} frames fail\n", part.m_sName, uiFailed, part.m_uiNumFrames);
  }

  if (!result.Passed())
    sText.Append("\n");

  sText.AppendFormat("Stretch: {} % (gate {} %)", plArgF(worst.m_fStretch * 100.0f, 1), plArgF(gates.m_fMaxStretch * 100.0f, 0));
  Verdict(plJoltClothStressGate::Stretch, sText);

  sText.AppendFormat("Inside the body: {} particles of {}, against {} body shapes", worst.m_uiInsideBody, desc.GetParticleCount(), body.m_Colliders.GetCount());
  Verdict(plJoltClothStressGate::InsideBody, sText);

  sText.AppendFormat("Speed against the body: {} m/s (gate {} m/s)", plArgF(worst.m_fRelativeSpeed, 1), plArgF(gates.m_fMaxRelativeSpeed, 0));
  Verdict(plJoltClothStressGate::Pop, sText);

  sText.AppendFormat("Beyond the max distance: {} mm", plArgF(worst.m_fOvershoot * 1000.0f, 1));
  Verdict(plJoltClothStressGate::Overshoot, sText);

  sText.AppendFormat("Solve: {} ms in this editor build\n", plArgF(worst.m_fSolveMs, 2));

  plDynamicArray<plString> suggestions;
  result.GetSuggestions(suggestions);

  for (const plString& sSuggestion : suggestions)
  {
    sText.Append("\n", sSuggestion, "\n");
  }

  sText.Append("\nThe timeline along the top of the view has every frame. Click or drag on it to see the cloth in that frame; press Stress Test again to go back to the preview.\n");

  // the worst values travel along, for the baseline that the window keeps next to the asset
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  result.SerializeBaseline(writer).IgnoreResult();

  reply.m_sPayload = sText;
  reply.m_PayloadValue = plDataBuffer(plArrayPtr<const plUInt8>(storage.GetData(), storage.GetStorageSize32()));
  SendProcessMessage(&reply);
}

void plJoltClothContext::GetStatsText(plStringBuilder& out_sText)
{
  out_sText.Clear();

  const plWorld* pWorld = m_pWorld;
  PL_LOCK(pWorld->GetReadMarker());

  const plJoltCharacterClothComponent* pCloth = nullptr;
  if (!pWorld->TryGetComponent(m_hClothComponent, pCloth))
    return;

  const plSharedPtr<plJoltClothData> pData = pCloth->GetClothData();
  if (pData == nullptr)
  {
    out_sText = "The cloth is not transformed yet.";
    return;
  }

  const plJoltClothDescriptor& desc = pData->GetDescriptor();

  plUInt32 uiBound = 0;
  for (const plClothVertexBinding& binding : desc.m_Bindings)
  {
    if (binding.m_uiParticle0 != plInvalidIndex)
      ++uiBound;
  }

  out_sText.AppendFormat("Particles: \t{}\n", desc.GetParticleCount());
  out_sText.AppendFormat("Triangles: \t{}\n", desc.GetTriangleCount());
  out_sText.AppendFormat("Mesh vertices: \t{} of {}\n", uiBound, desc.m_Bindings.GetCount());
  out_sText.AppendFormat("Colliders: \t{}\n", desc.m_Colliders.GetCount());
  out_sText.AppendFormat("Solve: \t{} ms", plArgF(pCloth->GetLastStepDuration().GetMilliseconds(), 2));
}

plEngineProcessViewContext* plJoltClothContext::CreateViewContext()
{
  return PL_DEFAULT_NEW(plJoltClothViewContext, this);
}

void plJoltClothContext::DestroyViewContext(plEngineProcessViewContext* pContext)
{
  PL_DEFAULT_DELETE(pContext);
}

bool plJoltClothContext::UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext)
{
  // Without a window nobody has said which mesh to show. The cloth knows the mesh it was made for.
  if (m_hMeshComponent.IsInvalidated())
  {
    plString sMesh;

    {
      plResourceLock<plJoltClothResource> pCloth(m_hCloth, plResourceAcquireMode::BlockTillLoaded_NeverFail);

      if (pCloth.GetAcquireResult() == plResourceAcquireResult::Final && pCloth->GetData() != nullptr)
        sMesh = pCloth->GetData()->GetDescriptor().m_sMesh;
    }

    if (!sMesh.IsEmpty())
    {
      m_sMesh = sMesh;

      PL_LOCK(m_pWorld->GetWriteMarker());
      RebuildPreview();

      // a thumbnail is taken of a world that stands still, and bounds are only handed out for one
      m_pWorld->SetWorldSimulationEnabled(false);
    }
  }

  plBoundingBoxSphere bounds = GetWorldBounds(m_pWorld);

  // The mesh takes a few frames to load and to report its bounds. A mesh that never does is given up on.
  if (!bounds.IsValid() && !m_hMeshComponent.IsInvalidated() && m_uiThumbnailWaitFrames < 300)
  {
    ++m_uiThumbnailWaitFrames;
    return false;
  }

  m_uiThumbnailWaitFrames = 0;

  plJoltClothViewContext* pViewContext = static_cast<plJoltClothViewContext*>(pThumbnailViewContext);
  return pViewContext->UpdateThumbnailCamera(bounds);
}

void plJoltClothContext::QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg)
{
  if (m_pGameObject == nullptr)
    return;

  plBoundingBoxSphere bounds = plBoundingBoxSphere::MakeInvalid();

  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    m_pGameObject->UpdateLocalBounds();
    m_pGameObject->UpdateGlobalTransformAndBounds();
    const auto& b = m_pGameObject->GetGlobalBounds();

    if (b.IsValid())
      bounds.ExpandToInclude(b);
  }

  const plQuerySelectionBBoxMsgToEngine* msg = static_cast<const plQuerySelectionBBoxMsgToEngine*>(pMsg);

  plQuerySelectionBBoxResultMsgToEditor res;
  res.m_uiViewID = msg->m_uiViewID;
  res.m_iPurpose = msg->m_iPurpose;
  res.m_vCenter = bounds.m_vCenter;
  res.m_vHalfExtents = bounds.m_vBoxHalfExtends;
  res.m_DocumentGuid = pMsg->m_DocumentGuid;

  SendProcessMessage(&res);
}

PL_STATICLINK_FILE(EnginePluginJolt, EnginePluginJolt_ClothAsset_JoltClothContext);
