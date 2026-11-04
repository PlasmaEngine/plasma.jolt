#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAsset.h>
#include <EditorPluginJolt/RagdollProfileAsset/JoltRagdollProfileAssetWindow.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>

plJoltRagdollProfileAssetDocumentWindow::plJoltRagdollProfileAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetCommandHistory()->m_Events.AddEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::CommandHistoryEventHandler, this));

  SetMenuBarActions("JoltRagdollProfileAssetMenuBar");
  SetToolBarActions("JoltRagdollProfileAssetToolBar");

  // the property grid is the whole document
  AddPropertiesPanel({"JoltRagdollProfileAssetDockWidget", "Ragdoll Profile"});
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
}

plJoltRagdollProfileAssetDocumentWindow::~plJoltRagdollProfileAssetDocumentWindow()
{
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetCommandHistory()->m_Events.RemoveEventHandler(plMakeDelegate(&plJoltRagdollProfileAssetDocumentWindow::CommandHistoryEventHandler, this));

  RestoreResource();
}

void plJoltRagdollProfileAssetDocumentWindow::UpdatePreview()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  auto pDocument = static_cast<plJoltRagdollProfileAssetDocument*>(GetDocument());

  plResourceUpdateMsgToEngine msg;
  msg.m_sResourceType = "Jolt_RagdollProfile";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plContiguousMemoryStreamStorage streamStorage;
  plMemoryStreamWriter memoryWriter(&streamStorage);

  plStringBuilder sAbsFilePath = GetDocument()->GetDocumentPath();
  sAbsFilePath.ChangeFileExtension("plJoltRagdollProfile");
  memoryWriter << sAbsFilePath;

  const plUInt64 uiHash = plAssetCurator::GetSingleton()->GetAssetTransformHash(GetDocument()->GetGuid());
  plAssetFileHeader AssetHeader;
  AssetHeader.SetFileHashAndVersion(uiHash, pDocument->GetAssetTypeVersion());
  AssetHeader.Write(memoryWriter).IgnoreResult();

  if (pDocument->WriteAsset(memoryWriter, plAssetCurator::GetSingleton()->GetActiveAssetProfile()).Succeeded())
  {
    msg.m_Data = plArrayPtr<const plUInt8>(streamStorage.GetData(), streamStorage.GetStorageSize32());
    plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
  }
}

void plJoltRagdollProfileAssetDocumentWindow::RestoreResource()
{
  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = "Jolt_RagdollProfile";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}

void plJoltRagdollProfileAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  UpdatePreview();

  if (m_bApplyingPreset || GetDocument()->GetCommandHistory()->IsInUndoRedo())
    return;

  // the document can't be edited from inside its own edit, so both reactions wait for the transaction to end
  if (e.m_sProperty == "Preset")
    m_bPresetPicked = e.m_NewValue.ConvertTo<plInt64>() != plJoltRagdollPreset::Custom;
  else
    m_bValueEdited = true;
}

void plJoltRagdollProfileAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  UpdatePreview();
}

void plJoltRagdollProfileAssetDocumentWindow::CommandHistoryEventHandler(const plCommandHistoryEvent& e)
{
  if (e.m_Type != plCommandHistoryEvent::Type::TransactionEnded || m_bApplyingPreset)
    return;

  auto pDocument = static_cast<plJoltRagdollProfileAssetDocument*>(GetDocument());
  const plDocumentObject* pRoot = pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0];

  m_bApplyingPreset = true;

  if (m_bPresetPicked)
  {
    const auto preset = static_cast<plJoltRagdollPreset::Enum>(pDocument->GetProperties()->m_Preset.GetValue());
    pDocument->ApplyPreset(preset);
  }
  else if (m_bValueEdited && pDocument->GetProperties()->m_Preset != plJoltRagdollPreset::Custom)
  {
    // an edited value means the profile no longer is the preset it started from
    plObjectAccessorBase* pAccessor = pDocument->GetObjectAccessor();
    pAccessor->StartTransaction("Ragdoll Profile is Custom");
    pAccessor->SetValueByName(pRoot, "Preset", (plInt64)plJoltRagdollPreset::Custom).IgnoreResult();
    pAccessor->FinishTransaction();
  }

  m_bApplyingPreset = false;
  m_bPresetPicked = false;
  m_bValueEdited = false;
}
