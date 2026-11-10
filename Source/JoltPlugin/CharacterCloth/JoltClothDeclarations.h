#pragma once

#include <Foundation/Reflection/Reflection.h>
#include <JoltPlugin/JoltPluginDLL.h>

/// \brief What a character cloth garment collides with besides its own body colliders.
struct PL_JOLTPLUGIN_DLL plJoltClothWorldCollision
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,   ///< Only the body colliders of its character.
    Static, ///< Also static geometry.
    All,    ///< Also static geometry and dynamic bodies.

    Default = None
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltClothWorldCollision);
