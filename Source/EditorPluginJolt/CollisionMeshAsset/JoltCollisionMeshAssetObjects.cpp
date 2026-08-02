#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/CollisionMeshAsset/JoltCollisionMeshAssetObjects.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plJoltSurfaceResourceSlot, plNoBase, 1, plRTTIDefaultAllocator<plJoltSurfaceResourceSlot>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Label", m_sLabel)->AddAttributes(new plReadOnlyAttribute()),
    PL_MEMBER_PROPERTY("Resource", m_sResource)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Surface", plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("Exclude", m_bExclude),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltCollisionMeshType, 2)
  PL_ENUM_CONSTANT(plJoltCollisionMeshType::ConvexHull),
  PL_ENUM_CONSTANT(plJoltCollisionMeshType::TriangleMesh),
  PL_ENUM_CONSTANT(plJoltCollisionMeshType::Cylinder),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltConvexCollisionMeshType, 1)
  PL_ENUM_CONSTANT(plJoltConvexCollisionMeshType::ConvexHull),
  PL_ENUM_CONSTANT(plJoltConvexCollisionMeshType::Cylinder),
  PL_ENUM_CONSTANT(plJoltConvexCollisionMeshType::ConvexDecomposition),
  PL_ENUM_CONSTANT(plJoltConvexCollisionMeshType::ConvexHullGroup),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plJoltCollisionMeshAssetProperties, 3, plRTTIDefaultAllocator<plJoltCollisionMeshAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("ImportTransform", plMeshImportTransform, m_ImportTransform),
    PL_ENUM_MEMBER_PROPERTY("RightDir", plBasisAxis, m_RightDir)->AddAttributes(new plDefaultValueAttribute((int)plBasisAxis::NegativeX)),
    PL_ENUM_MEMBER_PROPERTY("UpDir", plBasisAxis, m_UpDir)->AddAttributes(new plDefaultValueAttribute((int)plBasisAxis::PositiveY)),
    PL_MEMBER_PROPERTY("FlipForwardDir", m_bFlipForwardDir)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UniformScaling", m_fUniformScaling)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("IsConvexMesh", m_bIsConvexMesh)->AddAttributes(new plHiddenAttribute()),
    PL_ENUM_MEMBER_PROPERTY("ConvexMeshType", plJoltConvexCollisionMeshType, m_ConvexMeshType),
    PL_MEMBER_PROPERTY("MaxConvexPieces", m_uiMaxConvexPieces)->AddAttributes(new plDefaultValueAttribute(5)),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Radius2", m_fRadius2)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Height", m_fHeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Detail", m_uiDetail)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(0, 32)),
    PL_MEMBER_PROPERTY("MeshFile", m_sMeshFile)->AddAttributes(new plFileBrowserAttribute("Select Mesh", plFileBrowserAttribute::Meshes)),
    PL_MEMBER_PROPERTY("MeshIncludeTags", m_sMeshIncludeTags),
    PL_MEMBER_PROPERTY("MeshExcludeTags", m_sMeshExcludeTags),
    PL_ARRAY_MEMBER_PROPERTY("Surfaces", m_Slots)->AddAttributes(new plContainerAttribute(false, false, true)),
    PL_MEMBER_PROPERTY("Surface", m_sConvexMeshSurface)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Surface", plDependencyFlags::Package)),

    PL_MEMBER_PROPERTY("SimplifyMesh", m_bSimplifyMesh),
    PL_MEMBER_PROPERTY("MeshSimplification", m_uiMeshSimplification)->AddAttributes(new plDefaultValueAttribute(50), new plClampValueAttribute(1, 100)),
    PL_MEMBER_PROPERTY("MaxSimplificationError", m_uiMaxSimplificationError)->AddAttributes(new plDefaultValueAttribute(20), new plClampValueAttribute(1, 100)),
    PL_MEMBER_PROPERTY("AggressiveSimplification", m_bAggressiveSimplification),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plJoltCollisionMeshAssetProperties::plJoltCollisionMeshAssetProperties() = default;
plJoltCollisionMeshAssetProperties::~plJoltCollisionMeshAssetProperties() = default;

void plJoltCollisionMeshAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() != plGetStaticRTTI<plJoltCollisionMeshAssetProperties>())
    return;

  const bool bSimplify = e.m_pObject->GetTypeAccessor().GetValue("SimplifyMesh").ConvertTo<bool>();
  const bool isConvex = e.m_pObject->GetTypeAccessor().GetValue("IsConvexMesh").ConvertTo<bool>();
  const plInt64 meshType = e.m_pObject->GetTypeAccessor().GetValue("ConvexMeshType").ConvertTo<plInt64>();

  auto& props = *e.m_pPropertyStates;

  props["Radius"].m_Visibility = plPropertyUiState::Invisible;
  props["Radius2"].m_Visibility = plPropertyUiState::Invisible;
  props["Height"].m_Visibility = plPropertyUiState::Invisible;
  props["Detail"].m_Visibility = plPropertyUiState::Invisible;
  props["MeshFile"].m_Visibility = plPropertyUiState::Invisible;
  props["MeshIncludeTags"].m_Visibility = plPropertyUiState::Invisible;
  props["MeshExcludeTags"].m_Visibility = plPropertyUiState::Invisible;
  props["ConvexMeshType"].m_Visibility = plPropertyUiState::Invisible;
  props["MaxConvexPieces"].m_Visibility = plPropertyUiState::Invisible;
  props["MaxConvexPieces"].m_Visibility = plPropertyUiState::Invisible;
  props["Surfaces"].m_Visibility = plPropertyUiState::Invisible;
  props["SimplifyMesh"].m_Visibility = plPropertyUiState::Invisible;
  props["MeshSimplification"].m_Visibility = plPropertyUiState::Invisible;
  props["MaxSimplificationError"].m_Visibility = plPropertyUiState::Invisible;
  props["AggressiveSimplification"].m_Visibility = plPropertyUiState::Invisible;
  props["Surface"].m_Visibility = plPropertyUiState::Invisible;

  if (!isConvex)
  {
    props["MeshFile"].m_Visibility = plPropertyUiState::Default;
    props["MeshIncludeTags"].m_Visibility = plPropertyUiState::Default;
    props["MeshExcludeTags"].m_Visibility = plPropertyUiState::Default;
    props["Surfaces"].m_Visibility = plPropertyUiState::Default;
    props["SimplifyMesh"].m_Visibility = plPropertyUiState::Default;

    props["MeshSimplification"].m_Visibility = bSimplify ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["MaxSimplificationError"].m_Visibility = bSimplify ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["AggressiveSimplification"].m_Visibility = bSimplify ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    const plInt64 importTransform = e.m_pObject->GetTypeAccessor().GetValue("ImportTransform").ConvertTo<plInt64>();
    const bool bCustomTransform = importTransform == 127;
    props["RightDir"].m_Visibility = bCustomTransform ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["UpDir"].m_Visibility = bCustomTransform ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["FlipForwardDir"].m_Visibility = bCustomTransform ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else
  {
    props["ConvexMeshType"].m_Visibility = plPropertyUiState::Default;

    switch (meshType)
    {
      case plJoltConvexCollisionMeshType::ConvexHull:
        props["MeshFile"].m_Visibility = plPropertyUiState::Default;
        props["MeshIncludeTags"].m_Visibility = plPropertyUiState::Default;
        props["MeshExcludeTags"].m_Visibility = plPropertyUiState::Default;
        props["Surface"].m_Visibility = plPropertyUiState::Default;
        break;

      case plJoltConvexCollisionMeshType::ConvexDecomposition:
        props["MeshFile"].m_Visibility = plPropertyUiState::Default;
        props["MeshIncludeTags"].m_Visibility = plPropertyUiState::Default;
        props["MeshExcludeTags"].m_Visibility = plPropertyUiState::Default;
        props["Surface"].m_Visibility = plPropertyUiState::Default;
        props["MaxConvexPieces"].m_Visibility = plPropertyUiState::Default;
        break;

      case plJoltConvexCollisionMeshType::ConvexHullGroup:
        props["MeshFile"].m_Visibility = plPropertyUiState::Default;
        props["MeshIncludeTags"].m_Visibility = plPropertyUiState::Default;
        props["MeshExcludeTags"].m_Visibility = plPropertyUiState::Default;
        props["Surfaces"].m_Visibility = plPropertyUiState::Default;
        break;

      case plJoltConvexCollisionMeshType::Cylinder:
        props["Radius"].m_Visibility = plPropertyUiState::Default;
        props["Radius2"].m_Visibility = plPropertyUiState::Default;
        props["Height"].m_Visibility = plPropertyUiState::Default;
        props["Detail"].m_Visibility = plPropertyUiState::Default;
        props["Surface"].m_Visibility = plPropertyUiState::Default;
        break;
    }
  }
}

//////////////////////////////////////////////////////////////////////////

#include <Foundation/Serialization/GraphPatch.h>

class plJoltCollisionMeshAssetProperties_2_3 : public plGraphPatch
{
public:
  plJoltCollisionMeshAssetProperties_2_3()
    : plGraphPatch("plJoltCollisionMeshAssetProperties", 3)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    pNode->AddProperty("ImportTransform", 127);
  }
};

plJoltCollisionMeshAssetProperties_2_3 g_plJoltCollisionMeshAssetProperties_2_3;
