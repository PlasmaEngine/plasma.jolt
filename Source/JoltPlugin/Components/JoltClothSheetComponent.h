#pragma once

#include <JoltPlugin/JoltPluginDLL.h>

#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Pipeline/RenderData.h>

using plMaterialResourceHandle = plTypedResourceHandle<class plMaterialResource>;
using plDynamicMeshBufferResourceHandle = plTypedResourceHandle<class plDynamicMeshBufferResource>;

//////////////////////////////////////////////////////////////////////////

class PL_JOLTPLUGIN_DLL plJoltClothSheetComponentManager : public plComponentManager<class plJoltClothSheetComponent, plBlockStorageType::FreeList>
{
public:
  plJoltClothSheetComponentManager(plWorld* pWorld);
  ~plJoltClothSheetComponentManager();

  virtual void Initialize() override;

private:
  plUInt64 m_uiLastJoltUpdateCounter = 0;
  void UpdatePreAsync(const plWorldModule::UpdateContext& context);
  void UpdatePostAsync(const plWorldModule::UpdateContext& context);
};

//////////////////////////////////////////////////////////////////////////

/// \brief Flags for how a piece of cloth should be simulated.
struct PL_JOLTPLUGIN_DLL plJoltClothSheetFlags
{
  using StorageType = plUInt16;

  enum Enum
  {
    FixedCornerTopLeft = PL_BIT(0),     ///< This corner can't move.
    FixedCornerTopRight = PL_BIT(1),    ///< This corner can't move.
    FixedCornerBottomRight = PL_BIT(2), ///< This corner can't move.
    FixedCornerBottomLeft = PL_BIT(3),  ///< This corner can't move.
    FixedEdgeTop = PL_BIT(4),           ///< This entire edge can't move.
    FixedEdgeRight = PL_BIT(5),         ///< This entire edge can't move.
    FixedEdgeBottom = PL_BIT(6),        ///< This entire edge can't move.
    FixedEdgeLeft = PL_BIT(7),          ///< This entire edge can't move.

    Default = FixedEdgeTop
  };

  struct Bits
  {
    StorageType FixedCornerTopLeft : 1;
    StorageType FixedCornerTopRight : 1;
    StorageType FixedCornerBottomRight : 1;
    StorageType FixedCornerBottomLeft : 1;
    StorageType FixedEdgeTop : 1;
    StorageType FixedEdgeRight : 1;
    StorageType FixedEdgeBottom : 1;
    StorageType FixedEdgeLeft : 1;
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltClothSheetFlags);

/// \brief Simulates a rectangular piece of cloth.
///
/// The cloth doesn't interact with the environment and doesn't collide with any geometry.
/// The component samples the wind simulation and applies wind forces to the cloth.
///
/// Cloth sheets can be used as decorative elements like flags that blow in the wind.
class PL_JOLTPLUGIN_DLL plJoltClothSheetComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plJoltClothSheetComponent, plRenderComponent, plJoltClothSheetComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnSimulationStarted() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plRenderComponent

public:
  virtual plResult GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg) override;

private:
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

  //////////////////////////////////////////////////////////////////////////
  // plJoltClothSheetComponent

public:
  plJoltClothSheetComponent();
  ~plJoltClothSheetComponent();

  /// Sets the world-space size of the cloth.
  void SetSize(plVec2 vVal);                 // [ property ]
  plVec2 GetSize() const { return m_vSize; } // [ property ]

  /// Sets of how many pieces the cloth is made up.
  ///
  /// More pieces cost more performance to simulate the cloth.
  /// A size of 32x32 is already quite performance intensive. Use as few segments as possible.
  /// For many cases 8x8 or 12x12 should already be good enough.
  /// Also the more segments there are, the more the cloth will sag.
  void SetSegments(plVec2U32 vVal);                     // [ property ]
  plVec2U32 GetSegments() const { return m_vNumVertices; } // [ property ]

  /// The collision layer determines with which other actors this actor collides. \see plJoltActorComponent
  plUInt8 m_uiCollisionLayer = 0; // [ property ]

  /// \brief Adjusts how strongly gravity affects the soft body.
  float m_fGravityFactor = 1.0f; // [ property ]

  /// A factor to tweak how strong the wind can push the cloth.
  float m_fWindInfluence = 0.3f; // [ property ]

  /// Damping slows down cloth movement over time. Higher values make it stop sooner and also improve performance.
  float m_fDamping = 0.5f; // [ property ]

  /// How thick the cloth is, to prevent it from intersecting with other geometry.
  float m_fThickness = 0.05f; // [ property ]

  /// Tint color for the cloth material.
  plColor m_Color = plColor::White; // [ property ]

  /// Sets where the cloth is attached to the world.
  void SetFlags(plBitflags<plJoltClothSheetFlags> flags);                // [ property ]
  plBitflags<plJoltClothSheetFlags> GetFlags() const { return m_Flags; } // [ property ]

  plMaterialResourceHandle m_hMaterial;                                  // [ property ]

private:
  void UpdatePreAsync();
  void UpdatePostAsync();

  void ApplyWind();

  void SetupCloth();
  void RemoveBody();

  plVec2 m_vSize = plVec2(1.0f, 1.0f);
  plVec2 m_vTextureScale = plVec2(1.0f);
  plVec2U32 m_vNumVertices = plVec2U32(16, 16);
  plBitflags<plJoltClothSheetFlags> m_Flags;
  mutable plRenderData::Category m_RenderDataCategory;
  plUInt8 m_uiSleepCounter = 0;
  plUInt32 m_uiObjectFilterID = plInvalidIndex;
  plUInt32 m_uiUserDataIndex = plInvalidIndex;
  plUInt32 m_uiJoltBodyID = plInvalidIndex;

  plBoundingSphere m_BSphere;
  plTransform m_BodyGlobalTransform = plTransform::MakeIdentity();

  mutable plInstanceDataOffset m_InstanceDataOffset;

  plDynamicMeshBufferResourceHandle m_hDynamicMeshBuffer;
};
