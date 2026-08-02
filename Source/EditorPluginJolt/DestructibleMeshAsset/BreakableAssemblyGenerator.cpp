#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginJolt/DestructibleMeshAsset/BreakableAssemblyGenerator.h>
#include <Foundation/IO/OSFile.h>
#include <GameEngine/Destruction/VoronoiFracture.h>
#include <ModelImporter2/ModelImporter.h>
#include <ModelImporter2/ModelPartConventions.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBreakableAssemblyDocumentGenerator, 1, plRTTIDefaultAllocator<plBreakableAssemblyDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;

using namespace plModelImporter2;

plBreakableAssemblyDocumentGenerator::plBreakableAssemblyDocumentGenerator()
{
  AddSupportedFileType("fbx");
  AddSupportedFileType("gltf");
  AddSupportedFileType("glb");
}

plBreakableAssemblyDocumentGenerator::~plBreakableAssemblyDocumentGenerator() = default;

void plBreakableAssemblyDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
  info.m_Priority = plAssetDocGeneratorPriority::LowPriority;
  info.m_sName = "BreakableAssembly";
  info.m_sIcon = ":/AssetIcons/DestructibleMesh.svg";
}

namespace
{
  struct AssemblyPart
  {
    PartRole::Enum m_Role = PartRole::None;
    plString m_sNodeName; ///< Full DCC name incl. prefix - used as import filter.
    plString m_sBaseName; ///< Prefix and duplicate-suffix stripped - used for asset/object names.
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid();
    plVariantDictionary m_Properties;
  };

  struct PropertyValue
  {
    const char* m_sName;
    plVariant m_Value;
  };

  plInt64 PatternFromString(const plString& sPattern)
  {
    if (sPattern.IsEqual_NoCase("Glass"))
      return plDestructibleFracturePattern::Glass;
    if (sPattern.IsEqual_NoCase("Concrete"))
      return plDestructibleFracturePattern::Concrete;
    if (sPattern.IsEqual_NoCase("Wood"))
      return plDestructibleFracturePattern::Wood;
    if (sPattern.IsEqual_NoCase("Tiles"))
      return plDestructibleFracturePattern::Tiles;

    return plDestructibleFracturePattern::Uniform;
  }

  plString GuidToString(const plUuid& guid)
  {
    plStringBuilder sb;
    plConversionUtils::ToString(guid, sb);
    return sb;
  }

  /// Creates an asset document, applies properties, and appends it to the output list.
  /// Returns the document guid as a string, or an empty string on failure.
  plString CreateAssetDocument(plStringView sOutFile, plArrayPtr<const PropertyValue> properties, plDynamicArray<plDocument*>& out_generatedDocuments)
  {
    if (plOSFile::ExistsFile(sOutFile))
    {
      // re-import: the document already exists; keep the user's version untouched and just reference it
      plAssetCurator::plLockedSubAsset asset = plAssetCurator::GetSingleton()->FindSubAsset(sOutFile);
      if (asset.isValid())
      {
        return GuidToString(asset->m_Data.m_Guid);
      }

      plLog::Warning("Assembly part document exists but is unknown to the asset curator: '{}'", sOutFile);
      return {};
    }

    plDocument* pDoc = plQtEditorApp::GetSingleton()->CreateDocument(sOutFile, plDocumentFlags::None);
    if (pDoc == nullptr)
    {
      plLog::Error("Could not create assembly part document '{}'", sOutFile);
      return {};
    }

    out_generatedDocuments.PushBack(pDoc);

    // the asset's properties object is the first child of the document root (plSimpleAssetDocument layout)
    const auto& rootChildren = pDoc->GetObjectManager()->GetRootObject()->GetChildren();
    if (rootChildren.IsEmpty())
    {
      plLog::Error("Generated document '{}' has no properties object.", sOutFile);
      return {};
    }

    auto& accessor = rootChildren[0]->GetTypeAccessor();

    for (const PropertyValue& prop : properties)
    {
      if (!accessor.SetValue(prop.m_sName, prop.m_Value))
      {
        plLog::Warning("Could not set property '{}' on '{}'", prop.m_sName, sOutFile);
      }
    }

    return GuidToString(pDoc->GetGuid());
  }
} // namespace

