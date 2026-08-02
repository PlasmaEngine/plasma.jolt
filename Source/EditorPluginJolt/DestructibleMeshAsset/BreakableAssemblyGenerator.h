#pragma once

#include <EditorFramework/Assets/AssetDocumentGenerator.h>

/// \brief Imports a part-tagged model file (BRK_/STA_/GLS_/SWP_/RVL_/ANC_ object-name prefixes,
/// see plModelImporter2::PartConventions) as a COMPOUND BREAKABLE:
///
/// - one Destructible Mesh asset per BRK_ part (pattern/chunks from pl_* custom properties),
/// - one Mesh (+ triangle collision) asset per STA_/SWP_/RVL_ part,
/// - one assembly prefab wiring everything together: destructible components, static actors,
///   breakable glass slabs fitted to GLS_ part bounds, broken-prefab swaps, and a destruction-reveal
///   group for RVL_ parts.
///
/// Authoring workflow: Documentation/Destruction/Guide_CompoundBreakables.html
class plBreakableAssemblyDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plBreakableAssemblyDocumentGenerator, plAssetDocumentGenerator);

public:
  plBreakableAssemblyDocumentGenerator();
  ~plBreakableAssemblyDocumentGenerator();

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plPrefab"; }
  virtual plStringView GetGeneratorGroup() const override { return "Meshes"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};
