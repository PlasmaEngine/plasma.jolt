#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/DestructibleMeshAsset/DestructibleMeshAssetObjects.h>
#include <Foundation/Serialization/GraphPatch.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plDestructibleMaterialSlot, plNoBase, 2, plRTTIDefaultAllocator<plDestructibleMaterialSlot>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Label", m_sLabel)->AddAttributes(new plReadOnlyAttribute()),
    PL_MEMBER_PROPERTY("Resource", m_sResource)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material", plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("Toughness", m_fToughness)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.05f, 100.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plDestructibleMeshAssetProperties, 4, plRTTIDefaultAllocator<plDestructibleMeshAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("MeshFile", m_sMeshFile)->AddAttributes(new plFileBrowserAttribute("Select Mesh", plFileBrowserAttribute::Meshes)),
    PL_MEMBER_PROPERTY("PartFilter", m_sPartFilter),
    PL_ENUM_MEMBER_PROPERTY("ImportTransform", plMeshImportTransform, m_ImportTransform),
    PL_ENUM_MEMBER_PROPERTY("RightDir", plBasisAxis, m_RightDir)->AddAttributes(new plDefaultValueAttribute((int)plBasisAxis::NegativeX)),
    PL_ENUM_MEMBER_PROPERTY("UpDir", plBasisAxis, m_UpDir)->AddAttributes(new plDefaultValueAttribute((int)plBasisAxis::PositiveY)),
    PL_MEMBER_PROPERTY("FlipForwardDir", m_bFlipForwardDir)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UniformScaling", m_fUniformScaling)->AddAttributes(new plDefaultValueAttribute(1.0f)),

    PL_MEMBER_PROPERTY("ChunkCount", m_uiChunkCount)->AddAttributes(new plDefaultValueAttribute(16), new plClampValueAttribute(2, 256)),
    PL_MEMBER_PROPERTY("RandomSeed", m_uiRandomSeed),
    PL_ENUM_MEMBER_PROPERTY("FracturePattern", plDestructibleFracturePattern, m_FracturePattern),
    PL_MEMBER_PROPERTY("SealOpenSurfaces", m_bSealOpenSurfaces)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("InteriorMaterial", m_sInteriorMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material", plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("InteriorUVScale", m_fInteriorUVScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.001f, 1000.0f)),
    PL_ENUM_MEMBER_PROPERTY("AnchorMode", plDestructibleAnchorMode, m_AnchorMode),
    PL_MEMBER_PROPERTY("BondAreaMin", m_fBondAreaMin)->AddAttributes(new plClampValueAttribute(0.0f, plVariant())),
    PL_ARRAY_MEMBER_PROPERTY("SubdivisionLevels", m_SubdivisionLevels)->AddAttributes(new plMaxArraySizeAttribute(2)),
    PL_MEMBER_PROPERTY("SplitDamageThreshold", m_fSplitDamageThreshold)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_ARRAY_MEMBER_PROPERTY("Materials", m_Slots)->AddAttributes(new plContainerAttribute(false, false, false)),

    PL_MEMBER_PROPERTY("GeneratedChunks", m_uiGeneratedChunks)->AddAttributes(new plReadOnlyAttribute()),
    PL_MEMBER_PROPERTY("GeneratedBonds", m_uiGeneratedBonds)->AddAttributes(new plReadOnlyAttribute()),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plDestructibleMeshAssetProperties::plDestructibleMeshAssetProperties() = default;
plDestructibleMeshAssetProperties::~plDestructibleMeshAssetProperties() = default;

//////////////////////////////////////////////////////////////////////////

class plDestructibleMeshAssetPropertiesPatch_2_3 : public plGraphPatch
{
public:
  plDestructibleMeshAssetPropertiesPatch_2_3()
    : plGraphPatch("plDestructibleMeshAssetProperties", 3)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    // Existing destructible assets predate the repair toggle. Explicitly enable it on upgrade;
    // relying on a reflected default would leave some serialized documents at false.
    pNode->AddProperty("SealOpenSurfaces", true);
  }
};

plDestructibleMeshAssetPropertiesPatch_2_3 g_plDestructibleMeshAssetPropertiesPatch_2_3;
