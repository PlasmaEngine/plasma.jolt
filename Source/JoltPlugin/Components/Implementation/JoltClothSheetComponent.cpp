#include <JoltPlugin/JoltPluginPCH.h>

#include <Core/Interfaces/WindWorldModule.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <JoltPlugin/Components/JoltClothSheetComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <JoltPlugin/Utilities/JoltConversionUtils.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/CustomMeshComponent.h>
#include <RendererCore/Meshes/DynamicMeshBufferResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

// clang-format off
plCVarInt cvar_JoltClothSolverIterations("Jolt.Cloth.SolverIterations", 0, plCVarFlags::Save, "Overrides the Jolt soft-body solver iteration count for cloth sheets (Jolt default 5). Lower = cheaper solve + softer cloth. 0 = Jolt default. Applied when a cloth body is (re)created.");

PL_BEGIN_STATIC_REFLECTED_BITFLAGS(plJoltClothSheetFlags, 1)
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedCornerTopLeft),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedCornerTopRight),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedCornerBottomRight),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedCornerBottomLeft),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedEdgeTop),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedEdgeRight),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedEdgeBottom),
  PL_ENUM_CONSTANT(plJoltClothSheetFlags::FixedEdgeLeft),
PL_END_STATIC_REFLECTED_BITFLAGS;

PL_BEGIN_COMPONENT_TYPE(plJoltClothSheetComponent, 4, plComponentMode::Static)
  {
    PL_BEGIN_PROPERTIES
    {
      PL_ACCESSOR_PROPERTY("Size", GetSize, SetSize)->AddAttributes(new plDefaultValueAttribute(plVec2(0.5f, 0.5f))),
      PL_ACCESSOR_PROPERTY("Segments", GetSegments, SetSegments)->AddAttributes(new plDefaultValueAttribute(plVec2U32(16, 16)), new plClampValueAttribute(plVec2U32(2, 2), plVec2U32(64, 64))),
      PL_MEMBER_PROPERTY("CollisionLayer", m_uiCollisionLayer)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer")),
      PL_MEMBER_PROPERTY("WindInfluence", m_fWindInfluence)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 10.0f)),
      PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
      PL_MEMBER_PROPERTY("Damping", m_fDamping)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
      PL_MEMBER_PROPERTY("Thickness", m_fThickness)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.0f, 0.5f)),
      PL_BITFLAGS_ACCESSOR_PROPERTY("Flags", plJoltClothSheetFlags, GetFlags, SetFlags),
      PL_RESOURCE_MEMBER_PROPERTY("Material", m_hMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
      PL_MEMBER_PROPERTY("TextureScale", m_vTextureScale)->AddAttributes(new plDefaultValueAttribute(plVec2(1.0f))),
      PL_MEMBER_PROPERTY("Color", m_Color)->AddAttributes(new plDefaultValueAttribute(plColor::White)),
    }
    PL_END_PROPERTIES;
    PL_BEGIN_ATTRIBUTES
    {
      new plCategoryAttribute("Physics/Jolt/Effects"),
    }
    PL_END_ATTRIBUTES;
    PL_BEGIN_MESSAGEHANDLERS
    {
      PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
    }
    PL_END_MESSAGEHANDLERS;
  }
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plJoltClothSheetComponent::plJoltClothSheetComponent() = default;
plJoltClothSheetComponent::~plJoltClothSheetComponent() = default;

void plJoltClothSheetComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_vSize;
  s << m_vNumVertices;
  s << m_uiCollisionLayer;
  s << m_fWindInfluence;
  s << m_fGravityFactor;
  s << m_fDamping;
  s << m_Flags;
  s << m_hMaterial;
  s << m_vTextureScale;
  s << m_Color;
  s << m_fThickness;
}

void plJoltClothSheetComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  PL_ASSERT_DEBUG(uiVersion >= 4, "Outdated version, please re-transform asset.");
  if (uiVersion < 4)
    return;

  auto& s = inout_stream.GetStream();

  s >> m_vSize;
  s >> m_vNumVertices;
  s >> m_uiCollisionLayer;
  s >> m_fWindInfluence;
  s >> m_fGravityFactor;
  s >> m_fDamping;
  s >> m_Flags;
  s >> m_hMaterial;
  s >> m_vTextureScale;
  s >> m_Color;
  s >> m_fThickness;
}

void plJoltClothSheetComponent::OnActivated()
{
  SUPER::OnActivated();

  SetupCloth();
}

void plJoltClothSheetComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  if (m_uiObjectFilterID == plInvalidIndex)
  {
    // only create a new filter ID, if none has been passed in manually

    plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();
    m_uiObjectFilterID = pModule->CreateObjectFilterID();
  }

  SetupCloth();

  m_BodyGlobalTransform = GetOwner()->GetGlobalTransform();
}

void plJoltClothSheetComponent::OnDeactivated()
{
  plRenderDataManager* pRenderDataManager = GetWorld()->GetModule<plRenderDataManager>();
  pRenderDataManager->DeleteInstanceData(m_InstanceDataOffset);

  RemoveBody();

  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  pModule->DeallocateUserData(m_uiUserDataIndex);

  pModule->DeleteObjectFilterID(m_uiObjectFilterID);

  SUPER::OnDeactivated();
}

plResult plJoltClothSheetComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  if (m_BSphere.IsValid())
  {
    ref_bounds.ExpandToInclude(plBoundingBoxSphere::MakeFromSphere(m_BSphere));
  }
  else
  {
    plBoundingBox box = plBoundingBox::MakeInvalid();
    box.ExpandToInclude(plVec3::MakeZero());
    box.ExpandToInclude(plVec3(0, 0, -0.1f));
    box.ExpandToInclude(plVec3(m_vSize.x, m_vSize.y, 0.1f));

    ref_bounds.ExpandToInclude(plBoundingBoxSphere::MakeFromBox(box));
  }

  return PL_SUCCESS;
}

void plJoltClothSheetComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!m_hDynamicMeshBuffer.IsValid())
    return;

  // Force dynamic instance data buffer since the render data is not cached, so we would trash the static instance data buffer every frame.
  const bool bDynamic = true;
  const plTransform globalTransform = IsActiveAndSimulating() ? m_BodyGlobalTransform : GetOwner()->GetGlobalTransform();
  auto hInstanceDataBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, globalTransform, m_InstanceDataOffset, GetUniqueIdForRendering(), m_Color);

  plCustomMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plCustomMeshRenderData>(GetOwner());
  {
    pRenderData->m_uiNumInstances = 1;
    pRenderData->m_DataOffsets.m_uiInstance = m_InstanceDataOffset.m_uiOffset;
    pRenderData->m_hInstanceDataBuffer = hInstanceDataBuffer;
    pRenderData->m_fSortingDepthOffset = 0.0f;

    pRenderData->m_hMaterial = m_hMaterial;
    pRenderData->m_hDynamicMeshBuffer = m_hDynamicMeshBuffer;
    pRenderData->m_uiFirstPrimitive = 0;
    pRenderData->m_uiNumPrimitives = (m_vNumVertices.x - 1) * (m_vNumVertices.y - 1) * 2;

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
    pRenderData->m_FallbackGlobalBBox = GetOwner()->GetGlobalBounds().GetBox();
#endif

    pRenderData->FillSortingKey();
  }

  plRenderData::Category category = m_RenderDataCategory;
  if (!category.IsValid())
  {
    category = plDefaultRenderDataCategories::LitOpaque; // use as default fallback

    if (m_hMaterial.IsValid())
    {
      plResourceLock<plMaterialResource> pMaterial(m_hMaterial, plResourceAcquireMode::AllowLoadingFallback);
      category = pMaterial->GetRenderDataCategory();

      if (pMaterial.GetAcquireResult() != plResourceAcquireResult::LoadingFallback)
      {
        // if this is the final result, cache it
        m_RenderDataCategory = category;
      }
    }
  }

  pRenderData->m_DataOffsets.m_uiMaterial = plPackMaterialDataOffset(m_hMaterial, 0);
  msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
}

void plJoltClothSheetComponent::SetSize(plVec2 vVal)
{
  m_vSize = vVal;
  SetupCloth();
}

void plJoltClothSheetComponent::SetSegments(plVec2U32 vVal)
{
  m_vNumVertices = vVal;
  SetupCloth();
}

void plJoltClothSheetComponent::SetFlags(plBitflags<plJoltClothSheetFlags> flags)
{
  m_Flags = flags;
  SetupCloth();
}

