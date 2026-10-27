#include <EditorPluginJolt/EditorPluginJoltPCH.h>

#include <EditorPluginJolt/Dialogs/JoltNamePrompt.h>

namespace
{
  /// The name as it is stored and compared: without the spaces around it.
  plString ToName(plStringView sText)
  {
    plStringBuilder sName = sText;
    sName.Trim();
    return sName;
  }

  /// An empty name keeps OK disabled without a message (the prompt reads the field once more on OK, so Enter right
  /// after clearing the field keeps the prompt open too); other names go to the validator.
  plJoltNamePrompt::Validator RefuseEmptyName(plJoltNamePrompt::Validator validator)
  {
    return [validator](plStringView sText)
    {
      if (ToName(sText).IsEmpty())
        return plEnginePrompt::QuietInvalid();

      return validator ? validator(sText) : plString();
    };
  }

  /// True when an entry other than uiIgnoreKey has the name, ignoring case.
  template <typename Entries>
  bool IsNameTaken(const Entries& entries, plUInt8 uiIgnoreKey, plStringView sName)
  {
    for (plUInt32 i = 0; i < entries.GetCount(); ++i)
    {
      if (entries.GetKey(i) != uiIgnoreKey && sName.IsEqual_NoCase(entries.GetValue(i).m_sName.GetView()))
        return true;
    }

    return false;
  }
} // namespace

plString plJoltNamePrompt::Ask(plStringView sTitle, plStringView sName, Validator validator)
{
  const plString sResult = plEnginePrompt::GetText(sTitle, "Name:", sName, nullptr, RefuseEmptyName(validator));

  // an empty name does nothing, like Cancel
  return ToName(sResult);
}

plJoltNamePrompt::Validator plJoltNamePrompt::LayerValidator(const plCollisionFilterConfig& layers, plUInt32 uiIgnoreGroup, const char* szMessage)
{
  return [&layers, uiIgnoreGroup, szMessage](plStringView sText)
  {
    // the prompt refuses an empty name without a message
    const plString sName = ToName(sText);
    if (sName.IsEmpty())
      return plString();

    for (plUInt32 i = 0; i < layers.GetNumNamedGroups(); ++i)
    {
      const plUInt32 uiGroup = layers.GetNamedGroupIndex(i);
      if (uiGroup != uiIgnoreGroup && sName.IsEqual_NoCase(layers.GetGroupName(uiGroup)))
        return plString(szMessage);
    }

    return plString();
  };
}

plJoltNamePrompt::Validator plJoltNamePrompt::WeightValidator(const plWeightCategoryConfig& weights, plUInt8 uiIgnoreKey)
{
  return [&weights, uiIgnoreKey](plStringView sText)
  {
    const plString sName = ToName(sText);
    if (!sName.IsEmpty() && IsNameTaken(weights.m_Categories, uiIgnoreKey, sName))
      return plString("A weight category with that name already exists.");

    return plString();
  };
}

plJoltNamePrompt::Validator plJoltNamePrompt::ImpulseValidator(const plImpulseTypeConfig& impulses, plUInt8 uiIgnoreKey)
{
  return [&impulses, uiIgnoreKey](plStringView sText)
  {
    const plString sName = ToName(sText);
    if (!sName.IsEmpty() && IsNameTaken(impulses.m_Types, uiIgnoreKey, sName))
      return plString("An impulse type with that name already exists.");

    return plString();
  };
}
