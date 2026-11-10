#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/ClothAsset/JoltClothAssetObjects.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothFabricPreset, 1)
  PL_ENUM_CONSTANTS(plJoltClothFabricPreset::Custom, plJoltClothFabricPreset::Silk, plJoltClothFabricPreset::Cotton, plJoltClothFabricPreset::Wool)
  PL_ENUM_CONSTANTS(plJoltClothFabricPreset::Denim, plJoltClothFabricPreset::Leather, plJoltClothFabricPreset::Chainmail)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothColliderSource, 1)
  PL_ENUM_CONSTANTS(plJoltClothColliderSource::None, plJoltClothColliderSource::SkeletonShapes)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltClothColliderShape, 1)
  PL_ENUM_CONSTANTS(plJoltClothColliderShape::Sphere, plJoltClothColliderShape::Capsule, plJoltClothColliderShape::Box)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_TYPE(plJoltClothAssetCollider, plNoBase, 1, plRTTIDefaultAllocator<plJoltClothAssetCollider>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Joint", m_sJoint),
    PL_ENUM_MEMBER_PROPERTY("Shape", plJoltClothColliderShape, m_Shape),
    PL_MEMBER_PROPERTY("Offset", m_vOffset),
    PL_MEMBER_PROPERTY("Rotation", m_qRotation),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.08f), new plClampValueAttribute(0.005f, 2.0f)),
    PL_MEMBER_PROPERTY("Length", m_fLength)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 5.0f)),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.1f))),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    // Handles in the Cloth window, relative to the collider's joint: the Rotation row switches on a turn handle that
    // sits at the offset, the Offset row a move handle. The move handle comes last so that the Offset row gets it.
    new plJoltClothColliderHandleAttribute(nullptr, "Rotation", "Offset"),
    new plJoltClothColliderHandleAttribute("Offset"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothColliderHandleAttribute, 1, plRTTIDefaultAllocator<plJoltClothColliderHandleAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;

void plJoltClothColliderHandleAdapter::Finalize()
{
  plTransformManipulatorAdapter::Finalize();

  m_TranslateGizmo.SetSnapping(false);
  m_RotateGizmo.SetSnapping(false);
}

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltClothAssetProperties, 1, plRTTIDefaultAllocator<plJoltClothAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Mesh", m_sMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Mesh_Skinned", plDependencyFlags::Transform | plDependencyFlags::Thumbnail)),
    PL_ARRAY_MEMBER_PROPERTY("Garments", m_Garments),

    PL_MEMBER_PROPERTY("WeldDistance", m_fWeldDistance)->AddAttributes(new plDefaultValueAttribute(0.0001f), new plClampValueAttribute(0.0f, 0.1f), new plGroupAttribute("Sim Mesh")),

    PL_MEMBER_PROPERTY("UseImportedMasks", m_bUseImportedMasks)->AddAttributes(new plDefaultValueAttribute(true), new plGroupAttribute("Maps")),
    PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("BackstopDistance", m_fBackstopDistance)->AddAttributes(new plDefaultValueAttribute(0.02f), new plClampValueAttribute(0.0f, 0.2f)),
    PL_MEMBER_PROPERTY("BackstopRadius", m_fBackstopRadius)->AddAttributes(new plDefaultValueAttribute(0.4f), new plClampValueAttribute(0.05f, 1.0f)),
    PL_MEMBER_PROPERTY("PaintedMaps", m_PaintedMaps)->AddAttributes(new plHiddenAttribute()),

    PL_ENUM_MEMBER_PROPERTY("FabricPreset", plJoltClothFabricPreset, m_FabricPreset)->AddAttributes(new plGroupAttribute("Fabric")),
    PL_MEMBER_PROPERTY("Density", m_fDensity)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.01f, 20.0f)),
    PL_MEMBER_PROPERTY("Stretch", m_fStretch)->AddAttributes(new plDefaultValueAttribute(0.95f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Shear", m_fShear)->AddAttributes(new plDefaultValueAttribute(0.9f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Bend", m_fBend)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Damping", m_fDamping)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Drag", m_fDrag)->AddAttributes(new plDefaultValueAttribute(0.7f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("Lift", m_fLift)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("Friction", m_fFriction)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Thickness", m_fThickness)->AddAttributes(new plDefaultValueAttribute(0.01f), new plClampValueAttribute(0.0f, 0.1f)),

    PL_MEMBER_PROPERTY("StepRate", m_fStepRate)->AddAttributes(new plDefaultValueAttribute(60.0f), new plClampValueAttribute(15.0f, 240.0f), new plGroupAttribute("Simulation")),
    PL_MEMBER_PROPERTY("Iterations", m_uiIterations)->AddAttributes(new plDefaultValueAttribute(8), new plClampValueAttribute(1, 32)),
    PL_MEMBER_PROPERTY("Tethers", m_bTethers)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("TetherSlack", m_fTetherSlack)->AddAttributes(new plDefaultValueAttribute(1.03f), new plClampValueAttribute(1.0f, 1.5f)),
    PL_MEMBER_PROPERTY("StretchLimit", m_fStretchLimit)->AddAttributes(new plDefaultValueAttribute(0.08f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_ENUM_MEMBER_PROPERTY("WorldCollision", plJoltClothWorldCollision, m_WorldCollision),
    PL_MEMBER_PROPERTY("GravityFactor", m_fGravityFactor)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("FullRateDistance", m_fFullRateDistance)->AddAttributes(new plDefaultValueAttribute(14.0f), new plClampValueAttribute(0.0f, 1000.0f)),
    PL_MEMBER_PROPERTY("SimulateDistance", m_fSimulateDistance)->AddAttributes(new plDefaultValueAttribute(30.0f), new plClampValueAttribute(0.0f, 1000.0f)),

    PL_ENUM_MEMBER_PROPERTY("ColliderSource", plJoltClothColliderSource, m_ColliderSource)->AddAttributes(new plGroupAttribute("Colliders")),
    PL_ARRAY_MEMBER_PROPERTY("SkeletonShapeJoints", m_SkeletonShapeJoints),
    PL_MEMBER_PROPERTY("ColliderInflate", m_fColliderInflate)->AddAttributes(new plClampValueAttribute(-0.05f, 0.2f)),
    PL_ARRAY_MEMBER_PROPERTY("Colliders", m_Colliders),

    PL_MEMBER_PROPERTY("PreviewClip", m_sPreviewClip)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Keyframe_Animation", plDependencyFlags::None), new plGroupAttribute("Preview")),
    PL_ARRAY_MEMBER_PROPERTY("StressClips", m_StressClips)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Keyframe_Animation", plDependencyFlags::None)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

bool plJoltClothFabricPreset::GetFabric(Enum preset, plJoltClothFabric& out_fabric)
{
  // density, stretch, shear, bend, damping, drag, lift
  struct Values
  {
    float m_fDensity, m_fStretch, m_fShear, m_fBend, m_fDamping, m_fDrag, m_fLift;
  };

  Values values;
  switch (preset)
  {
    case Silk:
      values = {0.08f, 0.90f, 0.85f, 0.05f, 0.10f, 0.9f, 0.3f};
      break;
    case Cotton:
      values = {0.15f, 0.95f, 0.90f, 0.20f, 0.20f, 0.7f, 0.2f};
      break;
    case Wool:
      values = {0.30f, 0.90f, 0.85f, 0.35f, 0.35f, 0.6f, 0.15f};
      break;
    case Denim:
      values = {0.40f, 0.98f, 0.95f, 0.55f, 0.30f, 0.5f, 0.1f};
      break;
    case Leather:
      values = {0.90f, 0.99f, 0.97f, 0.80f, 0.45f, 0.3f, 0.05f};
      break;
    case Chainmail:
      values = {6.00f, 1.00f, 0.60f, 0.10f, 0.60f, 0.1f, 0.0f};
      break;
    default:
      return false;
  }

  out_fabric.m_fDensity = values.m_fDensity;
  out_fabric.m_fStretch = values.m_fStretch;
  out_fabric.m_fShear = values.m_fShear;
  out_fabric.m_fBend = values.m_fBend;
  out_fabric.m_fDamping = values.m_fDamping;
  out_fabric.m_fDrag = values.m_fDrag;
  out_fabric.m_fLift = values.m_fLift;
  return true;
}

plJoltClothFabric plJoltClothAssetProperties::GetFabric() const
{
  plJoltClothFabric fabric;
  fabric.m_fDensity = m_fDensity;
  fabric.m_fStretch = m_fStretch;
  fabric.m_fShear = m_fShear;
  fabric.m_fBend = m_fBend;
  fabric.m_fDamping = m_fDamping;
  fabric.m_fDrag = m_fDrag;
  fabric.m_fLift = m_fLift;

  // a preset replaces the material values; friction and thickness belong to the garment and stay
  plJoltClothFabricPreset::GetFabric(m_FabricPreset, fabric);

  fabric.m_fFriction = m_fFriction;
  fabric.m_fThickness = m_fThickness;
  return fabric;
}

void plJoltClothAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plJoltClothAssetCollider>())
  {
    const plInt64 iShape = e.m_pObject->GetTypeAccessor().GetValue("Shape").ConvertTo<plInt64>();

    auto& props = *e.m_pPropertyStates;
    props["Radius"].m_Visibility = iShape == plJoltClothColliderShape::Box ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["Length"].m_Visibility = iShape == plJoltClothColliderShape::Capsule ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["HalfExtents"].m_Visibility = iShape == plJoltClothColliderShape::Box ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    return;
  }

  if (e.m_pObject->GetTypeAccessor().GetType() != plGetStaticRTTI<plJoltClothAssetProperties>())
    return;

  const plInt64 iPreset = e.m_pObject->GetTypeAccessor().GetValue("FabricPreset").ConvertTo<plInt64>();
  const bool bTethers = e.m_pObject->GetTypeAccessor().GetValue("Tethers").ConvertTo<bool>();
  const plInt64 iColliderSource = e.m_pObject->GetTypeAccessor().GetValue("ColliderSource").ConvertTo<plInt64>();

  auto& props = *e.m_pPropertyStates;

  // a preset fixes the material values: show them only for Custom
  const plPropertyUiState::Visibility material = iPreset == plJoltClothFabricPreset::Custom ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  for (const char* szProperty : {"Density", "Stretch", "Shear", "Bend", "Damping", "Drag", "Lift"})
  {
    props[szProperty].m_Visibility = material;
  }

  props["TetherSlack"].m_Visibility = bTethers ? plPropertyUiState::Default : plPropertyUiState::Invisible;

  const plPropertyUiState::Visibility skeletonShapes = iColliderSource == plJoltClothColliderSource::SkeletonShapes ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  props["SkeletonShapeJoints"].m_Visibility = skeletonShapes;
  props["ColliderInflate"].m_Visibility = skeletonShapes;
}