void plJoltClothSheetComponent::UpdatePreAsync()
{
  // Apply the (possibly runtime-changed) solver-iteration override to the LIVE body so Jolt.Cloth.SolverIterations takes
  // effect without recreating the cloth. Must run before the simulation (PreAsync, before StartSimulation kicks the sim
  // task). 0 = leave the value set at creation time (Jolt default 5).
  if (const plInt32 iIterations = cvar_JoltClothSolverIterations.GetValue(); iIterations > 0)
  {
    JPH::BodyID bodyId(m_uiJoltBodyID);
    if (!bodyId.IsInvalid())
    {
      if (const plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>())
      {
        JPH::BodyLockWrite lock(pModule->GetJoltSystem()->GetBodyLockInterface(), bodyId);
        if (lock.SucceededAndIsInBroadPhase())
        {
          if (auto* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties()))
            pMotion->SetNumIterations(static_cast<JPH::uint32>(iIterations));
        }
      }
    }
  }

  if (GetOwner()->GetVisibilityState(60) == plVisibilityState::Direct)
  {
    // only apply wind to directly visible pieces of cloth
    ApplyWind();
  }
}

void plJoltClothSheetComponent::UpdatePostAsync()
{
  const plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  auto* pSystem = pModule->GetJoltSystem();
  const JPH::BodyLockInterface* pLi = &pSystem->GetBodyLockInterface();

  JPH::BodyID bodyId(m_uiJoltBodyID);

  if (bodyId.IsInvalid())
    return;

  // Get the body
  JPH::BodyLockRead lock(*pLi, bodyId);
  if (!lock.SucceededAndIsInBroadPhase())
    return;

  const JPH::Body& body = lock.GetBody();

  {
    plBoundingSphere prevBounds = m_BSphere;

    // TODO: should rather iterate over all active (soft) bodies, than to check this here
    if (!body.IsActive())
      return;

    const JPH::AABox box = body.GetWorldSpaceBounds();

    const plTransform t = GetOwner()->GetGlobalTransform().GetInverse();

    m_BSphere.m_vCenter = t.TransformPosition(plJoltConversionUtils::ToVec3(box.GetCenter()));

    const plVec3 ext = plJoltConversionUtils::ToVec3(box.GetExtent());
    m_BSphere.m_fRadius = plMath::Max(ext.x, ext.y, ext.z);

    if (prevBounds != m_BSphere)
    {
      TriggerLocalBoundsUpdate();
    }
  }

  // Don't update mesh and transform when invisible
  if (GetOwner()->GetVisibilityState() == plVisibilityState::Invisible)
    return;

  {
    const JPH::SoftBodyMotionProperties* pMotion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    const JPH::Array<JPH::SoftBodyMotionProperties::Vertex>& particles = pMotion->GetVertices();

    plResourceLock<plDynamicMeshBufferResource> pDynamicMeshBuffer(m_hDynamicMeshBuffer, plResourceAcquireMode::BlockTillLoaded);
    auto positions = pDynamicMeshBuffer->AccessPositionData();

    const plVec2U32 vNumVertices = m_vNumVertices;

    plUInt32 vidx = 0;
    for (plUInt32 y = 0; y < vNumVertices.y; ++y)
    {
      for (plUInt32 x = 0; x < vNumVertices.x; ++x, ++vidx)
      {
        positions[vidx] = plJoltConversionUtils::ToVec3(particles[vidx].mPosition);
      }
    }

    plDynamicMeshBufferResource::CalculateGridNormalAndTangents(pDynamicMeshBuffer.GetPointerNonConst(), vNumVertices);
  }

  {
    m_BodyGlobalTransform = GetOwner()->GetGlobalTransform();

    const auto& transformed_shape = body.GetTransformedShape();
    JPH::RMat44 matrix = transformed_shape.GetCenterOfMassTransform();

    m_BodyGlobalTransform.m_vPosition = plJoltConversionUtils::ToVec3(matrix.GetTranslation());
    m_BodyGlobalTransform.m_qRotation = plJoltConversionUtils::ToQuat(matrix.GetRotation().GetQuaternion());
  }
}