plStatus plBreakableAssemblyDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  plStringBuilder sInputFileRel = sInputFileAbs;
  plQtEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sInputFileRel);

  // ---- scan the file's node hierarchy for tagged parts
  plDynamicArray<AssemblyPart> parts;
  {
    plUniquePtr<Importer> pImporter = RequestImporterForFileType(sInputFileAbs);
    if (pImporter == nullptr)
      return plStatus("No known importer for this file type.");

    plMeshResourceDescriptor throwaway;
    ImportOptions opt;
    opt.m_sSourceFile = sInputFileAbs;
    opt.m_pMeshOutput = &throwaway;

    if (pImporter->Import(opt).Failed())
      return plStatus("Model importer was unable to read this file.");

    for (const auto& node : pImporter->m_OutputNodes)
    {
      plStringBuilder sBaseName;
      const PartRole::Enum role = PartConventions::ClassifyName(node.m_sName, &sBaseName);

      if (role == PartRole::None || role == PartRole::Anchor || node.m_MeshNames.IsEmpty())
        continue; // anchors are consumed by the destructible bake itself; untagged nodes are ignored

      AssemblyPart& part = parts.ExpandAndGetRef();
      part.m_Role = role;
      part.m_sNodeName = node.m_sName;
      part.m_sBaseName = sBaseName;
      part.m_Bounds = node.m_MeshBounds;
      part.m_Properties = node.m_CustomProperties;
    }
  }

  if (parts.IsEmpty())
  {
    return plStatus("The file contains no part-tagged objects (BRK_/STA_/GLS_/SWP_/RVL_ name prefixes). See Documentation/Destruction/Guide_CompoundBreakables.html.");
  }

  const plStringView sBaseFile = sInputFileAbs.GetFileName();
  plStringBuilder sDirectory = sInputFileAbs;
  sDirectory = sDirectory.GetFileDirectory();

  // ---- create the per-part assets
  struct PartAsset
  {
    const AssemblyPart* m_pPart = nullptr;
    plString m_sMeshAssetGuid;      // render mesh (STA/SWP/RVL)
    plString m_sCollisionAssetGuid; // triangle collision (STA)
    plString m_sDestructibleGuid;   // BRK
  };

  plDynamicArray<PartAsset> partAssets;
  plStringBuilder sOutFile;

  for (const AssemblyPart& part : parts)
  {
    PartAsset& asset = partAssets.ExpandAndGetRef();
    asset.m_pPart = &part;

    if (part.m_Role == PartRole::Breakable)
    {
      sOutFile.SetFormat("{}{}_{}.plDestructibleMeshAsset", sDirectory, sBaseFile, part.m_sBaseName);

      plHybridArray<PropertyValue, 8> props;
      props.PushBack({"MeshFile", plVariant(sInputFileRel.GetView())});
      props.PushBack({"PartFilter", plVariant(part.m_sNodeName)});
      props.PushBack({"ChunkCount", plVariant((plInt32)PartConventions::GetFloatProperty(part.m_Properties, "pl_chunks", 16.0f))});
      props.PushBack({"FracturePattern", plVariant(PatternFromString(PartConventions::GetStringProperty(part.m_Properties, "pl_pattern", "Uniform")))});

      asset.m_sDestructibleGuid = CreateAssetDocument(sOutFile, props, out_generatedDocuments);
    }
    else if (part.m_Role == PartRole::Static || part.m_Role == PartRole::Swap || part.m_Role == PartRole::Reveal)
    {
      sOutFile.SetFormat("{}{}_{}.plMeshAsset", sDirectory, sBaseFile, part.m_sBaseName);

      plHybridArray<PropertyValue, 4> props;
      props.PushBack({"MeshFile", plVariant(sInputFileRel.GetView())});
      props.PushBack({"MeshIncludeTags", plVariant(part.m_sNodeName)});

      asset.m_sMeshAssetGuid = CreateAssetDocument(sOutFile, props, out_generatedDocuments);

      if (part.m_Role == PartRole::Static)
      {
        sOutFile.SetFormat("{}{}_{}.plJoltCollisionMeshAsset", sDirectory, sBaseFile, part.m_sBaseName);

        plHybridArray<PropertyValue, 4> colProps;
        colProps.PushBack({"MeshFile", plVariant(sInputFileRel.GetView())});
        colProps.PushBack({"MeshIncludeTags", plVariant(part.m_sNodeName)});

        asset.m_sCollisionAssetGuid = CreateAssetDocument(sOutFile, colProps, out_generatedDocuments);
      }
    }
  }

  // ---- create the assembly prefab
  sOutFile.SetFormat("{}{}_Assembly.plPrefab", sDirectory, sBaseFile);

  if (plOSFile::ExistsFile(sOutFile))
  {
    plLog::Info("Assembly prefab already exists, not regenerating: '{}'", sOutFile);
    return plStatus(PL_SUCCESS);
  }

  plDocument* pPrefabDoc = plQtEditorApp::GetSingleton()->CreateDocument(sOutFile, plDocumentFlags::None);
  if (pPrefabDoc == nullptr)
    return plStatus("Could not create the assembly prefab document.");

  out_generatedDocuments.PushBack(pPrefabDoc);

  plCommandHistory* pHistory = pPrefabDoc->GetCommandHistory();
  pHistory->StartTransaction("Generate Breakable Assembly");

  auto addObject = [&](plStringView sName, const plUuid& parent) -> plUuid {
    plAddObjectCommand cmd;
    cmd.SetType("plGameObject");
    cmd.m_NewObjectGuid = plUuid::MakeUuid();
    cmd.m_Parent = parent;
    cmd.m_sParentProperty = "Children";
    cmd.m_Index = -1;
    PL_VERIFY(pHistory->AddCommand(cmd).m_Result.Succeeded(), "AddCommand failed");

    plSetObjectPropertyCommand nameCmd;
    nameCmd.m_Object = cmd.m_NewObjectGuid;
    nameCmd.m_sProperty = "Name";
    nameCmd.m_NewValue = plString(sName);
    PL_VERIFY(pHistory->AddCommand(nameCmd).m_Result.Succeeded(), "AddCommand failed");

    return cmd.m_NewObjectGuid;
  };

  auto addComponent = [&](const plUuid& object, const char* szType) -> plUuid {
    plAddObjectCommand cmd;
    cmd.SetType(szType);
    cmd.m_NewObjectGuid = plUuid::MakeUuid();
    cmd.m_Parent = object;
    cmd.m_sParentProperty = "Components";
    cmd.m_Index = -1;

    if (pHistory->AddCommand(cmd).m_Result.Failed())
    {
      plLog::Error("Could not create component of type '{}' - is the plugin active?", szType);
      return plUuid();
    }

    return cmd.m_NewObjectGuid;
  };

  auto setProperty = [&](const plUuid& object, const char* szProperty, const plVariant& value) {
    if (!object.IsValid())
      return;

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = object;
    cmd.m_sProperty = szProperty;
    cmd.m_NewValue = value;

    if (pHistory->AddCommand(cmd).m_Result.Failed())
    {
      plLog::Warning("Could not set component property '{}'", szProperty);
    }
  };

  const plUuid rootObject = addObject(sBaseFile, plUuid());
  plUuid revealRoot; // created on demand for RVL_ parts

  for (const PartAsset& asset : partAssets)
  {
    const AssemblyPart& part = *asset.m_pPart;

    switch (part.m_Role)
    {
      case PartRole::Breakable:
      {
        const plUuid object = addObject(part.m_sBaseName, rootObject);
        const plUuid component = addComponent(object, "plJoltDestructibleComponent");
        setProperty(component, "DestructibleMesh", asset.m_sDestructibleGuid);
        setProperty(component, "Toughness", PartConventions::GetFloatProperty(part.m_Properties, "pl_toughness", 1.0f));
        break;
      }

      case PartRole::Static:
      {
        const plUuid object = addObject(part.m_sBaseName, rootObject);
        const plUuid meshComp = addComponent(object, "plMeshComponent");
        setProperty(meshComp, "Mesh", asset.m_sMeshAssetGuid);

        const plUuid actorComp = addComponent(object, "plJoltStaticActorComponent");
        setProperty(actorComp, "CollisionMesh", asset.m_sCollisionAssetGuid);
        break;
      }

      case PartRole::Glass:
      {
        // fit a breakable slab to the part's bounds: the thinnest axis becomes the pane normal
        const plUuid object = addObject(part.m_sBaseName, rootObject);

        const plVec3 vCenter = part.m_Bounds.IsValid() ? part.m_Bounds.GetCenter() : plVec3::MakeZero();
        const plVec3 vSize = part.m_Bounds.IsValid() ? part.m_Bounds.GetHalfExtents() * 2.0f : plVec3(1, 1, 0.02f);

        plUInt32 uiThinAxis = 0;
        if (vSize.y < vSize.GetData()[uiThinAxis])
          uiThinAxis = 1;
        if (vSize.z < vSize.GetData()[uiThinAxis])
          uiThinAxis = 2;

        setProperty(object, "LocalPosition", vCenter);

        plQuat qRotation = plQuat::MakeIdentity();
        if (uiThinAxis == 0)
        {
          qRotation = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisY(), plAngle::MakeFromDegree(90.0f));
        }
        else if (uiThinAxis == 1)
        {
          qRotation = plQuat::MakeFromAxisAndAngle(plVec3::MakeAxisX(), plAngle::MakeFromDegree(-90.0f));
        }
        setProperty(object, "LocalRotation", qRotation);

        const plUInt32 uiAxisA = (uiThinAxis + 1) % 3;
        const plUInt32 uiAxisB = (uiThinAxis + 2) % 3;

        const plUuid slabComp = addComponent(object, "plJoltBreakableSlabComponent");
        setProperty(slabComp, "Width", plMath::Max(0.1f, vSize.GetData()[uiAxisA]));
        setProperty(slabComp, "Height", plMath::Max(0.1f, vSize.GetData()[uiAxisB]));
        setProperty(slabComp, "Thickness", plMath::Clamp(vSize.GetData()[uiThinAxis], 0.005f, 1.0f));
        setProperty(slabComp, "Health", PartConventions::GetFloatProperty(part.m_Properties, "pl_health", 15.0f));
        setProperty(slabComp, "InstantShatterDamage", PartConventions::GetFloatProperty(part.m_Properties, "pl_instant", 0.0f));
        break;
      }

      case PartRole::Swap:
      {
        const plUuid object = addObject(part.m_sBaseName, rootObject);
        const plUuid meshComp = addComponent(object, "plMeshComponent");
        setProperty(meshComp, "Mesh", asset.m_sMeshAssetGuid);

        const plUuid swapComp = addComponent(object, "plBrokenPrefabSwapComponent");
        setProperty(swapComp, "Health", PartConventions::GetFloatProperty(part.m_Properties, "pl_health", 10.0f));

        const plString sBrokenPrefab = PartConventions::GetStringProperty(part.m_Properties, "pl_prefab", "");
        if (!sBrokenPrefab.IsEmpty())
        {
          setProperty(swapComp, "BrokenPrefab", sBrokenPrefab);
        }
        else
        {
          plLog::Warning("SWP_ part '{}' has no 'pl_prefab' custom property - assign the BrokenPrefab manually.", part.m_sNodeName);
        }
        break;
      }

      case PartRole::Reveal:
      {
        if (!revealRoot.IsValid())
        {
          revealRoot = addObject("Hidden", rootObject);
          const plUuid revealComp = addComponent(revealRoot, "plDestructionRevealComponent");
          setProperty(revealComp, "RevealMargin", PartConventions::GetFloatProperty(part.m_Properties, "pl_margin", 0.5f));
        }

        const plUuid object = addObject(part.m_sBaseName, revealRoot);
        const plUuid meshComp = addComponent(object, "plMeshComponent");
        setProperty(meshComp, "Mesh", asset.m_sMeshAssetGuid);
        break;
      }

      default:
        break;
    }
  }

  pHistory->FinishTransaction();

  plLog::Success("Imported breakable assembly: '{}' ({} parts)", sOutFile, parts.GetCount());
  return plStatus(PL_SUCCESS);
}
