#pragma once

#include <JoltPlugin/CharacterCloth/JoltClothInstance.h>

class plJoltWorldModule;
class plWorld;

/// \brief The limits a garment has to stay inside while the stress test moves its character.
struct PL_JOLTPLUGIN_DLL plJoltClothStressGates
{
  float m_fMaxStretch = 0.12f;       ///< how much longer than at rest an edge may stay: 0.12 is 12 percent
  float m_fStretchGraceTime = 0.1f;  ///< seconds an edge may be longer than that, as cloth is when it whips
  float m_fMaxStretchPeak = 0.5f;    ///< how much longer an edge may get at any time
  float m_fMaxOvershoot = 0.005f;    ///< metres a particle may be beyond its max distance
  float m_fMaxPenetration = 0.015f;  ///< metres a particle may be inside a body shape before it counts as inside
  plUInt32 m_uiMaxInsideBody = 0;    ///< particles that may be inside the body in one frame
  float m_fMaxRelativeSpeed = 12.0f; ///< metres per second the cloth may move against the body. More is a pop.
  float m_fMaxSolveMs = 1.0f;        ///< milliseconds one step may take. 0 does not check the cost.
};

/// \brief The gates of the stress test, as bits.
struct plJoltClothStressGate
{
  enum Enum : plUInt8
  {
    Stretch = PL_BIT(0),
    Overshoot = PL_BIT(1),
    InsideBody = PL_BIT(2),
    Pop = PL_BIT(3),
    Cost = PL_BIT(4),
    NotFinite = PL_BIT(5),
  };
};

/// \brief What the stress test measured in one frame.
struct PL_JOLTPLUGIN_DLL plJoltClothStressFrame
{
  float m_fTime = 0.0f;
  float m_fStretch = 0.0f;
  float m_fOvershoot = 0.0f;
  float m_fRelativeSpeed = 0.0f;
  float m_fSolveMs = 0.0f;
  plUInt32 m_uiInsideBody = 0;
  plUInt8 m_uiFailedGates = 0; ///< plJoltClothStressGate bits
};

struct PL_JOLTPLUGIN_DLL plJoltClothStressResult
{
  plDynamicArray<plJoltClothStressFrame> m_Frames;
  plJoltClothStressFrame m_Worst; ///< the largest value of every measure; m_uiFailedGates has every gate that failed in any frame
  plUInt32 m_uiFailedFrames = 0;

  bool Passed() const { return m_Worst.m_uiFailedGates == 0; }

  /// \brief The first frame in which the gate failed, or plInvalidIndex.
  plUInt32 FindFirstFailure(plJoltClothStressGate::Enum gate) const;

  /// \brief What to change in the cloth asset for every gate that failed.
  void GetSuggestions(plDynamicArray<plString>& out_suggestions) const;

  /// \brief The worst values, as the baseline a later run is compared with.
  plResult SerializeBaseline(plStreamWriter& inout_stream) const;
  static plResult DeserializeBaseline(plStreamReader& inout_stream, plJoltClothStressFrame& out_worst);

  /// \brief Whether this run is worse than the baseline by more than the tolerance (0.2 is 20 percent), and why.
  bool IsWorseThan(const plJoltClothStressFrame& baseline, float fTolerance, plStringBuilder& out_sWhy) const;
};

/// \brief How the motion script drives the character. The defaults are the defaults of the component.
struct PL_JOLTPLUGIN_DLL plJoltClothStressScriptOptions
{
  float m_fFrameRate = 60.0f;
  float m_fSprintSpeed = 7.0f;   ///< metres per second
  float m_fSpinSpeed = 360.0f;   ///< degrees per second
  float m_fTeleportStep = 30.0f; ///< metres the character jumps in one frame

  float m_fLinearInertia = 0.7f;
  float m_fAngularInertia = 0.7f;
  float m_fTeleportDistance = 2.0f;
  plEnum<plJoltClothTeleportMode> m_TeleportMode;
  plVec3 m_vWind = plVec3::MakeZero();
};

/// \brief Measures a garment frame by frame against the gates: stretch, max distance, body penetration, pops and cost.
///
/// Feed it the instance after every step. The cloth window does that while it plays the clips of a character; the
/// motion script below does it without any clip.
class PL_JOLTPLUGIN_DLL plJoltClothStressTest
{
public:
  /// \brief bodyShapes are all shapes of the character's body, whether the garment collides with them or not. That is
  /// how the test finds a collider that is missing. Both arrays must outlive the test.
  void Begin(const plJoltClothDescriptor& desc, const plJoltClothSolverSetup& setup, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, float fShapeScale = 1.0f);

  /// \brief Measures the instance after a step. bodyJoints has the world transform of the joint of every body shape.
  void AddFrame(const plJoltClothInstance& instance, const plVec3& vOwnerPosition, plArrayPtr<const plTransform> bodyJoints, float fTimeStep, float fMaxDistanceScale = 1.0f);

  const plJoltClothStressResult& GetResult() const { return m_Result; }

  /// \brief Runs a garment through the motion script: stand, sprint, hard stop, sprint backwards, spin and teleport.
  ///
  /// The character moves as one rigid body in its bind pose, so no clip and no skeleton is needed. The garment's own
  /// colliders are the body shapes when none are given.
  static plResult RunScript(plJoltWorldModule& ref_module, const plJoltClothDescriptor& desc, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, const plJoltClothStressScriptOptions& options, plJoltClothStressResult& out_result);

  /// \brief The same, in the physics of a world. The world must be locked for writing and not be updating.
  static plResult RunScript(plWorld& ref_world, const plJoltClothDescriptor& desc, plArrayPtr<const plJoltClothCollider> bodyShapes, const plJoltClothStressGates& gates, const plJoltClothStressScriptOptions& options, plJoltClothStressResult& out_result);

private:
  const plJoltClothDescriptor* m_pDesc = nullptr;
  const plJoltClothSolverSetup* m_pSetup = nullptr;
  plArrayPtr<const plJoltClothCollider> m_BodyShapes;
  plJoltClothStressGates m_Gates;
  float m_fShapeScale = 1.0f;

  plDynamicArray<float> m_RestLengths; ///< per triangle edge, in simulation units
  plDynamicArray<plVec3> m_LastOffsets; ///< per particle: where it was against its skinned position in the last frame
  bool m_bHasLastOffsets = false;
  float m_fTime = 0.0f;
  float m_fStretchedFor = 0.0f; ///< seconds the cloth has been over the stretch gate without a break

  plDynamicArray<plVec3> m_Positions;
  plDynamicArray<plVec3> m_Skinned;

  plJoltClothStressResult m_Result;
};
