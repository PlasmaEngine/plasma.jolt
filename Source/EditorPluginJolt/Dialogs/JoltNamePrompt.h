#pragma once

#include <EditorPluginJolt/EditorPluginJoltDLL.h>
#include <GameEngine/Physics/CollisionFilter.h>
#include <GameEngine/Physics/ImpulseType.h>
#include <GameEngine/Physics/WeightCategory.h>
#include <GuiFoundation/EngineUi/EnginePrompt.h>

/// \brief The "Name:" prompt of Add and Rename in the Jolt Project Settings.
///
/// It is the engine text prompt: a name that is taken shows its message under the field and OK stays disabled while
/// the name is taken or empty. Names are compared without case and without the spaces around them.
class plJoltNamePrompt
{
public:
  using Validator = plEnginePrompt::Validator;

  /// Asks for a name, starting with sName. Returns the name without the spaces around it, or an empty string after
  /// Cancel or for a name of only spaces.
  static plString Ask(plStringView sTitle, plStringView sName, Validator validator);

  /// Refuses the name of another collision layer with szMessage; uiIgnoreGroup is the layer being renamed,
  /// plInvalidIndex when adding.
  static Validator LayerValidator(const plCollisionFilterConfig& layers, plUInt32 uiIgnoreGroup, const char* szMessage);

  /// Refuses the name of another weight category; uiIgnoreKey is the category being renamed.
  static Validator WeightValidator(const plWeightCategoryConfig& weights, plUInt8 uiIgnoreKey = plWeightCategoryConfig::InvalidKey);

  /// Refuses the name of another impulse type; uiIgnoreKey is the type being renamed.
  static Validator ImpulseValidator(const plImpulseTypeConfig& impulses, plUInt8 uiIgnoreKey = plImpulseTypeConfig::InvalidKey);
};
