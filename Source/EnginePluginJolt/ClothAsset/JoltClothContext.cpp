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
      PL_LOCK(m_pWorld->GetWriteMarker());
      m_pWorld->GetClock().SetSpeed(pMsg->m_PayloadValue.ConvertTo<double>());
    }
    else if (pMsg->m_sWhatToDo == "StressTest")
    {
      RunStressTest();
    }

    return;
  }

  if (plDynamicCast<const plViewRedrawMsgToEngine*>(pDocMsg) != nullptr && m_bPaintMode)
  {
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

  // the paint tools work on the rest pose: no clip, no simulation
  if (!m_bPaintMode && !m_sClip.IsEmpty())
  {
    m_hAnimationComponent = plSimpleAnimationComponent::CreateComponent(m_pGameObject, pAnimation);
    pAnimation->SetAnimationClipFile(m_sClip);
  }

  m_hClothComponent = plJoltCharacterClothComponent::CreateComponent(m_pGameObject, pCloth);
  pCloth->SetCloth(m_hCloth);
  pCloth->m_Importance = plJoltClothImportance::Always;
  pCloth->SetSimulate(!m_bPaintMode, plTime::MakeZero());
  pCloth->SetDebugView(m_iView);

  pWorld->SetWorldSimulationEnabled(true);

  // The cloth on disk may be older than the document, or not there yet: the window answers with the current cook.
  plSimpleDocumentConfigMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();
  msg.m_sWhatToDo = "ClothPreviewReady";
  SendProcessMessage(&msg);
}

plJoltCharacterClothComponent* plJoltClothContext::GetCloth()
{
  plJoltCharacterClothComponent* pCloth = nullptr;
  m_pWorld->TryGetComponent(m_hClothComponent, pCloth);
  return pCloth;
}

void plJoltClothContext::DrawPaintOverlay()
{
  PL_LOCK(m_pWorld->GetReadMarker());

  const plAnimatedMeshComponent* pMesh = nullptr;
  if (!m_pWorld->TryGetComponent(m_hMeshComponent, pMesh))
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
    plDynamicArray<plDebugRenderer::Triangle> triangles;
    triangles.Reserve(desc.m_Indices.GetCount() / 3 * 2);

    for (plUInt32 i = 0; i + 2 < desc.m_Indices.GetCount(); i += 3)
    {
      const plUInt32 a = desc.m_Indices[i];
      const plUInt32 b = desc.m_Indices[i + 1];
      const plUInt32 c = desc.m_Indices[i + 2];

      plVec3 vNormal = (positions[b] - positions[a]).CrossRH(positions[c] - positions[a]);
      if (vNormal.NormalizeIfNotZero(plVec3::MakeZero(), 1.0e-12f).Failed())
        continue;

      const float fValue = (m_PaintValues[a] + m_PaintValues[b] + m_PaintValues[c]) / (3.0f * 255.0f);
      const plColor color = plJoltCharacterClothComponent::GetHeatColor(fValue);

      // on both sides of the surface, lifted off it so that it does not flicker against the mesh
      const plVec3 vLift = vNormal * 0.004f;

      plDebugRenderer::Triangle& front = triangles.ExpandAndGetRef();
      front = plDebugRenderer::Triangle(positions[a] + vLift, positions[b] + vLift, positions[c] + vLift);
      front.m_color = color;

      plDebugRenderer::Triangle& back = triangles.ExpandAndGetRef();
      back = plDebugRenderer::Triangle(positions[a] - vLift, positions[c] - vLift, positions[b] - vLift);
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

void plJoltClothContext::RunStressTest()
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

  // an editor build is not optimized, so its solve time says nothing: it is reported, not gated
  plJoltClothStressGates gates;
  gates.m_fMaxSolveMs = 0.0f;

  plJoltClothStressScriptOptions options;
  options.m_vWind.Set(-m_fWind, 0, 0);

  plJoltClothStressResult result;
  if (plJoltClothStressTest::RunScript(*m_pWorld, desc, body.m_Colliders, gates, options, result).Failed())
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
  sText.AppendFormat("{} over {} frames: stand, sprint, hard stop, sprint backwards, spin, teleport.\n\n", result.Passed() ? "Passed" : "FAILED", result.m_Frames.GetCount());

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

  PL_LOCK(m_pWorld->GetReadMarker());

  const plJoltCharacterClothComponent* pCloth = nullptr;
  if (!m_pWorld->TryGetComponent(m_hClothComponent, pCloth))
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