void plJoltClothSheetComponent::ApplyWind()
{
  if (m_fWindInfluence <= 0.0f)
    return;

  if (!m_BSphere.IsValid())
    return;

  if (const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
  {
    const plVec3 vSamplePos = GetOwner()->GetGlobalTransform().TransformPosition(m_BSphere.m_vCenter);

    const plVec3 vWind = pWind->GetWindAt(vSamplePos) * m_fWindInfluence;

    if (!vWind.IsZero())
    {
      plVec3 windForce = vWind;
      windForce += pWind->ComputeWindFlutter(vWind, vWind.GetOrthogonalVector(), 5.0f, GetOwner()->GetStableRandomSeed());

      JPH::Vec3 windVel = plJoltConversionUtils::ToVec3(windForce);

      plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
      auto* pSystem = pModule->GetJoltSystem();
      const JPH::BodyLockInterface* pLi = &pSystem->GetBodyLockInterface();

      JPH::BodyID bodyId(m_uiJoltBodyID);

      if (bodyId.IsInvalid())
        return;

      // Get write access to the body
      JPH::BodyLockWrite lock(*pLi, bodyId);
      if (!lock.SucceededAndIsInBroadPhase())
        return;

      JPH::Body& body = lock.GetBody();
      JPH::SoftBodyMotionProperties* pMotion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());

      if (!body.IsActive())
      {
        pSystem->GetBodyInterfaceNoLock().ActivateBody(bodyId);
      }

      JPH::Array<JPH::SoftBodyMotionProperties::Vertex>& particles = pMotion->GetVertices();

      // randomize which vertices get the wind velocity applied,
      // both to save performance and also to introduce a nice ripple effect
      const plUInt32 uiStart = GetWorld()->GetRandomNumberGenerator().UIntInRange(plMath::Min<plUInt32>(16u, (plUInt32)particles.size()));
      const plUInt32 uiStep = GetWorld()->GetRandomNumberGenerator().IntMinMax(16, 16 + 32);

      for (plUInt32 i = uiStart; i < particles.size(); i += uiStep)
      {
        if (particles[i].mInvMass > 0)
        {
          particles[i].mVelocity = windVel;
        }
      }
    }
  }
}

static JPH::Ref<JPH::SoftBodySharedSettings> CreateCloth(plVec2U32 vNumVertices, plVec2 vSpacing, plBitflags<plJoltClothSheetFlags> flags, float fPerVertexMass)
{
  // Create settings
  JPH::SoftBodySharedSettings* settings = new JPH::SoftBodySharedSettings;

  const float fInvVtxMass = 1.0f / fPerVertexMass;

  for (plUInt32 y = 0; y < vNumVertices.y; ++y)
  {
    for (plUInt32 x = 0; x < vNumVertices.x; ++x)
    {
      JPH::SoftBodySharedSettings::Vertex v;
      v.mPosition = JPH::Float3(x * vSpacing.x, y * vSpacing.y, 0.0f);
      v.mInvMass = fInvVtxMass;
      settings->mVertices.push_back(v);
    }
  }

  // Function to get the vertex index of a point on the cloth
  auto GetIdx = [vNumVertices](plUInt32 x, plUInt32 y) -> plUInt32
  {
    return x + y * vNumVertices.x;
  };

  if (flags.IsAnyFlagSet())
  {
    if (flags.IsSet(plJoltClothSheetFlags::FixedCornerTopLeft))
    {
      settings->mVertices[GetIdx(0, 0)].mInvMass = 0.0f;
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedCornerTopRight))
    {
      settings->mVertices[GetIdx(vNumVertices.x - 1, 0)].mInvMass = 0.0f;
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedCornerBottomLeft))
    {
      settings->mVertices[GetIdx(0, vNumVertices.y - 1)].mInvMass = 0.0f;
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedCornerBottomRight))
    {
      settings->mVertices[GetIdx(vNumVertices.x - 1, vNumVertices.y - 1)].mInvMass = 0.0f;
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedEdgeTop))
    {
      for (plUInt32 x = 0; x < vNumVertices.x; ++x)
      {
        settings->mVertices[GetIdx(x, 0)].mInvMass = 0.0f;
      }
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedEdgeBottom))
    {
      for (plUInt32 x = 0; x < vNumVertices.x; ++x)
      {
        settings->mVertices[GetIdx(x, vNumVertices.y - 1)].mInvMass = 0.0f;
      }
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedEdgeLeft))
    {
      for (plUInt32 y = 0; y < vNumVertices.y; ++y)
      {
        settings->mVertices[GetIdx(0, y)].mInvMass = 0.0f;
      }
    }

    if (flags.IsSet(plJoltClothSheetFlags::FixedEdgeRight))
    {
      for (plUInt32 y = 0; y < vNumVertices.y; ++y)
      {
        settings->mVertices[GetIdx(vNumVertices.x - 1, y)].mInvMass = 0.0f;
      }
    }
  }

  // Create edges
  for (plUInt32 y = 0; y < vNumVertices.y; ++y)
  {
    for (plUInt32 x = 0; x < vNumVertices.x; ++x)
    {
      JPH::SoftBodySharedSettings::Edge e;
      e.mCompliance = 0.00001f;
      e.mVertex[0] = GetIdx(x, y);
      if (x < vNumVertices.x - 1)
      {
        e.mVertex[1] = GetIdx(x + 1, y);
        settings->mEdgeConstraints.push_back(e);
      }
      if (y < vNumVertices.y - 1)
      {
        e.mVertex[1] = GetIdx(x, y + 1);
        settings->mEdgeConstraints.push_back(e);
      }
      if (x < vNumVertices.x - 1 && y < vNumVertices.y - 1)
      {
        e.mVertex[1] = GetIdx(x + 1, y + 1);
        settings->mEdgeConstraints.push_back(e);

        e.mVertex[0] = GetIdx(x + 1, y);
        e.mVertex[1] = GetIdx(x, y + 1);
        settings->mEdgeConstraints.push_back(e);
      }
    }
  }

  settings->CalculateEdgeLengths();

  // Create faces
  for (plUInt32 y = 0; y < vNumVertices.y - 1; ++y)
  {
    for (plUInt32 x = 0; x < vNumVertices.x - 1; ++x)
    {
      JPH::SoftBodySharedSettings::Face f;
      f.mVertex[0] = GetIdx(x, y);
      f.mVertex[1] = GetIdx(x, y + 1);
      f.mVertex[2] = GetIdx(x + 1, y + 1);
      settings->AddFace(f);

      f.mVertex[1] = GetIdx(x + 1, y + 1);
      f.mVertex[2] = GetIdx(x + 1, y);
      settings->AddFace(f);
    }
  }

  settings->Optimize();

  return settings;
}


