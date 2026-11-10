#pragma once

#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

/// \brief The ragdoll profile's window: its properties, which update the profile in the engine process as they change, so
/// ragdolls that use it pick up the edit while the scene is simulating.
///
/// Picking a preset writes its values into the asset; editing any value afterwards marks the profile Custom.
class plJoltRagdollProfileAssetDocumentWindow : public plDocumentWindow
{
public:
  plJoltRagdollProfileAssetDocumentWindow(plDocument* pDocument);
  ~plJoltRagdollProfileAssetDocumentWindow();

private:
  void UpdatePreview();
  void RestoreResource();

  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void CommandHistoryEventHandler(const plCommandHistoryEvent& e);

  bool m_bApplyingPreset = false;
  bool m_bPresetPicked = false; ///< apply the picked preset once the edit's transaction has ended
  bool m_bValueEdited = false;  ///< mark the profile Custom once the edit's transaction has ended
};
