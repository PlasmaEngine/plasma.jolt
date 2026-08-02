#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <GameEngine/Destruction/VoronoiFracture.h>
#include <RendererCore/Declarations.h>

/// \brief One material slot of the destructible mesh, mapped from the imported source materials.
struct plDestructibleMaterialSlot
{
  plString m_sLabel;
  plString m_sResource;
  float m_fToughness = 1.0f; ///< Bond toughness multiplier for chunks carrying this material (weak drywall shell over strong studs).
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plDestructibleMaterialSlot);

/// \brief Editor-side properties of the 'Destructible Mesh' asset.
class plDestructibleMeshAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plDestructibleMeshAssetProperties, plReflectedClass);

public:
  plDestructibleMeshAssetProperties();
  ~plDestructibleMeshAssetProperties();

  plString m_sMeshFile;

  /// If non-empty, only meshes/nodes whose name starts or ends with this string are fractured.
  /// Used by compound-breakable assemblies to bake one tagged part (e.g. "BRK_SidePanels") of a
  /// multi-part file into this asset.
  plString m_sPartFilter;

  // import transform (same options as the collision mesh asset)
  plEnum<plMeshImportTransform> m_ImportTransform;
  plEnum<plBasisAxis> m_RightDir = plBasisAxis::NegativeX;
  plEnum<plBasisAxis> m_UpDir = plBasisAxis::PositiveY;
  bool m_bFlipForwardDir = true;
  float m_fUniformScaling = 1.0f;

  // fracture settings
  plUInt16 m_uiChunkCount = 16;
  plUInt32 m_uiRandomSeed = 0;
  plEnum<plDestructibleFracturePattern> m_FracturePattern;
  bool m_bSealOpenSurfaces = true;
  plString m_sInteriorMaterial;
  float m_fInteriorUVScale = 1.0f;
  plEnum<plDestructibleAnchorMode> m_AnchorMode;
  float m_fBondAreaMin = 0.0f;

  /// Additional fracture levels: each entry sub-fractures every chunk of the previous level into that many pieces.
  plHybridArray<plUInt16, 2> m_SubdivisionLevels;
  /// Accumulated damage at which a chunk splits into its children (0 = never). Only relevant with subdivision levels.
  float m_fSplitDamageThreshold = 2.0f;

  /// One slot per imported source material; unassigned slots fall back to the default material.
  plHybridArray<plDestructibleMaterialSlot, 8> m_Slots;

  // statistics, updated during transform
  plUInt32 m_uiGeneratedChunks = 0;
  plUInt32 m_uiGeneratedBonds = 0;
};