void plJoltClothSheetComponent::SetupCloth()
{
  m_BSphere = plBoundingSphere::MakeInvalid();

  if (IsActiveAndSimulating())
  {
    RemoveBody();

    float fPerVertexMass = 1.0f; // default value

    plJoltWorldModule* pModule = GetWorld()->GetOrCreateModule<plJoltWorldModule>();
    auto* pSystem = pModule->GetJoltSystem();
    auto* pBodies = &pSystem->GetBodyInterface();

    JPH::Ref<JPH::SoftBodySharedSettings> settings = CreateCloth(m_vNumVertices, m_vSize.CompDiv(plVec2(static_cast<float>(m_vNumVertices.x - 1), static_cast<float>(m_vNumVertices.y - 1))), m_Flags, fPerVertexMass);

    plTransform t = GetOwner()->GetGlobalTransform();

    plJoltUserData* pUserData = nullptr;
    m_uiUserDataIndex = pModule->AllocateUserData(pUserData);
    pUserData->Init(this);

    JPH::SoftBodyCreationSettings cloth(settings, plJoltConversionUtils::ToVec3(t.m_vPosition), plJoltConversionUtils::ToQuat(t.m_qRotation), plJoltCollisionFiltering::ConstructObjectLayer(m_uiCollisionLayer, plJoltBroadphaseLayer::Cloth));

    cloth.mVertexRadius = m_fThickness;
    cloth.mPressure = 0.0f;
    cloth.mLinearDamping = m_fDamping;
    cloth.mGravityFactor = m_fGravityFactor;
    if (const plInt32 iIterations = cvar_JoltClothSolverIterations.GetValue(); iIterations > 0)
      cloth.mNumIterations = static_cast<plUInt32>(iIterations);
    cloth.mUserData = reinterpret_cast<plUInt64>(pUserData);
    cloth.mCollisionGroup.SetGroupID(m_uiObjectFilterID);
    // cloth.mCollisionGroup.SetGroupFilter(pModule->GetGroupFilter()); // the group filter is only needed for objects constrained via joints

    auto pBody = pBodies->CreateSoftBody(cloth);

    m_uiJoltBodyID = pBody->GetID().GetIndexAndSequenceNumber();

    pModule->QueueBodyToAdd(pBody, true);
  }

  if (IsActiveAndInitialized() && m_vSize.x > 0 && m_vSize.y > 0 && m_vNumVertices.x > 1 && m_vNumVertices.y > 1)
  {
    plStringBuilder sResourceName;
    sResourceName.SetFormat("JoltClothSheet_{}_{}x{}_{}x{}_{}x{}", plArgP(this), m_vSize.x, m_vSize.y, m_vNumVertices.x, m_vNumVertices.y, m_vTextureScale.x, m_vTextureScale.y);

    m_hDynamicMeshBuffer = plResourceManager::GetExistingResource<plDynamicMeshBufferResource>(sResourceName);

    if (!m_hDynamicMeshBuffer.IsValid())
    {
      plDynamicMeshBufferResourceDescriptor desc;
      desc.m_uiMaxVertices = m_vNumVertices.x * m_vNumVertices.y;
      desc.m_IndexType = plGALIndexType::UShort;
      desc.m_uiMaxPrimitives = (m_vNumVertices.x - 1) * (m_vNumVertices.y - 1) * 2;

      m_hDynamicMeshBuffer = plResourceManager::GetOrCreateResource<plDynamicMeshBufferResource>(sResourceName, std::move(desc));
    }

    plResourceLock<plDynamicMeshBufferResource> pDynamicMeshBuffer(m_hDynamicMeshBuffer, plResourceAcquireMode::BlockTillLoaded);
    plDynamicMeshBufferResource::CreateGridXY(pDynamicMeshBuffer.GetPointerNonConst(), m_vSize, m_vNumVertices);
  }

  TriggerLocalBoundsUpdate();
}

