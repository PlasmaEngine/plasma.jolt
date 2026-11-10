#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorFramework/Manipulators/TransformManipulatorAdapter.h>
#include <JoltPlugin/CharacterCloth/JoltClothDescriptor.h>

struct plPropertyMetaStateEvent;

/// \brief Named starting points for a fabric. Custom leaves the values as they are.
struct plJoltClothFabricPreset
{
  using StorageType = plUInt8;

  enum Enum
  {
    Custom,
    Silk,
    Cotton,
    Wool,
    Denim,
    Leather,
    Chainmail,

    Default = Cotton
  };

  /// \brief The values of a preset. Returns false for Custom.
  static bool GetFabric(Enum preset, plJoltClothFabric& out_fabric);
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothFabricPreset);

/// \brief Which body shapes a garment collides with.
struct plJoltClothColliderSource
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,           ///< Only the shapes listed in the asset.
    SkeletonShapes, ///< The shapes of the skeleton that ragdolls and hitboxes use, plus the ones listed in the asset.

    Default = SkeletonShapes
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothColliderSource);

struct plJoltClothColliderShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sphere,
    Capsule,
    Box,

    Default = Capsule
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothColliderShape);

/// \brief A cloth-only collider added in the asset, where the skeleton has no shape.
struct plJoltClothAssetCollider
{
  plString m_sJoint;
  plEnum<plJoltClothColliderShape> m_Shape;
  plVec3 m_vOffset = plVec3::MakeZero();
  plQuat m_qRotation = plQuat::MakeIdentity();
  float m_fRadius = 0.08f;
  float m_fLength = 0.3f;
  plVec3 m_vHalfExtents = plVec3(0.1f);
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plJoltClothAssetCollider);

/// \brief The move and turn handles of a cloth collider. Like the transform manipulator, but without snapping to
/// the editor's grid: colliders are placed by centimetres.
class plJoltClothColliderHandleAttribute : public plTransformManipulatorAttribute
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothColliderHandleAttribute, plTransformManipulatorAttribute);

public:
  plJoltClothColliderHandleAttribute() = default;
  plJoltClothColliderHandleAttribute(const char* szTranslateProperty, const char* szRotateProperty = nullptr, const char* szOffsetTranslation = nullptr)
    : plTransformManipulatorAttribute(szTranslateProperty, szRotateProperty, nullptr, szOffsetTranslation)
  {
  }
};

class plJoltClothColliderHandleAdapter : public plTransformManipulatorAdapter
{
protected:
  virtual void Finalize() override;
};

class plJoltClothAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plJoltClothAssetProperties, plReflectedClass);

public:
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  // Source
  plString m_sMesh;
  plHybridArray<plString, 2> m_Garments; ///< names of the garment pieces to simulate; empty takes every piece of the mesh

  // Sim mesh
  float m_fWeldDistance = 0.0001f;

  // Maps
  bool m_bUseImportedMasks = true;
  float m_fMaxDistance = 0.3f;
  float m_fBackstopDistance = 0.02f;
  float m_fBackstopRadius = 0.4f;
  plDataBuffer m_PaintedMaps; ///< a serialized plJoltClothPaintedMaps; empty while nothing was painted

  // Fabric
  plEnum<plJoltClothFabricPreset> m_FabricPreset;
  float m_fDensity = 0.15f;
  float m_fStretch = 0.95f;
  float m_fShear = 0.9f;
  float m_fBend = 0.2f;
  float m_fDamping = 0.2f;
  float m_fDrag = 0.7f;
  float m_fLift = 0.2f;
  float m_fFriction = 0.2f;
  float m_fThickness = 0.01f;

  // Simulation
  float m_fStepRate = 60.0f;
  plUInt8 m_uiIterations = 8;
  bool m_bTethers = true;
  float m_fTetherSlack = 1.03f;
  float m_fStretchLimit = 0.08f;
  plEnum<plJoltClothWorldCollision> m_WorldCollision;
  float m_fGravityFactor = 1.0f;
  float m_fFullRateDistance = 14.0f;
  float m_fSimulateDistance = 30.0f;

  // Colliders
  plEnum<plJoltClothColliderSource> m_ColliderSource;
  plHybridArray<plString, 4> m_SkeletonShapeJoints; ///< joints whose skeleton shapes collide; empty takes every shape in reach
  float m_fColliderInflate = 0.0f;
  plDynamicArray<plJoltClothAssetCollider> m_Colliders;

  // Preview, not part of the cooked cloth
  plString m_sPreviewClip;
  plHybridArray<plString, 4> m_StressClips; ///< clips the stress test plays after its motion script, next to the preview clip

  /// \brief The fabric the cook uses: the preset's values, or the custom ones.
  plJoltClothFabric GetFabric() const;
};