void plJoltClothSheetComponent::RemoveBody()
{
  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();

  JPH::BodyID bodyId(m_uiJoltBodyID);

  if (!bodyId.IsInvalid())
  {
    auto* pSystem = pModule->GetJoltSystem();
    auto* pBodies = &pSystem->GetBodyInterface();

    if (pBodies->IsAdded(bodyId))
    {
      pBodies->RemoveBody(bodyId);
    }
    else
    {
      // still in the module's pending-add queue (deactivated in the same frame
      // it was created); the queue must not reference a destroyed body
      pModule->RemoveBodyFromQueue(bodyId);
    }

    pBodies->DestroyBody(bodyId);
    m_uiJoltBodyID = JPH::BodyID::cInvalidBodyID;
  }

  // TODO: currently not yet needed
  // pModule->DeallocateUserData(m_uiUserDataIndex);
  // pModule->DeleteObjectFilterID(m_uiObjectFilterID);
}

//////////////////////////////////////////////////////////////////////////

plJoltClothSheetComponentManager::plJoltClothSheetComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

plJoltClothSheetComponentManager::~plJoltClothSheetComponentManager() = default;

void plJoltClothSheetComponentManager::Initialize()
{
  SUPER::Initialize();

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltClothSheetComponentManager::UpdatePreAsync, this);
    desc.m_Phase = plWorldUpdatePhase::PreAsync;
    desc.m_bOnlyUpdateWhenSimulating = true;

    this->RegisterUpdateFunction(desc);
  }

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plJoltClothSheetComponentManager::UpdatePostAsync, this);
    desc.m_Phase = plWorldUpdatePhase::PostAsync;
    desc.m_bOnlyUpdateWhenSimulating = true;

    this->RegisterUpdateFunction(desc);
  }
}

void plJoltClothSheetComponentManager::UpdatePreAsync(const plWorldModule::UpdateContext& context)
{
  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  if (pModule == nullptr || pModule->GetJoltUpdateCounter() == m_uiLastJoltUpdateCounter)
  {
    // skip cloth updates, when there was no Jolt update yet
    return;
  }

  m_uiLastJoltUpdateCounter = pModule->GetJoltUpdateCounter();

  for (auto it = this->m_ComponentStorage.GetIterator(context.m_uiFirstComponentIndex, context.m_uiComponentCount); it.IsValid(); ++it)
  {
    if (it->IsActiveAndInitialized())
    {
      it->UpdatePreAsync();
    }
  }
}

void plJoltClothSheetComponentManager::UpdatePostAsync(const plWorldModule::UpdateContext& context)
{
  plJoltWorldModule* pModule = GetWorld()->GetModule<plJoltWorldModule>();
  if (pModule == nullptr || pModule->GetJoltUpdateCounter() == m_uiLastJoltUpdateCounter)
  {
    // skip cloth updates, when there was no Jolt update yet
    return;
  }

  for (auto it = this->m_ComponentStorage.GetIterator(context.m_uiFirstComponentIndex, context.m_uiComponentCount); it.IsValid(); ++it)
  {
    if (it->IsActiveAndInitialized())
    {
      it->UpdatePostAsync();
    }
  }
}


PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltClothSheetComponent);
