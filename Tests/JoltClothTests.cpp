#include "JoltTestSupport.h"
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Time/Stopwatch.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/CVar.h>
#include <JoltPlugin/CharacterCloth/JoltBoneChainComponent.h>
#include <JoltPlugin/CharacterCloth/JoltCharacterClothComponent.h>
#include <JoltPlugin/CharacterCloth/JoltClothBuilder.h>
#include <JoltPlugin/CharacterCloth/JoltClothInstance.h>
#include <JoltPlugin/CharacterCloth/JoltClothMapTools.h>
#include <JoltPlugin/CharacterCloth/JoltClothStressTest.h>
#include <JoltPlugin/System/JoltCollisionFiltering.h>
#include <RendererCore/AnimationSystem/SkeletonBuilder.h>
#include <RendererCore/AnimationSystem/SkeletonResource.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

using namespace JoltTest;

namespace
{
  /// A rectangular cape that hangs behind a character at the origin: X is forward, Y is right, Z is up.
  /// All its particles follow bone 0, so one matrix moves the whole collar.
  struct Cape
  {
    plDynamicArray<plVec3> m_Positions;
    plDynamicArray<plUInt32> m_Indices;
    plDynamicArray<plVec4U16> m_BoneIndices;
    plDynamicArray<plVec4> m_BoneWeights;
    plDynamicArray<float> m_MaxDistance;

    plJoltClothDescriptor m_Desc;
    plJoltClothBuildReport m_Report;
    plJoltClothSolverSetup m_Setup;

    plUInt32 m_uiColumns = 0;
    plUInt32 m_uiRows = 0;
  };

  constexpr float s_fCapeWidth = 0.8f;
  constexpr float s_fCapeLength = 1.0f;
  constexpr float s_fCapeMaxDistance = 0.4f;

  void AddCapeVertices(Cape& ref_cape, plUInt32 uiFirstColumn, plUInt32 uiNumColumns, plUInt32 uiTotalColumns, plUInt32 uiRows)
  {
    const plUInt32 uiFirstVertex = ref_cape.m_Positions.GetCount();

    for (plUInt32 r = 0; r < uiRows; ++r)
    {
      for (plUInt32 c = 0; c < uiNumColumns; ++c)
      {
        const float u = static_cast<float>(uiFirstColumn + c) / static_cast<float>(uiTotalColumns - 1);
        const float v = static_cast<float>(r) / static_cast<float>(uiRows - 1);

        ref_cape.m_Positions.PushBack(plVec3(-0.2f, (u - 0.5f) * s_fCapeWidth, 1.5f - v * s_fCapeLength));
        ref_cape.m_BoneIndices.PushBack(plVec4U16(0, 0, 0, 0));
        ref_cape.m_BoneWeights.PushBack(plVec4(1, 0, 0, 0));

        // the collar is pinned, the rest may move further the lower it hangs
        ref_cape.m_MaxDistance.PushBack(v < 0.1f ? 0.0f : v * s_fCapeMaxDistance);
      }
    }

    for (plUInt32 r = 0; r + 1 < uiRows; ++r)
    {
      for (plUInt32 c = 0; c + 1 < uiNumColumns; ++c)
      {
        const plUInt32 i0 = uiFirstVertex + r * uiNumColumns + c;
        const plUInt32 i1 = i0 + 1;
        const plUInt32 i2 = i0 + uiNumColumns;
        const plUInt32 i3 = i2 + 1;

        // wound so that the front of the cape faces away from the character, towards -X
        for (plUInt32 uiIndex : {i0, i1, i2, i1, i3, i2})
        {
          ref_cape.m_Indices.PushBack(uiIndex);
        }
      }
    }
  }

  /// Builds a cape of uiColumns x uiRows particles. With bSeam it is made of two halves that share a column of
  /// duplicated vertices, the way a UV seam splits a render mesh.
  bool MakeCape(Cape& ref_cape, plUInt32 uiColumns, plUInt32 uiRows, bool bSeam = false, const plMat4& mBindToSim = plMat4::MakeIdentity())
  {
    ref_cape.m_uiColumns = uiColumns;
    ref_cape.m_uiRows = uiRows;

    if (bSeam)
    {
      const plUInt32 uiHalf = uiColumns / 2;
      AddCapeVertices(ref_cape, 0, uiHalf + 1, uiColumns, uiRows);
      AddCapeVertices(ref_cape, uiHalf, uiColumns - uiHalf, uiColumns, uiRows);
    }
    else
    {
      AddCapeVertices(ref_cape, 0, uiColumns, uiColumns, uiRows);
    }

    plJoltClothBuildPiece piece;
    piece.m_Positions = ref_cape.m_Positions;
    piece.m_Indices = ref_cape.m_Indices;
    piece.m_BoneIndices = ref_cape.m_BoneIndices;
    piece.m_BoneWeights = ref_cape.m_BoneWeights;
    piece.m_MaxDistance = ref_cape.m_MaxDistance;
    piece.m_uiFirstRenderVertex = 0;

    plJoltClothBuildOptions options;
    options.m_uiRenderVertexCount = ref_cape.m_Positions.GetCount();

    if (plJoltClothBuilder::Build(plMakeArrayPtr(&piece, 1), options, ref_cape.m_Desc, &ref_cape.m_Report).Failed())
      return false;

    return ref_cape.m_Setup.Build(ref_cape.m_Desc, mBindToSim).Succeeded();
  }

  plUInt16 ClothLayer(plJoltClothWorldCollision::Enum world)
  {
    return plJoltCollisionFiltering::ConstructCharacterClothLayer(0, world);
  }

  /// The collar bone turning about the up axis through the character.
  plMat4 Spin(float fAngleDeg)
  {
    return plMat4::MakeRotationZ(plAngle::MakeFromDegree(fAngleDeg));
  }

  struct Measure
  {
    float m_fMaxOvershoot = 0.0f; ///< how far a particle is beyond its max distance, metres
    float m_fMaxStretch = 0.0f;   ///< longest triangle edge against its rest length, minus one
    bool m_bFinite = true;
  };

  void MeasureCloth(const Cape& cape, const plJoltClothInstance& instance, Measure& ref_measure, float fMaxDistanceScale = 1.0f)
  {
    plDynamicArray<plVec3> positions;
    plDynamicArray<plVec3> skinned;
    instance.GetSimPositions(positions);
    instance.GetSkinnedPositions(skinned);

    for (plUInt32 p = 0; p < positions.GetCount(); ++p)
    {
      if (!positions[p].IsValid())
      {
        ref_measure.m_bFinite = false;
        return;
      }

      const float fDistance = (positions[p] - skinned[p]).GetLength();
      ref_measure.m_fMaxOvershoot = plMath::Max(ref_measure.m_fMaxOvershoot, fDistance - cape.m_Desc.m_MaxDistance[p] * fMaxDistanceScale);
    }

    const auto& indices = cape.m_Desc.m_Indices;
    for (plUInt32 i = 0; i + 2 < indices.GetCount(); i += 3)
    {
      for (plUInt32 e = 0; e < 3; ++e)
      {
        const plUInt32 a = indices[i + e];
        const plUInt32 b = indices[i + (e + 1) % 3];

        const float fRest = (cape.m_Desc.m_Positions[a] - cape.m_Desc.m_Positions[b]).GetLength();
        const float fNow = (positions[a] - positions[b]).GetLength();
        ref_measure.m_fMaxStretch = plMath::Max(ref_measure.m_fMaxStretch, fNow / fRest - 1.0f);
      }
    }
  }

  plVec3 HemCenter(const Cape& cape, const plJoltClothInstance& instance)
  {
    plDynamicArray<plVec3> positions;
    instance.GetSimPositions(positions);

    plVec3 vSum = plVec3::MakeZero();
    for (plUInt32 c = 0; c < cape.m_uiColumns; ++c)
    {
      vSum += positions[(cape.m_uiRows - 1) * cape.m_uiColumns + c];
    }
    return vSum / static_cast<float>(cape.m_uiColumns);
  }
} // namespace

/// A UV seam duplicates vertices. The sim mesh must come out as one connected sheet.
static void TestBuilderWeldsSeams()
{
  Cape whole;
  Cape split;
  Check(MakeCape(whole, 12, 16), "a cape builds");
  Check(MakeCape(split, 12, 16, true), "a cape made of two halves builds");

  Check(split.m_Positions.GetCount() == 12 * 16 + 16, "the split cape has one duplicated column of vertices");
  Check(split.m_Desc.GetParticleCount() == 12 * 16, "the duplicated column welds into single particles");
  Check(split.m_Desc.GetTriangleCount() == whole.m_Desc.GetTriangleCount(), "welding keeps every triangle");
  Check(split.m_Report.m_uiBoundaryEdges == whole.m_Report.m_uiBoundaryEdges, "the seam leaves no boundary edge behind");
  Check(split.m_Report.m_uiNonManifoldEdges == 0 && split.m_Report.m_uiLooseVertices == 0, "the welded cape is manifold and has no loose vertex");
  Check(split.m_Report.m_uiBoundVertices == split.m_Positions.GetCount(), "every render vertex of the cape is bound");

  bool bPinnedStaySkinned = true;
  bool bFreeFollowCloth = true;
  for (plUInt32 v = 0; v < split.m_Desc.m_Bindings.GetCount(); ++v)
  {
    const plClothVertexBinding& binding = split.m_Desc.m_Bindings[v];
    const float fMaxDistance = split.m_Desc.m_MaxDistance[binding.m_uiParticle0];

    if (fMaxDistance == 0.0f && binding.m_fBlend != 0.0f)
      bPinnedStaySkinned = false;
    if (fMaxDistance >= 0.02f && binding.m_fBlend != 1.0f)
      bFreeFollowCloth = false;
  }
  Check(bPinnedStaySkinned, "render vertices on pinned particles stay skinned");
  Check(bFreeFollowCloth, "render vertices on free particles follow the cloth fully");
}

/// A low-poly proxy is simulated in place of the garment, whose vertices bind to the proxy's triangles.
static void TestBuilderBindsToProxy()
{
  Cape garment;
  Cape proxy;
  AddCapeVertices(garment, 0, 21, 21, 31);
  AddCapeVertices(proxy, 0, 6, 6, 9);

  plJoltClothBuildPiece pieces[2];
  pieces[0].m_Positions = garment.m_Positions;
  pieces[0].m_Indices = garment.m_Indices;
  pieces[0].m_BoneIndices = garment.m_BoneIndices;
  pieces[0].m_BoneWeights = garment.m_BoneWeights;
  pieces[0].m_uiFirstRenderVertex = 0;
  pieces[0].m_uiProxyPiece = 1;

  pieces[1].m_Positions = proxy.m_Positions;
  pieces[1].m_Indices = proxy.m_Indices;
  pieces[1].m_BoneIndices = proxy.m_BoneIndices;
  pieces[1].m_BoneWeights = proxy.m_BoneWeights;
  pieces[1].m_MaxDistance = proxy.m_MaxDistance;

  plJoltClothBuildOptions options;
  options.m_uiRenderVertexCount = garment.m_Positions.GetCount();

  plJoltClothDescriptor desc;
  plJoltClothBuildReport report;
  Check(plJoltClothBuilder::Build(plMakeArrayPtr(pieces), options, desc, &report).Succeeded(), "a garment with a proxy builds");
  Check(desc.GetParticleCount() == 6 * 9, "only the proxy is simulated");
  Check(report.m_uiBoundVertices == garment.m_Positions.GetCount(), "every garment vertex is bound to the proxy");

  // both lie in one plane, so a bound vertex is exactly where its weights put it
  float fMaxError = 0.0f;
  for (plUInt32 v = 0; v < desc.m_Bindings.GetCount(); ++v)
  {
    const plClothVertexBinding& b = desc.m_Bindings[v];
    const plVec3 vBound = desc.m_Positions[b.m_uiParticle0] * b.m_fWeight0 + desc.m_Positions[b.m_uiParticle1] * b.m_fWeight1 +
                          desc.m_Positions[b.m_uiParticle2] * (1.0f - b.m_fWeight0 - b.m_fWeight1) + b.m_vOffset;

    fMaxError = plMath::Max(fMaxError, (vBound - garment.m_Positions[v]).GetLength());
  }
  CheckNear(fMaxError, 0.0f, 1.0e-4f, "bindings to the proxy reproduce the garment's bind pose");
}

static void TestDescriptorRoundTrip()
{
  Cape cape;
  Check(MakeCape(cape, 8, 10), "a cape builds");

  cape.m_Desc.m_Fabric.m_fBend = 0.61f;
  cape.m_Desc.m_Simulation.m_WorldCollision = plJoltClothWorldCollision::Static;

  plJoltClothCollider& collider = cape.m_Desc.m_Colliders.ExpandAndGetRef();
  collider.m_sJoint.Assign("Thigh_L");
  collider.m_fRadius = 0.09f;

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  Check(cape.m_Desc.Serialize(writer).Succeeded(), "the descriptor serializes");

  plJoltClothDescriptor loaded;
  plMemoryStreamReader reader(&storage);
  Check(loaded.Deserialize(reader).Succeeded(), "the descriptor deserializes");

  Check(loaded.GetParticleCount() == cape.m_Desc.GetParticleCount() && loaded.m_Indices.GetCount() == cape.m_Desc.m_Indices.GetCount(), "particles and triangles survive");
  Check(loaded.m_Bindings.GetCount() == cape.m_Desc.m_Bindings.GetCount() && loaded.m_Bindings[17].m_uiParticle0 == cape.m_Desc.m_Bindings[17].m_uiParticle0, "bindings survive");
  CheckNear(loaded.m_Fabric.m_fBend, 0.61f, 1.0e-6f, "the fabric survives");
  Check(loaded.m_Simulation.m_WorldCollision == plJoltClothWorldCollision::Static, "the simulation settings survive");
  Check(loaded.m_Colliders.GetCount() == 1 && loaded.m_Colliders[0].m_sJoint == plTempHashedString("Thigh_L"), "the colliders survive");
}

/// The envelope: whatever the pose does, no particle may be further from its skinned position than its max distance.
static void TestMaxDistanceHoldsOnSpinningJoint()
{
  World test;
  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  plJoltClothInstance instance;
  Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");

  Measure measure;
  Measure spinning;

  // two seconds of hanging, then the collar spins at one turn per second
  for (plUInt32 uiFrame = 0; uiFrame < 360; ++uiFrame)
  {
    const float fSpinTime = uiFrame > 120 ? (uiFrame - 120) / 60.0f : 0.0f;
    const plMat4 mSkin = Spin(fSpinTime * 360.0f);

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
    instance.Step(input);

    MeasureCloth(cape, instance, measure);
    if (uiFrame > 120)
      MeasureCloth(cape, instance, spinning);
  }

  std::printf("  spinning joint: largest overshoot %.4f m, largest edge stretch %.1f %%\n", measure.m_fMaxOvershoot, spinning.m_fMaxStretch * 100.0f);

  Check(measure.m_bFinite, "all particle positions stay finite");
  CheckNear(measure.m_fMaxOvershoot, 0.0f, 0.0005f, "no particle leaves its max distance, at rest or spinning");
  Check(spinning.m_fMaxStretch < 0.1f, "edges stretch less than 10 percent on a spinning joint");

  plDynamicArray<plVec3> positions;
  plDynamicArray<plVec3> skinned;
  instance.GetSimPositions(positions);
  instance.GetSkinnedPositions(skinned);

  float fPinnedError = 0.0f;
  for (plUInt32 p = 0; p < positions.GetCount(); ++p)
  {
    if (cape.m_Desc.m_MaxDistance[p] == 0.0f)
      fPinnedError = plMath::Max(fPinnedError, (positions[p] - skinned[p]).GetLength());
  }
  CheckNear(fPinnedError, 0.0f, 1.0e-4f, "pinned particles sit exactly on their skinned positions");

  // the scale shrinks the envelope: at zero the whole garment is skinned
  Measure scaled;
  for (plUInt32 uiFrame = 0; uiFrame < 30; ++uiFrame)
  {
    const plMat4 mSkin = Spin(uiFrame * 9.0f);

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
    input.m_fMaxDistanceScale = 0.0f;
    instance.Step(input);
  }
  MeasureCloth(cape, instance, scaled, 0.0f);
  CheckNear(scaled.m_fMaxOvershoot, 0.0f, 1.0e-4f, "with a max distance scale of zero the garment is fully skinned");
}

/// A character that moves further than the teleport distance in one frame must not whip its cloth across the level.
static void TestClothTeleport()
{
  for (plUInt32 uiMode = 0; uiMode < 2; ++uiMode)
  {
    World test;
    Cape cape;
    Check(MakeCape(cape, 12, 16), "a cape builds");

    plJoltClothInstance instance;
    Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");

    const plMat4 mSkin = plMat4::MakeIdentity();

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
    input.m_TeleportMode = uiMode == 0 ? plJoltClothTeleportMode::Reset : plJoltClothTeleportMode::KeepShape;
    input.m_vWind = plVec3(-6, 0, 0); // blows the cape out behind the character, so it has a shape to keep

    for (plUInt32 uiFrame = 0; uiFrame < 120; ++uiFrame)
      instance.Step(input);

    const plVec3 vHemBefore = HemCenter(cape, instance);

    input.m_vOwnerPosition = plVec3(50, -30, 4);
    instance.Step(input);

    Measure measure;
    MeasureCloth(cape, instance, measure);
    const plVec3 vHemAfter = HemCenter(cape, instance);

    Check(measure.m_bFinite, "positions are finite after a teleport");
    CheckNear(measure.m_fMaxOvershoot, 0.0f, 0.002f, "the cloth is inside its envelope right after a teleport");
    Check(measure.m_fMaxStretch < 0.1f, "a teleport does not stretch the cloth");

    if (uiMode == 0)
    {
      Check(instance.DidResetInLastStep(), "Reset mode resets the cloth on a teleport");
      Check(plMath::Abs(vHemAfter.x - (-0.2f)) < 0.05f, "after a reset the hem hangs at its skinned place");
    }
    else
    {
      Check(!instance.DidResetInLastStep(), "Keep Shape mode does not reset the cloth on a teleport");
      Check((vHemAfter - vHemBefore).GetLength() < 0.05f, "Keep Shape carries the blown-out shape along");
    }
  }
}

/// Sub steps keep their length, so a cape in a steady wind hangs the same at every frame rate.
static void TestFrameRateIndependence()
{
  const float frameRates[3] = {30.0f, 60.0f, 144.0f};
  plVec3 hems[3];
  plUInt32 iterations[3] = {};

  for (plUInt32 i = 0; i < 3; ++i)
  {
    World test;
    Cape cape;
    Check(MakeCape(cape, 12, 16), "a cape builds");

    plJoltClothInstance instance;
    Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");

    const plMat4 mSkin = plMat4::MakeIdentity();

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
    input.m_fTimeStep = 1.0f / frameRates[i];
    input.m_vWind = plVec3(-3, 0, 0);

    const plUInt32 uiFrames = static_cast<plUInt32>(4.0f * frameRates[i]);
    for (plUInt32 uiFrame = 0; uiFrame < uiFrames; ++uiFrame)
      instance.Step(input);

    hems[i] = HemCenter(cape, instance);
    iterations[i] = instance.GetLastIterations();
  }

  Check(iterations[0] > iterations[1] && iterations[1] > iterations[2], "a longer frame takes more sub steps");
  Check((hems[0] - hems[1]).GetLength() < 0.03f, "the hem hangs within 3 cm at 30 and 60 fps");
  Check((hems[2] - hems[1]).GetLength() < 0.03f, "the hem hangs within 3 cm at 144 and 60 fps");
  Check(hems[1].x < -0.25f, "the wind blows the hem out behind the character");
}

/// A mirrored and scaled character (a centimetre mesh under a flipped root transform) simulates like a plain one.
static void TestScaledAndMirroredBindSpace()
{
  World test;

  // bind space in centimetres with Y flipped; this takes it back to metres and the engine's handedness
  plMat4 mBindToSim = plMat4::MakeScaling(plVec3(0.01f, -0.01f, 0.01f));

  Cape cape;
  cape.m_uiColumns = 12;
  cape.m_uiRows = 16;
  AddCapeVertices(cape, 0, 12, 12, 16);

  // the same cape, authored in that bind space: mirrored positions need the opposite winding to face outward
  for (plVec3& vPosition : cape.m_Positions)
    vPosition = plVec3(vPosition.x * 100.0f, vPosition.y * -100.0f, vPosition.z * 100.0f);
  for (plUInt32 i = 0; i + 2 < cape.m_Indices.GetCount(); i += 3)
    plMath::Swap(cape.m_Indices[i + 1], cape.m_Indices[i + 2]);

  plJoltClothBuildPiece piece;
  piece.m_Positions = cape.m_Positions;
  piece.m_Indices = cape.m_Indices;
  piece.m_BoneIndices = cape.m_BoneIndices;
  piece.m_BoneWeights = cape.m_BoneWeights;
  piece.m_MaxDistance = cape.m_MaxDistance;
  piece.m_uiFirstRenderVertex = 0;

  plJoltClothBuildOptions options;
  options.m_fWeldDistance = 0.01f;
  options.m_uiRenderVertexCount = cape.m_Positions.GetCount();

  Check(plJoltClothBuilder::Build(plMakeArrayPtr(&piece, 1), options, cape.m_Desc).Succeeded(), "a cape in centimetres builds");
  Check(cape.m_Setup.Build(cape.m_Desc, mBindToSim).Succeeded(), "the solver setup builds for a mirrored, scaled bind space");

  Cape plain;
  Check(MakeCape(plain, 12, 16), "a plain cape builds");

  plJoltClothInstance instance;
  plJoltClothInstance reference;
  Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");
  Check(reference.Initialize(test.GetJolt(), plain.m_Desc, plain.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 2).Succeeded(), "the reference instance is created");

  const plMat4 mSkin = plMat4::MakeIdentity();

  plJoltClothInstance::StepInput input;
  input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
  input.m_vWind = plVec3(4, 0, 0); // into the character's back: only the backstop keeps the cape out of the body

  for (plUInt32 uiFrame = 0; uiFrame < 180; ++uiFrame)
  {
    instance.Step(input);
    reference.Step(input);
  }

  const plVec3 vHem = HemCenter(cape, instance);
  const plVec3 vReferenceHem = HemCenter(plain, reference);

  Check((vHem - vReferenceHem).GetLength() < 0.03f, "the scaled, mirrored cape hangs like the plain one");
  Check(vHem.x < -0.2f + 0.05f, "the backstop is on the body side: wind from behind does not push the cape through the character");

  // render particles come back in bind space, rotated only as far as the cloth actually turned
  plDynamicArray<plClothParticleState> particles;
  instance.GetRenderParticles(particles);

  bool bInBindUnits = true;
  for (plUInt32 p = 0; p < particles.GetCount(); ++p)
  {
    if ((particles[p].m_vPosition - cape.m_Desc.m_Positions[p]).GetLength() > s_fCapeMaxDistance * 100.0f + 1.0f)
      bInBindUnits = false;
  }
  Check(bInBindUnits, "render particles are in the bind space of the mesh");

  const plVec3 vPinnedNormal = particles[5].m_qRotation * plVec3(1, 0, 0);
  CheckNear(vPinnedNormal.x, 1.0f, 0.01f, "a pinned particle has not rotated");
}

/// The backstop keeps the cloth from sinking into the body behind it, where no collider is.
static void TestBackstop()
{
  // the pose holds the cape out flat: once with its outer face up (the body is below), once with it down
  for (plUInt32 uiSide = 0; uiSide < 2; ++uiSide)
  {
    World test;
    Cape cape;
    Check(MakeCape(cape, 12, 16), "a cape builds");

    for (plUInt32 p = 0; p < cape.m_Desc.GetParticleCount(); ++p)
    {
      if (cape.m_Desc.m_MaxDistance[p] > 0.0f)
        cape.m_Desc.m_MaxDistance[p] = 0.5f;
    }
    Check(cape.m_Setup.Build(cape.m_Desc, plMat4::MakeIdentity()).Succeeded(), "the solver setup builds");

    plJoltClothInstance instance;
    Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");

    const plAngle angle = plAngle::MakeFromDegree(uiSide == 0 ? 90.0f : -90.0f);
    const plMat4 mSkin = plMat4::MakeTranslation(plVec3(-0.2f, 0, 1.5f)) * plMat4::MakeRotationY(angle) * plMat4::MakeTranslation(plVec3(0.2f, 0, -1.5f));

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);

    for (plUInt32 uiFrame = 0; uiFrame < 180; ++uiFrame)
      instance.Step(input);

    plDynamicArray<plVec3> positions;
    instance.GetSimPositions(positions);

    float fLowest = 10.0f;
    for (const plVec3& p : positions)
      fLowest = plMath::Min(fLowest, p.z);

    if (uiSide == 0)
      Check(fLowest > 1.5f - 0.035f, "the cloth sinks no further than the backstop distance towards the body");
    else
      Check(fLowest < 1.5f - 0.3f, "away from the body the cloth falls freely");
  }
}

/// A leg that kicks back through the cape pushes it out of the way, for the garment that owns the collider only.
static void TestBodyColliders()
{
  World test;

  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  // a thigh: a capsule that starts at the hip joint and runs 40 cm along the joint's X axis
  plJoltClothCollider& thigh = cape.m_Desc.m_Colliders.ExpandAndGetRef();
  thigh.m_sJoint.Assign("Thigh");
  thigh.m_Type = plJoltClothCollider::Type::Capsule;
  thigh.m_Transform.m_vPosition.Set(0.2f, 0, 0);
  thigh.m_fRadius = 0.08f;
  thigh.m_fLength = 0.4f;

  Cape bare;
  Check(MakeCape(bare, 12, 16), "a second cape builds");

  plJoltClothInstance withCollider;
  plJoltClothInstance other;
  Check(withCollider.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 1).Succeeded(), "the cloth instance is created");
  Check(other.Initialize(test.GetJolt(), bare.m_Desc, bare.m_Setup, ClothLayer(plJoltClothWorldCollision::None), 2).Succeeded(), "the second cloth instance is created");
  Check(withCollider.GetNumColliders() == 1 && other.GetNumColliders() == 0, "the garment with a collider in its asset has a collider body");

  const plMat4 mSkin = plMat4::MakeIdentity();
  const plVec3 vHip(0, 0, 1.0f);
  plVec3 vKnee = vHip;

  // the leg swings back to 60 degrees and stays there
  for (plUInt32 uiFrame = 0; uiFrame < 180; ++uiFrame)
  {
    const plAngle swing = plAngle::MakeFromDegree(plMath::Min(uiFrame * 1.0f, 60.0f));
    const plVec3 vLegDirection(-plMath::Sin(swing), 0, -plMath::Cos(swing));
    vKnee = vHip + vLegDirection * 0.4f;

    plTransform joint;
    joint.m_vPosition = vHip;
    joint.m_qRotation = plQuat::MakeShortestRotation(plVec3(1, 0, 0), vLegDirection);
    joint.m_vScale.Set(1.0f);

    withCollider.SetColliderJointTransforms(plMakeArrayPtr(&joint, 1));

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
    withCollider.Step(input);
    other.Step(input);
  }

  auto CountInsideLeg = [&](const plJoltClothInstance& instance) -> plUInt32
  {
    plDynamicArray<plVec3> positions;
    instance.GetSimPositions(positions);

    plUInt32 uiInside = 0;
    for (const plVec3& p : positions)
    {
      const plVec3 vAxis = vKnee - vHip;
      const float t = plMath::Saturate((p - vHip).Dot(vAxis) / vAxis.GetLengthSquared());
      if ((p - (vHip + vAxis * t)).GetLength() < 0.08f - 0.005f)
        ++uiInside;
    }
    return uiInside;
  };

  const plUInt32 uiInsideWith = CountInsideLeg(withCollider);
  const plUInt32 uiInsideOther = CountInsideLeg(other);
  std::printf("  body collider: %u particles inside the leg with the collider, %u for the garment without\n", uiInsideWith, uiInsideOther);

  Check(uiInsideWith == 0, "no particle of the garment is inside its leg collider");
  Check(uiInsideOther > 0, "a garment without that collider is not pushed by it");

  Measure measure;
  MeasureCloth(cape, withCollider, measure);
  Check(measure.m_bFinite && measure.m_fMaxOvershoot < 0.0005f, "the pushed garment stays inside its envelope");

  const plUInt32 uiBodies = test.GetJolt().GetJoltSystem()->GetNumBodies();
  withCollider.Deinitialize();
  Check(test.GetJolt().GetJoltSystem()->GetNumBodies() == uiBodies - 2, "the collider body goes away with its garment");
}

/// With world collision on, a cape that is dropped onto a static table comes to rest on it.
static void TestWorldCollision()
{
  for (plUInt32 uiMode = 0; uiMode < 2; ++uiMode)
  {
    World test;

    // a table behind the character: its top is at z = 1.05, between x = -1.0 and x = -0.4
    test.AddStaticBox(plVec3(-0.7f, 0, 1.0f), plVec3(0.3f, 0.6f, 0.05f));
    test.Step(2);

    Cape cape;
    Check(MakeCape(cape, 12, 16), "a cape builds");

    // everything below the collar is free to fall, and no backstop holds it up
    for (plUInt32 p = 0; p < cape.m_Desc.GetParticleCount(); ++p)
    {
      if (cape.m_Desc.m_MaxDistance[p] > 0.0f)
        cape.m_Desc.m_MaxDistance[p] = 2.0f;

      cape.m_Desc.m_BackstopDistance[p] = 10.0f;
    }
    Check(cape.m_Setup.Build(cape.m_Desc, plMat4::MakeIdentity()).Succeeded(), "the solver setup builds");

    const plJoltClothWorldCollision::Enum world = uiMode == 0 ? plJoltClothWorldCollision::Static : plJoltClothWorldCollision::None;

    plJoltClothInstance instance;
    Check(instance.Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(world), 1).Succeeded(), "the cloth instance is created");

    // the pose holds the cape out flat behind the collar, 45 cm above the table, and the cloth drops from there
    const plMat4 mSkin = plMat4::MakeTranslation(plVec3(-0.2f, 0, 1.5f)) * plMat4::MakeRotationY(plAngle::MakeFromDegree(90)) * plMat4::MakeTranslation(plVec3(0.2f, 0, -1.5f));

    plJoltClothInstance::StepInput input;
    input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);

    for (plUInt32 uiFrame = 0; uiFrame < 240; ++uiFrame)
      instance.Step(input);

    plDynamicArray<plVec3> positions;
    instance.GetSimPositions(positions);

    plUInt32 uiInsideTable = 0;
    plUInt32 uiOnTable = 0;
    for (const plVec3& p : positions)
    {
      if (plMath::Abs(p.y) > 0.58f)
        continue;

      if (p.x > -0.98f && p.x < -0.42f && p.z > 0.96f && p.z < 1.04f)
        ++uiInsideTable;

      if (p.x > -1.0f && p.x < -0.4f && p.z >= 1.04f && p.z < 1.12f)
        ++uiOnTable;
    }

    std::printf("  world collision %s: %u particles on the table, %u inside it\n", uiMode == 0 ? "Static" : "None", uiOnTable, uiInsideTable);

    if (uiMode == 0)
    {
      Check(uiInsideTable == 0, "with world collision no particle is inside the static box");
      Check(uiOnTable >= 20, "the cape rests on top of the box");
    }
    else
    {
      Check(uiOnTable == 0, "without world collision nothing holds the cape up");
    }
  }
}

/// Instances come and go with their characters. Nothing may be left behind in the physics system.
static void TestInstanceChurn()
{
  World test;
  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  const plUInt32 uiBodiesBefore = test.GetJolt().GetJoltSystem()->GetNumBodies();

  for (plUInt32 uiRound = 0; uiRound < 20; ++uiRound)
  {
    plJoltClothInstance instances[8];
    const plMat4 mSkin = Spin(uiRound * 20.0f);

    for (plUInt32 i = 0; i < 8; ++i)
    {
      if (instances[i].Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::All), i + 1).Failed())
      {
        Check(false, "the cloth instance is created");
        return;
      }

      plJoltClothInstance::StepInput input;
      input.m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
      input.m_vOwnerPosition = plVec3(i * 2.0f, 0, 0);
      instances[i].Step(input);
    }

    test.Step(1);
  }

  Check(test.GetJolt().GetJoltSystem()->GetNumBodies() == uiBodiesBefore, "destroyed cloth instances leave no body behind");
}

/// The component drives an instance from the world update: it loads its asset, steps after the pose, follows its
/// properties, and leaves nothing behind when its object goes away.
static void TestComponent()
{
  World test;
  const plUInt32 uiBodiesBefore = test.GetJolt().GetJoltSystem()->GetNumBodies();

  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  plJoltClothResourceDescriptor resourceDesc;
  resourceDesc.m_Cloth = cape.m_Desc;
  const plJoltClothResourceHandle hCloth = plResourceManager::CreateResource<plJoltClothResource>("JoltTests_Cape", std::move(resourceDesc));

  const plMat4 mSkin = plMat4::MakeIdentity();
  plHybridArray<plGameObjectHandle, 8> objects;
  plHybridArray<plComponentHandle, 8> components;

  // several garments, so that the manager steps them on worker threads
  for (plUInt32 i = 0; i < 6; ++i)
  {
    plGameObject* pObject = test.CreateObject(plVec3(i * 2.0f, 0, 0));
    objects.PushBack(pObject->GetHandle());

    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = nullptr;
    components.PushBack(plJoltCharacterClothComponent::CreateComponent(pObject, pCloth));
    pCloth->SetCloth(hCloth);
    pCloth->SetPose(plMakeArrayPtr(&mSkin, 1), plMat4::MakeIdentity());
  }

  auto GetCloth = [&](plUInt32 uiIndex) -> plJoltCharacterClothComponent*
  {
    plJoltCharacterClothComponent* pCloth = nullptr;
    test.GetWorld().TryGetComponent(components[uiIndex], pCloth);
    return pCloth;
  };

  test.StepSeconds(2.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = GetCloth(0);
    Check(pCloth != nullptr && pCloth->GetState() == plJoltClothState::Full, "a garment with an asset and a pose simulates at full rate");
    Check(pCloth != nullptr && pCloth->GetInstance().GetParticleCount() == cape.m_Desc.GetParticleCount(), "the component built its cloth from the asset");

    plJoltCharacterClothComponentManager* pManager = test.GetWorld().GetComponentManager<plJoltCharacterClothComponentManager>();
    Check(pManager != nullptr && pManager->GetNumSimulated() == 6, "the manager stepped all six garments");

    Measure measure;
    MeasureCloth(cape, pCloth->GetInstance(), measure);
    Check(measure.m_bFinite && measure.m_fMaxOvershoot < 0.0005f, "the garment stays inside its envelope");

    // switching simulation off fades the envelope to nothing instead of popping
    pCloth->SetSimulate(false, plTime::MakeFromSeconds(0.5));
  }

  test.StepSeconds(0.25f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    Check(GetCloth(0)->GetState() == plJoltClothState::Blending, "half way through the fade the garment is blending");
  }

  test.StepSeconds(0.5f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    Check(GetCloth(0)->GetState() == plJoltClothState::Skinned, "after the fade the garment is skinned");
    Check(GetCloth(1)->GetState() == plJoltClothState::Full, "the other garments keep simulating");

    GetCloth(0)->SetSimulate(true, plTime::MakeZero());

    // a character that is scaled up needs its own solver setup, built on demand
    plGameObject* pObject = nullptr;
    test.GetWorld().TryGetObject(objects[2], pObject);
    pObject->SetGlobalScaling(plVec3(1.5f));
  }

  test.StepSeconds(1.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    Check(GetCloth(0)->GetState() == plJoltClothState::Full, "switched back on, the garment simulates again");

    plDynamicArray<plVec3> scaled;
    plDynamicArray<plVec3> plain;
    GetCloth(2)->GetInstance().GetSimPositions(scaled);
    GetCloth(1)->GetInstance().GetSimPositions(plain);
    CheckNear(scaled[0].z, plain[0].z * 1.5f, 0.01f, "a scaled character simulates a scaled garment");
  }

  // A paused world keeps its cloth as it is. A garment that appears while it is paused still starts from its pose.
  plDynamicArray<plVec3> beforePause;
  plComponentHandle hLate;

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    GetCloth(1)->GetInstance().GetSimPositions(beforePause);
    test.GetWorld().GetClock().SetPaused(true);

    plGameObject* pObject = test.CreateObject(plVec3(20.0f, 0, 0));
    objects.PushBack(pObject->GetHandle());

    plJoltCharacterClothComponent* pLate = nullptr;
    hLate = plJoltCharacterClothComponent::CreateComponent(pObject, pLate);
    pLate->SetCloth(hCloth);
    pLate->SetPose(plMakeArrayPtr(&mSkin, 1), plMat4::MakeIdentity());
  }

  test.Step(5);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plDynamicArray<plVec3> duringPause;
    GetCloth(1)->GetInstance().GetSimPositions(duringPause);

    float fMoved = 0.0f;
    for (plUInt32 p = 0; p < beforePause.GetCount(); ++p)
      fMoved = plMath::Max(fMoved, (duringPause[p] - beforePause[p]).GetLength());

    CheckNear(fMoved, 0.0f, 0.00001f, "a paused world leaves its cloth where it is");
    Check(GetCloth(1)->GetState() == plJoltClothState::Full, "a paused garment keeps its state");

    plJoltCharacterClothComponent* pLate = nullptr;
    Check(test.GetWorld().TryGetComponent(hLate, pLate) && pLate->GetInstance().IsInitialized() && pLate->GetState() == plJoltClothState::Full, "a garment created in a paused world starts from its pose");

    test.GetWorld().GetClock().SetPaused(false);

    for (const plGameObjectHandle& hObject : objects)
      test.GetWorld().DeleteObjectNow(hObject);
  }

  test.Step(2);
  Check(test.GetJolt().GetJoltSystem()->GetNumBodies() == uiBodiesBefore, "deleted garments leave no body behind");
}

/// The budget goes to the important and the near garments. The rest fade to skinned; far ones step at half rate.
static void TestLodAndBudget()
{
  World test;

  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  plJoltClothResourceDescriptor resourceDesc;
  resourceDesc.m_Cloth = cape.m_Desc;
  const plJoltClothResourceHandle hCloth = plResourceManager::CreateResource<plJoltClothResource>("JoltTests_LodCape", std::move(resourceDesc));

  plCVarInt* pBudget = static_cast<plCVarInt*>(plCVar::FindCVarByName("Jolt.Cloth.MaxSimulatedGarments"));
  Check(pBudget != nullptr, "the budget cvar exists");
  if (pBudget == nullptr)
    return;

  // without a budget of its own the manager takes the one of the quality tier; a test has no game settings: medium
  Check(plJoltCharacterClothComponentManager::GetBudgetForTier("low") < plJoltCharacterClothComponentManager::GetBudgetForTier("high"), "the low tier simulates fewer garments than the high tier");
  Check(plJoltCharacterClothComponentManager::GetBudgetForTier("high") <= plJoltCharacterClothComponentManager::GetBudgetForTier("max"), "the max tier simulates at least as many garments as the high tier");
  Check(plJoltCharacterClothComponentManager::GetBudgetForTier("custom") == plJoltCharacterClothComponentManager::GetBudgetForTier("medium"), "a tier the plugin does not know gets the medium budget");

  {
    plCVarInt* pMedium = static_cast<plCVarInt*>(plCVar::FindCVarByName("Jolt.Cloth.Budget.Medium"));
    Check(pMedium != nullptr && plJoltCharacterClothComponentManager::GetBudgetForTier("medium") == *pMedium, "the budget of a tier is its cvar");
  }

  const plInt32 iOldBudget = *pBudget;
  *pBudget = 3;

  // metres from the reference point: four near ones, one in the half-rate band, one beyond the simulate distance
  const float distances[6] = {2.0f, 5.0f, 8.0f, 11.0f, 20.0f, 40.0f};
  const plMat4 mSkin = plMat4::MakeIdentity();
  plHybridArray<plComponentHandle, 8> components;

  for (float fDistance : distances)
  {
    plGameObject* pObject = test.CreateObject(plVec3(fDistance, 0, 0));

    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = nullptr;
    components.PushBack(plJoltCharacterClothComponent::CreateComponent(pObject, pCloth));
    pCloth->SetCloth(hCloth);
    pCloth->SetPose(plMakeArrayPtr(&mSkin, 1), plMat4::MakeIdentity());
  }

  plJoltCharacterClothComponentManager* pManager = nullptr;
  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    pManager = test.GetWorld().GetOrCreateComponentManager<plJoltCharacterClothComponentManager>();
    pManager->SetLodReferencePoint(plVec3::MakeZero());
  }

  auto StateOf = [&](plUInt32 uiIndex) -> plJoltClothState
  {
    plJoltCharacterClothComponent* pCloth = nullptr;
    test.GetWorld().TryGetComponent(components[uiIndex], pCloth);
    return pCloth->GetState();
  };

  // a garment at reduced rate reports Reduced on the frames it steps and on the ones it skips
  auto IsSimulated = [&](plUInt32 uiIndex) -> bool
  {
    const plJoltClothState state = StateOf(uiIndex);
    return state == plJoltClothState::Full || state == plJoltClothState::Reduced;
  };

  test.StepSeconds(1.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    Check(StateOf(0) == plJoltClothState::Full && StateOf(1) == plJoltClothState::Full && StateOf(2) == plJoltClothState::Full, "a budget of three simulates the three nearest garments");
    Check(StateOf(3) == plJoltClothState::Skinned && StateOf(4) == plJoltClothState::Skinned, "garments over the budget are skinned");
    Check(StateOf(5) == plJoltClothState::Skinned, "a garment beyond the simulate distance is skinned");
  }

  // a budget that is larger than the number of garments limits nothing
  *pBudget = 1000;
  test.StepSeconds(1.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    Check(StateOf(3) == plJoltClothState::Full, "without a limit the fourth garment simulates again");
    Check(StateOf(4) == plJoltClothState::Reduced, "a garment beyond the full rate distance steps at reduced rate");
    Check(StateOf(5) == plJoltClothState::Skinned, "the far garment stays skinned without a limit too");

    plJoltCharacterClothComponent* pFar = nullptr;
    test.GetWorld().TryGetComponent(components[5], pFar);
    pFar->m_Importance = plJoltClothImportance::Always;
  }

  test.StepSeconds(1.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    Check(StateOf(5) == plJoltClothState::Full, "Importance Always simulates at full rate at any distance");
  }

  // four full, one always, one at half rate: 5.5 garments step per frame on average
  plUInt32 uiSteps = 0;
  for (plUInt32 uiFrame = 0; uiFrame < 60; ++uiFrame)
  {
    test.Step(1);
    uiSteps += pManager->GetNumSimulated();
  }
  Check(uiSteps >= 325 && uiSteps <= 335, "the garment at reduced rate steps every other frame");

  *pBudget = 0;
  test.StepSeconds(1.0f);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    Check(!IsSimulated(0) && !IsSimulated(4), "a budget of zero skins every ordinary garment");
    Check(StateOf(5) == plJoltClothState::Full, "and still simulates the one with Importance Always");
  }

  *pBudget = iOldBudget;
}

/// The C0 measurement: what a garment costs per step, and whether garments can step on several threads at once.
static void TestCostAndParallelStepping()
{
  struct Size
  {
    plUInt32 m_uiColumns;
    plUInt32 m_uiRows;
  };
  const Size sizes[3] = {{14, 18}, {20, 30}, {40, 50}};

  std::printf("  cloth cost   particles  constraints  serial ms/step  16 garments serial ms  16 garments parallel ms\n");

  for (const Size& size : sizes)
  {
    World test;
    test.AddFloor();
    test.Step(2);

    Cape cape;
    Check(MakeCape(cape, size.m_uiColumns, size.m_uiRows), "a cape builds");

    constexpr plUInt32 uiNumGarments = 16;
    plJoltClothInstance serial[uiNumGarments];
    plJoltClothInstance parallel[uiNumGarments];

    for (plUInt32 i = 0; i < uiNumGarments; ++i)
    {
      Check(serial[i].Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::Static), i + 1).Succeeded(), "the cloth instance is created");
      Check(parallel[i].Initialize(test.GetJolt(), cape.m_Desc, cape.m_Setup, ClothLayer(plJoltClothWorldCollision::Static), i + 100).Succeeded(), "the cloth instance is created");
    }

    constexpr plUInt32 uiFrames = 240;
    plTime serialTime;
    plTime parallelTime;

    for (plUInt32 uiFrame = 0; uiFrame < uiFrames; ++uiFrame)
    {
      const plMat4 mSkin = Spin(uiFrame * 3.0f);

      plJoltClothInstance::StepInput inputs[uiNumGarments];
      for (plUInt32 i = 0; i < uiNumGarments; ++i)
      {
        inputs[i].m_SkinTransforms = plMakeArrayPtr(&mSkin, 1);
        inputs[i].m_vOwnerPosition = plVec3(i * 1.5f, 0, 0);
        inputs[i].m_vWind = plVec3(-2, 1, 0);
      }

      {
        plStopwatch timer;
        for (plUInt32 i = 0; i < uiNumGarments; ++i)
          serial[i].Step(inputs[i]);
        serialTime += timer.GetRunningTotal();
      }

      {
        plStopwatch timer;
        plTaskSystem::ParallelForIndexed(static_cast<plUInt32>(0), uiNumGarments, [&](plUInt32 uiStart, plUInt32 uiEnd)
          {
            for (plUInt32 i = uiStart; i < uiEnd; ++i)
              parallel[i].Step(inputs[i]); },
          "ClothStep");
        parallelTime += timer.GetRunningTotal();
      }
    }

    // the same garment with the same input must come out the same, whichever thread stepped it
    float fMaxDifference = 0.0f;
    bool bFinite = true;
    for (plUInt32 i = 0; i < uiNumGarments; ++i)
    {
      plDynamicArray<plVec3> a;
      plDynamicArray<plVec3> b;
      serial[i].GetSimPositions(a);
      parallel[i].GetSimPositions(b);

      for (plUInt32 p = 0; p < a.GetCount(); ++p)
      {
        bFinite = bFinite && a[p].IsValid() && b[p].IsValid();
        fMaxDifference = plMath::Max(fMaxDifference, (a[p] - b[p]).GetLength());
      }
    }

    const double fSerialPerGarment = serialTime.GetMilliseconds() / (uiFrames * uiNumGarments);
    const double fSerialPerFrame = serialTime.GetMilliseconds() / uiFrames;
    const double fParallelPerFrame = parallelTime.GetMilliseconds() / uiFrames;

    std::printf("               %9u  %11u  %14.3f  %21.3f  %23.3f\n", cape.m_Desc.GetParticleCount(), cape.m_Setup.m_uiNumConstraints, fSerialPerGarment, fSerialPerFrame, fParallelPerFrame);

    Check(bFinite, "serial and parallel stepping both stay finite");
    CheckNear(fMaxDifference, 0.0f, 1.0e-4f, "a garment stepped on a worker thread matches the same garment stepped serially");

    // Development builds are not optimized, so only a Release build says anything about cost
    const bool bOptimized = plOSFile::GetApplicationDirectory().FindSubString_NoCase("_Release") != nullptr;

    if (size.m_uiColumns == 20 && bOptimized)
      Check(fSerialPerGarment < 0.5, "a 600 particle garment steps in under 0.5 ms");
  }
}

/// The red, green and blue vertex colours of an imported garment become its max distance, backstop and stiffness maps.
static void TestImportedMasks()
{
  Cape cape;
  AddCapeVertices(cape, 0, 6, 6, 8);

  plMeshResourceDescriptor mesh;
  mesh.m_bHasClothSubMeshes = true;

  auto& garment = mesh.m_ClothSubMeshes.ExpandAndGetRef();
  garment.m_sName.Assign("CLOTH_Cape");
  garment.m_uiFirstVertex = 0;
  garment.m_uiVertexCount = cape.m_Positions.GetCount();
  garment.m_RestPositions = cape.m_Positions;
  garment.m_Indices = cape.m_Indices;
  garment.m_BoneIndices = cape.m_BoneIndices;
  garment.m_BoneWeights = cape.m_BoneWeights;

  // red pins the top row, green fades in from the top, blue is the same everywhere
  for (plUInt32 r = 0; r < 8; ++r)
  {
    for (plUInt32 c = 0; c < 6; ++c)
    {
      garment.m_PinMaskImported.PushBack(r == 0 ? 255 : 0);
      garment.m_BackstopMaskImported.PushBack(static_cast<plUInt8>(r * 255 / 7));
      garment.m_StiffnessMaskImported.PushBack(128);
    }
  }

  plJoltClothBuildOptions options;
  options.m_uiRenderVertexCount = cape.m_Positions.GetCount();
  options.m_fDefaultBackstopDistance = 0.1f;

  plJoltClothDescriptor desc;
  Check(plJoltClothBuilder::BuildFromMesh(mesh, {}, 0.5f, options, desc).Succeeded(), "a garment builds from an imported mesh");
  Check(desc.GetParticleCount() == 6 * 8, "the imported garment has all its particles");

  CheckNear(desc.m_MaxDistance[0], 0.0f, 1.0e-6f, "a red vertex is pinned");
  CheckNear(desc.m_MaxDistance[6 * 7], 0.5f, 1.0e-6f, "a vertex without red may move the full max distance");
  CheckNear(desc.m_BackstopDistance[0], 0.0f, 1.0e-6f, "no green gives no room behind the surface");
  CheckNear(desc.m_BackstopDistance[6 * 7], 0.1f, 1.0e-4f, "full green gives the default backstop distance");
  CheckNear(desc.m_StiffnessScale[10], 1.0f, 1.0e-6f, "a colour channel with one value everywhere counts as not painted");

  plJoltClothDescriptor named;
  const plString sOther = "CLOTH_Skirt";
  Check(plJoltClothBuilder::BuildFromMesh(mesh, plMakeArrayPtr(&sOther, 1), 0.5f, options, named).Failed(), "asking for a garment the mesh does not have fails");

  options.m_bUseImportedMasks = false;
  options.m_fDefaultMaxDistance = 0.25f;

  plJoltClothDescriptor unmasked;
  Check(plJoltClothBuilder::BuildFromMesh(mesh, {}, 0.5f, options, unmasked).Succeeded(), "the garment builds without its masks");
  CheckNear(unmasked.m_MaxDistance[0], 0.25f, 1.0e-6f, "without the imported masks every particle gets the default max distance");
}

/// The capsules, spheres and boxes of the skeleton become body colliders, the ones the cloth can reach by default.
static void TestSkeletonColliders()
{
  Cape cape;
  Check(MakeCape(cape, 10, 12), "a cape builds");

  plSkeletonResourceDescriptor skeleton;

  {
    plSkeletonBuilder builder;
    const plUInt16 uiHips = builder.AddJoint("Hips", plTransform(plVec3(0, 0, 1.0f)));
    const plUInt16 uiSpine = builder.AddJoint("Spine", plTransform(plVec3(0, 0, 0.3f)), uiHips);
    builder.AddJoint("Hand", plTransform(plVec3(0, 2.0f, 0)), uiSpine);
    builder.BuildSkeleton(skeleton.m_Skeleton);
    skeleton.m_Skeleton.m_BoneDirection = plBasisAxis::PositiveX;
  }

  // a capsule along the spine, right in front of the cape
  {
    plSkeletonResourceGeometry& geo = skeleton.m_Geometry.ExpandAndGetRef();
    geo.m_uiAttachedToJoint = 1;
    geo.m_Type = plSkeletonJointGeometryType::Capsule;
    geo.m_Transform = plTransform(plVec3(0, 0, 0), plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(-90)), plVec3(0.3f, 1, 0.25f));
  }

  // a sphere on the hand, two metres away from the cape
  {
    plSkeletonResourceGeometry& geo = skeleton.m_Geometry.ExpandAndGetRef();
    geo.m_uiAttachedToJoint = 2;
    geo.m_Type = plSkeletonJointGeometryType::Sphere;
    geo.m_Transform = plTransform(plVec3::MakeZero(), plQuat::MakeIdentity(), plVec3(1, 1, 0.06f));
  }

  // a convex shape cannot be used
  {
    plSkeletonResourceGeometry& geo = skeleton.m_Geometry.ExpandAndGetRef();
    geo.m_uiAttachedToJoint = 0;
    geo.m_Type = plSkeletonJointGeometryType::ConvexMesh;
  }

  plJoltClothDescriptor reach = cape.m_Desc;
  plJoltClothSkeletonColliderOptions options;

  Check(plJoltClothBuilder::AddSkeletonColliders(skeleton, options, reach) == 1, "only the shape the cape can reach becomes a collider");
  Check(reach.m_Colliders.GetCount() == 1 && reach.m_Colliders[0].m_sJoint == plTempHashedString("Spine"), "the collider follows the spine");

  if (reach.m_Colliders.GetCount() == 1)
  {
    const plJoltClothCollider& collider = reach.m_Colliders[0];
    Check(collider.m_Type == plJoltClothCollider::Type::Capsule, "a skeleton capsule stays a capsule");
    CheckNear(collider.m_fRadius, 0.25f, 1.0e-5f, "the capsule keeps its radius");
    CheckNear(collider.m_fLength, 0.3f, 1.0e-5f, "the capsule keeps its length");

    // the shape starts at the joint and points up the spine: its middle is half its length above the joint
    const plVec3 vAxis = collider.m_Transform.m_qRotation * plVec3(1, 0, 0);
    CheckNear(plMath::Abs(vAxis.z), 1.0f, 1.0e-4f, "the capsule runs along the spine");
    CheckNear(collider.m_Transform.m_vPosition.GetLength(), 0.15f, 1.0e-4f, "the capsule's middle is half its length from the joint");
  }

  Check(reach.Validate().Succeeded(), "a cloth with skeleton colliders is valid");

  plJoltClothDescriptor listed = cape.m_Desc;
  const plString sHand = "Hand";
  options.m_Joints = plMakeArrayPtr(&sHand, 1);
  options.m_fInflate = 0.02f;

  Check(plJoltClothBuilder::AddSkeletonColliders(skeleton, options, listed) == 1, "a listed joint's shape is taken, in reach or not");
  Check(listed.m_Colliders.GetCount() == 1 && listed.m_Colliders[0].m_Type == plJoltClothCollider::Type::Sphere, "the hand's sphere is the collider");

  if (listed.m_Colliders.GetCount() == 1)
    CheckNear(listed.m_Colliders[0].m_fRadius, 0.08f, 1.0e-5f, "inflate adds to the radius");
}

/// Bone chains as the stand-in of a garment: they rest while its cloth simulates and swing while it is skinned.
static void TestBoneChainStandIn()
{
  World test;

  Cape cape;
  Check(MakeCape(cape, 8, 10), "a cape builds");

  plJoltClothResourceDescriptor resourceDesc;
  resourceDesc.m_Cloth = cape.m_Desc;
  const plJoltClothResourceHandle hCloth = plResourceManager::CreateResource<plJoltClothResource>("JoltTests_StandInCape", std::move(resourceDesc));

  // a tail of two joints behind the hips
  plSkeletonResourceDescriptor skeleton;
  plDynamicArray<plMat4> restPose;

  {
    plSkeletonBuilder builder;
    const plUInt16 uiHips = builder.AddJoint("Hips", plTransform(plVec3(0, 0, 1.0f)));
    const plUInt16 uiTail1 = builder.AddJoint("Tail1", plTransform(plVec3(-0.1f, 0, 0)), uiHips);
    builder.AddJoint("Tail2", plTransform(plVec3(-0.1f, 0, 0)), uiTail1);
    builder.BuildSkeleton(skeleton.m_Skeleton);
    skeleton.m_Skeleton.m_BoneDirection = plBasisAxis::PositiveX;

    restPose.PushBack(plMat4::MakeTranslation(plVec3(0, 0, 1.0f)));
    restPose.PushBack(plMat4::MakeTranslation(plVec3(-0.1f, 0, 1.0f)));
    restPose.PushBack(plMat4::MakeTranslation(plVec3(-0.2f, 0, 1.0f)));
  }

  const plTransform rootTransform = skeleton.m_RootTransform;
  const plSkeletonResourceHandle hSkeleton = plResourceManager::CreateResource<plSkeletonResource>("JoltTests_StandInSkeleton", std::move(skeleton));

  plGameObject* pObject = test.CreateObject(plVec3::MakeZero());
  const plMat4 mSkin = plMat4::MakeIdentity();
  plComponentHandle hChain;
  plComponentHandle hClothComponent;

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = nullptr;
    hClothComponent = plJoltCharacterClothComponent::CreateComponent(pObject, pCloth);
    pCloth->SetCloth(hCloth);
    pCloth->SetPose(plMakeArrayPtr(&mSkin, 1), plMat4::MakeIdentity());
    pCloth->m_Importance = plJoltClothImportance::Always;

    plJoltBoneChainComponent* pChain = nullptr;
    hChain = plJoltBoneChainComponent::CreateComponent(pObject, pChain);
    pChain->m_Chains.PushBack("Tail1");
    pChain->m_fStiffness = 0.0f;
    pChain->m_MaxAngle = plAngle::MakeFromDegree(180);
    pChain->m_bStandInForCloth = true;
    pChain->SetSkeleton(hSkeleton);
  }

  // the animation sends the rest pose every frame
  auto Play = [&](plUInt32 uiFrames)
  {
    for (plUInt32 uiFrame = 0; uiFrame < uiFrames; ++uiFrame)
    {
      {
        PL_LOCK(test.GetWorld().GetWriteMarker());

        plResourceLock<plSkeletonResource> pSkeleton(hSkeleton, plResourceAcquireMode::BlockTillLoaded);

        plMsgAnimationPoseUpdated msg;
        msg.m_pRootTransform = &rootTransform;
        msg.m_pSkeleton = &pSkeleton->GetDescriptor().m_Skeleton;
        msg.m_ModelTransforms = restPose;
        pObject->SendMessage(msg);
      }

      test.Step(1);
    }
  };

  auto TailEnd = [&]() -> plVec3
  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltBoneChainComponent* pChain = nullptr;
    if (!test.GetWorld().TryGetComponent(hChain, pChain) || pChain->GetChainCount() == 0 || pChain->GetChainPoints(0).GetCount() != 3)
      return plVec3::MakeZero();

    return pChain->GetChainPoints(0)[2];
  };

  auto SetSimulate = [&](bool bSimulate)
  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = nullptr;
    if (test.GetWorld().TryGetComponent(hClothComponent, pCloth))
      pCloth->SetSimulate(bSimulate, plTime::MakeZero());
  };

  SetSimulate(false);
  Play(120);
  Check(TailEnd().z < 0.85f, "while the cloth is skinned the stand-in chain hangs under gravity");

  SetSimulate(true);
  Play(60);

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());

    plJoltCharacterClothComponent* pCloth = nullptr;
    Check(test.GetWorld().TryGetComponent(hClothComponent, pCloth) && pCloth->GetState() == plJoltClothState::Full, "the cloth simulates again");
  }

  // the chain keeps the points it had when it stopped: a later hand-over starts from the animated pose again
  SetSimulate(false);
  Play(2);
  Check(TailEnd().z > 0.97f, "the stand-in chain starts from the animated pose when the cloth hands over");

  Play(120);
  Check(TailEnd().z < 0.85f, "and swings again while the cloth is skinned");
}

/// Fit From Skin: a limb skinned to one bone becomes a capsule along it, a ball a sphere, and thin parts get nothing.
static void TestFitCollidersFromSkin()
{
  plDynamicArray<plVec3> positions;
  plDynamicArray<plVec4U16> boneIndices;
  plDynamicArray<plVec4> boneWeights;

  auto AddVertex = [&](const plVec3& vPosition, plUInt16 uiBone)
  {
    positions.PushBack(vPosition);
    boneIndices.PushBack(plVec4U16(uiBone, 0, 0, 0));
    boneWeights.PushBack(plVec4(1, 0, 0, 0));
  };

  // bone 0: a thigh, a cylinder of radius 8 cm from the hip at z = 1 down to the knee at z = 0.5
  for (plUInt32 ring = 0; ring <= 10; ++ring)
  {
    for (plUInt32 side = 0; side < 16; ++side)
    {
      const plAngle angle = plAngle::MakeFromDegree(side * 22.5f);
      AddVertex(plVec3(0.08f * plMath::Cos(angle), 0.2f + 0.08f * plMath::Sin(angle), 1.0f - ring * 0.05f), 0);
    }
  }

  // bone 1: a head, points on a sphere of radius 10 cm around z = 1.7
  for (plUInt32 ring = 1; ring < 8; ++ring)
  {
    for (plUInt32 side = 0; side < 12; ++side)
    {
      const plAngle polar = plAngle::MakeFromDegree(ring * 22.5f);
      const plAngle angle = plAngle::MakeFromDegree(side * 30.0f);
      AddVertex(plVec3(0, 0, 1.7f) + plVec3(plMath::Sin(polar) * plMath::Cos(angle), plMath::Sin(polar) * plMath::Sin(angle), plMath::Cos(polar)) * 0.1f, 1);
    }
  }

  // bone 2: a finger, a cylinder of radius 1 cm
  for (plUInt32 ring = 0; ring <= 4; ++ring)
  {
    for (plUInt32 side = 0; side < 8; ++side)
    {
      const plAngle angle = plAngle::MakeFromDegree(side * 45.0f);
      AddVertex(plVec3(0.6f + ring * 0.02f, 0.01f * plMath::Cos(angle), 1.4f + 0.01f * plMath::Sin(angle)), 2);
    }
  }

  plHashedString names[3];
  names[0].Assign("Thigh");
  names[1].Assign("Head");
  names[2].Assign("Finger");

  // the thigh's joint sits at the hip and is turned, so that the fit has to work in the joint's own space
  const plTransform thighJoint(plVec3(0, 0.2f, 1.0f), plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(90)));
  const plTransform headJoint(plVec3(0, 0, 1.6f));
  const plTransform fingerJoint(plVec3(0.6f, 0, 1.4f));

  const plMat4 inverseBindPoses[3] = {thighJoint.GetAsMat4().GetInverse(), headJoint.GetAsMat4().GetInverse(), fingerJoint.GetAsMat4().GetInverse()};

  plJoltClothSkinFitOptions options;
  options.m_fRadiusScale = 1.0f;

  plDynamicArray<plJoltClothCollider> colliders;
  Check(plJoltClothBuilder::FitCollidersFromSkin(positions, boneIndices, boneWeights, names, inverseBindPoses, options, nullptr, colliders) == 2, "the thigh and the head get a shape, the finger does not");

  if (colliders.GetCount() == 2)
  {
    const plJoltClothCollider& thigh = colliders[0];
    Check(thigh.m_sJoint == plTempHashedString("Thigh") && thigh.m_Type == plJoltClothCollider::Type::Capsule, "the thigh becomes a capsule");
    CheckNear(thigh.m_fRadius, 0.08f, 0.002f, "the capsule has the thigh's radius");
    CheckNear(thigh.m_fLength, 0.5f - 2.0f * 0.08f, 0.01f, "the capsule's caps end where the thigh ends");

    // in the bind space of the mesh the capsule stands upright in the middle of the thigh
    const plTransform shape = thigh.GetShapeTransform(thighJoint);
    const plVec3 vAxis = shape.m_qRotation * plVec3(1, 0, 0);
    CheckNear(plMath::Abs(vAxis.z), 1.0f, 1.0e-3f, "the capsule runs along the thigh");
    Check((shape.m_vPosition - plVec3(0, 0.2f, 0.75f)).GetLength() < 0.005f, "the capsule sits in the middle of the thigh");

    const plJoltClothCollider& head = colliders[1];
    Check(head.m_sJoint == plTempHashedString("Head") && head.m_Type == plJoltClothCollider::Type::Sphere, "the head becomes a sphere");
    Check((head.GetShapeTransform(headJoint).m_vPosition - plVec3(0, 0, 1.7f)).GetLength() < 0.01f, "the sphere sits in the head");
  }

  // with a cloth to fit for, only the shapes it can reach are kept: a cape behind the thigh, far from the head
  Cape cape;
  Check(MakeCape(cape, 6, 8), "a cape builds");

  plJoltClothDescriptor reach = cape.m_Desc;
  for (plVec3& vPosition : reach.m_Positions)
  {
    vPosition = plVec3(-0.15f, 0.2f + vPosition.y * 0.2f, 0.75f + vPosition.z * 0.1f);
  }

  colliders.Clear();
  Check(plJoltClothBuilder::FitCollidersFromSkin(positions, boneIndices, boneWeights, names, inverseBindPoses, options, &reach, colliders) == 1 && colliders[0].m_sJoint == plTempHashedString("Thigh"), "only the shape in reach of the cloth is kept");
}

/// Painted maps are stored with their positions and find their particles again after the sim mesh changed.
static void TestPaintedMaps()
{
  Cape cape;
  Check(MakeCape(cape, 8, 10), "a cape builds");

  plJoltClothPaintedMaps maps;
  plJoltClothBuilder::ExtractPaintedMaps(cape.m_Desc, maps);
  Check(maps.m_Positions.GetCount() == cape.m_Desc.GetParticleCount() && !maps.IsEmpty(), "the maps of a cloth can be taken as painted maps");

  for (plUInt32 p = 0; p < maps.m_MaxDistance.GetCount(); ++p)
  {
    maps.m_MaxDistance[p] = 0.01f * static_cast<float>(p);
  }
  maps.m_StiffnessScale.Clear(); // not painted

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  Check(maps.Serialize(writer).Succeeded(), "painted maps serialize");

  plJoltClothPaintedMaps loaded;
  plMemoryStreamReader reader(&storage);
  Check(loaded.Deserialize(reader).Succeeded(), "painted maps deserialize");
  Check(loaded.m_Positions.GetCount() == maps.m_Positions.GetCount() && loaded.m_StiffnessScale.IsEmpty(), "the round trip keeps which maps are painted");

  plJoltClothDescriptor same = cape.m_Desc;
  same.m_StiffnessScale[3] = 0.5f;
  plJoltClothBuilder::ApplyPaintedMaps(loaded, 0.02f, same);

  CheckNear(same.m_MaxDistance[17], 0.17f, 1.0e-6f, "a particle takes the painted value of its sample");
  CheckNear(same.m_StiffnessScale[3], 0.5f, 1.0e-6f, "a map that was not painted is left alone");

  // pinned particles keep their render vertices skinned
  bool bBlendFollows = true;
  for (const plClothVertexBinding& binding : same.m_Bindings)
  {
    const float fExpected = plMath::Saturate(same.m_MaxDistance[binding.m_uiParticle0] / 0.02f);
    if (plMath::Abs(binding.m_fBlend - fExpected) > 1.0e-4f)
      bBlendFollows = false;
  }
  Check(bBlendFollows, "the binding blend follows the painted max distance");

  // the same cape with more particles: every particle takes the value painted closest to it
  Cape dense;
  Check(MakeCape(dense, 15, 19), "a denser cape builds");

  plJoltClothBuilder::ApplyPaintedMaps(loaded, 0.02f, dense.m_Desc);

  float fMaxError = 0.0f;
  for (plUInt32 p = 0; p < dense.m_Desc.GetParticleCount(); ++p)
  {
    float fClosest = plMath::MaxValue<float>();
    float fExpected = 0.0f;

    for (plUInt32 s = 0; s < loaded.m_Positions.GetCount(); ++s)
    {
      const float fDistance = (loaded.m_Positions[s] - dense.m_Desc.m_Positions[p]).GetLengthSquared();
      if (fDistance < fClosest)
      {
        fClosest = fDistance;
        fExpected = loaded.m_MaxDistance[s];
      }
    }

    fMaxError = plMath::Max(fMaxError, plMath::Abs(dense.m_Desc.m_MaxDistance[p] - fExpected));
  }
  CheckNear(fMaxError, 0.0f, 1.0e-6f, "painted maps carry over to a sim mesh with other particles");
}

/// The brush, smooth, fill, gradient and auto mask operations of the paint tools.
static void TestMapTools()
{
  Cape cape;
  Check(MakeCape(cape, 9, 11), "a cape builds");

  // a second, separate piece next to the cape
  Cape two;
  AddCapeVertices(two, 0, 9, 9, 11);
  const plUInt32 uiFirstOfSecond = two.m_Positions.GetCount();
  AddCapeVertices(two, 0, 9, 9, 11);
  for (plUInt32 v = uiFirstOfSecond; v < two.m_Positions.GetCount(); ++v)
  {
    two.m_Positions[v].y += 5.0f;
  }

  plJoltClothMapTopology topology;
  topology.Build(two.m_Positions, two.m_Indices);

  Check(topology.m_uiIslandCount == 2, "two separate sheets are two pieces");
  Check(topology.GetNeighbours(0).GetCount() == 2 && topology.GetNeighbours(10).GetCount() == 6, "a corner has two neighbours, an inner particle six");

  const plUInt32 uiParticles = two.m_Positions.GetCount();
  const plUInt32 uiMiddle = 5 * 9 + 4; // row 5, column 4 of the first sheet

  // a ray from behind the cape onto its middle
  plJoltClothMapTools::RayHit hit;
  const plVec3 vTarget = two.m_Positions[uiMiddle] + plVec3(0, 0.01f, -0.01f);
  Check(plJoltClothMapTools::RayCast(topology, vTarget + plVec3(-2.0f, 0, 0), plVec3(1, 0, 0), hit), "a ray onto the cape hits it");
  CheckNear(hit.m_fDistance, 2.0f, 1.0e-4f, "the hit is where the ray meets the sheet");
  Check(hit.m_uiClosestParticle == uiMiddle, "the hit names the nearest particle");
  Check(!plJoltClothMapTools::RayCast(topology, plVec3(-2.0f, 2.5f, 1.0f), plVec3(1, 0, 0), hit), "a ray between the sheets hits nothing");

  plDynamicArray<float> map, start, weights;
  map.SetCount(uiParticles, 0.2f);
  start = map;
  weights.SetCount(uiParticles, 0.0f);

  plJoltClothBrush brush;
  brush.m_vCenter = two.m_Positions[uiMiddle];
  brush.m_fRadius = 0.25f;
  brush.m_fStrength = 0.5f;
  brush.m_fHardness = 0.5f;

  Check(plJoltClothMapTools::AccumulateStroke(topology, brush, weights), "the brush touches particles");
  Check(!plJoltClothMapTools::AccumulateStroke(topology, brush, weights), "holding the brush still adds nothing");

  plJoltClothMapTools::ApplyPaint(start, weights, 1.0f, map);
  CheckNear(map[uiMiddle], 0.6f, 1.0e-5f, "at half strength the brush moves the value half way");
  CheckNear(map[0], 0.2f, 1.0e-6f, "particles outside the brush keep their value");
  CheckNear(map[uiFirstOfSecond + uiMiddle], 0.2f, 1.0e-6f, "the other sheet is not touched");

  // smoothing pulls a spike towards its neighbours
  plDynamicArray<float> spike;
  spike.SetCount(uiParticles, 0.0f);
  spike[uiMiddle] = 1.0f;

  brush.m_fStrength = 1.0f;
  plJoltClothMapTools::Smooth(topology, brush, spike);
  CheckNear(spike[uiMiddle], 0.0f, 1.0e-5f, "smoothing takes a spike down to its neighbours' average");
  Check(spike[uiMiddle + 1] > 0.1f, "smoothing spreads the spike to its neighbours");

  plJoltClothMapTools::Fill(topology, topology.m_Island[uiFirstOfSecond], 0.7f, map);
  CheckNear(map[uiFirstOfSecond + 3], 0.7f, 1.0e-6f, "fill sets the whole piece");
  CheckNear(map[0], 0.2f, 1.0e-6f, "fill leaves the other piece alone");

  const plVec3 vTop = two.m_Positions[4];
  const plVec3 vBottom = two.m_Positions[10 * 9 + 4];
  plJoltClothMapTools::Gradient(topology, topology.m_Island[0], vTop, vBottom, 0.0f, 0.4f, map);
  CheckNear(map[4], 0.0f, 1.0e-5f, "the gradient starts at its first value");
  CheckNear(map[10 * 9 + 4], 0.4f, 1.0e-5f, "the gradient ends at its second value");
  CheckNear(map[5 * 9 + 4], 0.2f, 1.0e-4f, "half way down the gradient is half way between");

  plDynamicArray<float> mask;
  mask.SetCount(uiParticles, -1.0f);
  plJoltClothMapTools::AutoMask(topology, plVec3(0, 0, 1), 0.1f, 0.5f, mask);

  bool bMonotone = true;
  for (plUInt32 r = 0; r + 1 < 11; ++r)
  {
    if (mask[(r + 1) * 9 + 4] < mask[r * 9 + 4])
      bMonotone = false;
  }

  CheckNear(mask[4], 0.0f, 1.0e-6f, "auto mask pins the top");
  CheckNear(mask[10 * 9 + 4], 0.5f, 1.0e-3f, "auto mask gives the hem the full max distance");
  Check(bMonotone, "auto mask lets the cloth move further the lower it hangs");
  CheckNear(mask[uiFirstOfSecond + 10 * 9 + 4], 0.5f, 1.0e-3f, "auto mask treats every piece on its own");
}

/// The stress test passes a garment that is set up well and catches the faults it is there to catch.
static void TestStressTest()
{
  World test;

  Cape cape;
  Check(MakeCape(cape, 12, 16), "a cape builds");

  // This cape relies on its collider alone: no backstop keeps it out of the body.
  for (plUInt32 p = 0; p < cape.m_Desc.GetParticleCount(); ++p)
  {
    cape.m_Desc.m_BackstopDistance[p] = 10.0f;
  }

  // the torso: an upright capsule on the spine joint, 3 cm in front of the cape
  plJoltClothCollider torso;
  torso.m_sJoint.Assign("Spine");
  torso.m_Type = plJoltClothCollider::Type::Capsule;
  torso.m_JointBindPose = plTransform(plVec3(0, 0, 1.1f));
  torso.m_Transform.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(-90));
  torso.m_fRadius = 0.17f;
  torso.m_fLength = 0.6f;

  cape.m_Desc.m_Colliders.PushBack(torso);

  plJoltClothStressGates gates;

  // Development builds are not optimized, so only a Release build says anything about cost
  if (plOSFile::GetApplicationDirectory().FindSubString_NoCase("_Release") == nullptr)
    gates.m_fMaxSolveMs = 0.0f;

  auto Print = [](const char* szWhat, const plJoltClothStressResult& result)
  {
    const plJoltClothStressFrame& worst = result.m_Worst;
    std::printf("  stress %-22s stretch %5.1f %%  overshoot %.4f m  inside %3u  speed %5.1f m/s  solve %.3f ms  failed frames %u of %u\n", szWhat, worst.m_fStretch * 100.0f, worst.m_fOvershoot, worst.m_uiInsideBody, worst.m_fRelativeSpeed, worst.m_fSolveMs, result.m_uiFailedFrames, result.m_Frames.GetCount());
  };

  plJoltClothStressScriptOptions options;

  plJoltClothStressResult good;
  Check(plJoltClothStressTest::RunScript(test.GetJolt(), cape.m_Desc, {}, gates, options, good).Succeeded(), "the motion script runs");
  Print("good cape:", good);
  Check(good.m_Frames.GetCount() > 300, "the script covers standing, sprinting, stopping, spinning and a teleport");
  Check(good.Passed(), "a cape with its collider, tethers and teleport reset passes every gate");

  const plUInt32 uiBodies = test.GetJolt().GetJoltSystem()->GetNumBodies();

  // fault 1: the collider is missing from the garment, the body is still there
  {
    plJoltClothDescriptor desc = cape.m_Desc;
    desc.m_Colliders.Clear();

    plJoltClothStressResult result;
    Check(plJoltClothStressTest::RunScript(test.GetJolt(), desc, plMakeArrayPtr(&torso, 1), gates, options, result).Succeeded(), "the script runs without the collider");
    Print("missing collider:", result);

    Check((result.m_Worst.m_uiFailedGates & plJoltClothStressGate::InsideBody) != 0, "the test catches a missing collider");
    Check(result.FindFirstFailure(plJoltClothStressGate::InsideBody) != plInvalidIndex, "the test names the frame in which the cloth first went into the body");

    plDynamicArray<plString> suggestions;
    result.GetSuggestions(suggestions);
    Check(!suggestions.IsEmpty() && suggestions[0].FindSubString("Auto Select") != nullptr, "a cloth inside the body comes with the fix: Auto Select");
  }

  // fault 2: nothing holds the length of the cloth, neither tethers nor the stretch limit
  {
    plJoltClothDescriptor desc = cape.m_Desc;
    desc.m_Simulation.m_bTethers = false;
    desc.m_Simulation.m_fStretchLimit = 0.0f;

    plJoltClothStressResult result;
    Check(plJoltClothStressTest::RunScript(test.GetJolt(), desc, {}, gates, options, result).Succeeded(), "the script runs without tethers");
    Print("tethers off:", result);

    Check((result.m_Worst.m_uiFailedGates & plJoltClothStressGate::Stretch) != 0, "the test catches tethers and stretch limit that are switched off");
  }

  // the stretch limit alone keeps the cloth from stretching, also without tethers
  {
    plJoltClothDescriptor desc = cape.m_Desc;
    desc.m_Simulation.m_bTethers = false;

    plJoltClothStressResult result;
    Check(plJoltClothStressTest::RunScript(test.GetJolt(), desc, {}, gates, options, result).Succeeded(), "the script runs with the stretch limit only");
    Print("stretch limit only:", result);

    Check((result.m_Worst.m_uiFailedGates & plJoltClothStressGate::Stretch) == 0, "the stretch limit holds the cloth's length without tethers");
    Check(result.m_Worst.m_fStretch <= desc.m_Simulation.m_fStretchLimit + 0.03f, "no edge ends a step much beyond the stretch limit");
  }

  // fault 3: a teleport is not treated as one
  {
    plJoltClothStressScriptOptions noReset = options;
    noReset.m_fTeleportDistance = 1000.0f;

    plJoltClothStressResult result;
    Check(plJoltClothStressTest::RunScript(test.GetJolt(), cape.m_Desc, {}, gates, noReset, result).Succeeded(), "the script runs without teleport handling");
    Print("teleport reset off:", result);

    Check((result.m_Worst.m_uiFailedGates & plJoltClothStressGate::Pop) != 0, "the test catches a teleport that drags the cloth along");

    // a baseline of the good run makes this run a regression
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    Check(good.SerializeBaseline(writer).Succeeded(), "a baseline is written");

    plJoltClothStressFrame baseline;
    plMemoryStreamReader reader(&storage);
    Check(plJoltClothStressResult::DeserializeBaseline(reader, baseline).Succeeded(), "a baseline is read");

    plStringBuilder sWhy;
    Check(!good.IsWorseThan(baseline, 0.2f, sWhy), "a run is not worse than its own baseline");
    Check(result.IsWorseThan(baseline, 0.2f, sWhy) && !sWhy.IsEmpty(), "the faulty run is worse than the baseline, and the test says why");
  }

  Check(test.GetJolt().GetJoltSystem()->GetNumBodies() == uiBodies, "the script leaves no body behind");
}

void RunClothStressFile(const char* szClothFile, const char* szBaselineFile, bool bWriteBaseline)
{
  BeginGroup("cloth stress");

  // a transformed cloth asset: the asset header, the file version, then the cloth
  plJoltClothDescriptor desc;

  {
    plOSFile file;
    plDynamicArray<plUInt8> content;

    if (file.Open(szClothFile, plFileOpenMode::Read).Failed())
    {
      std::printf("  cannot open '%s'\n", szClothFile);
      Check(false, "the cloth file opens");
      return;
    }

    content.SetCountUninitialized(static_cast<plUInt32>(file.GetFileSize()));
    file.Read(content.GetData(), content.GetCount());

    plRawMemoryStreamReader reader(content.GetData(), content.GetCount());

    plAssetFileHeader header;
    plUInt8 uiVersion = 0;

    if (header.Read(reader).Failed())
    {
      Check(false, "the file has an asset header");
      return;
    }

    reader >> uiVersion;

    if (uiVersion != plJoltClothResource::FileVersion || desc.Deserialize(reader).Failed())
    {
      Check(false, "the file holds a cloth this build can read");
      return;
    }
  }

  World test;

  plJoltClothStressGates gates;
  if (plOSFile::GetApplicationDirectory().FindSubString_NoCase("_Release") == nullptr)
    gates.m_fMaxSolveMs = 0.0f;

  plJoltClothStressResult result;
  if (plJoltClothStressTest::RunScript(test.GetJolt(), desc, {}, gates, plJoltClothStressScriptOptions(), result).Failed())
  {
    Check(false, "the motion script runs on the cloth");
    return;
  }

  const plJoltClothStressFrame& worst = result.m_Worst;
  std::printf("  %u particles, %u triangles, %u collider(s), %u frames\n", desc.GetParticleCount(), desc.GetTriangleCount(), desc.m_Colliders.GetCount(), result.m_Frames.GetCount());
  std::printf("  stretch            %6.1f %%   gate %.0f %% for longer than %.2f s, or %.0f %% at any time\n", worst.m_fStretch * 100.0f, gates.m_fMaxStretch * 100.0f, gates.m_fStretchGraceTime, gates.m_fMaxStretchPeak * 100.0f);
  std::printf("  beyond max distance %.4f m   gate %.4f m\n", worst.m_fOvershoot, gates.m_fMaxOvershoot);
  std::printf("  inside the body    %6u     gate %u particles\n", worst.m_uiInsideBody, gates.m_uiMaxInsideBody);
  std::printf("  speed against body %6.1f m/s gate %.1f m/s\n", worst.m_fRelativeSpeed, gates.m_fMaxRelativeSpeed);
  std::printf("  solve              %6.3f ms  gate %.2f ms\n", worst.m_fSolveMs, gates.m_fMaxSolveMs);

  // where in the script the worst values are: the parts of the script by the time they start at
  {
    static const struct
    {
      float m_fStart;
      const char* m_szName;
    } s_Parts[] = {{0.0f, "stand"}, {1.0f, "sprint"}, {2.5f, "hard stop"}, {3.25f, "sprint backwards"}, {4.25f, "hard stop"}, {5.0f, "spin"}, {6.5f, "after the spin"}, {7.0f, "teleport"}};

    for (plUInt32 p = 0; p < PL_ARRAY_SIZE(s_Parts); ++p)
    {
      const float fEnd = p + 1 < PL_ARRAY_SIZE(s_Parts) ? s_Parts[p + 1].m_fStart : 1.0e9f;

      plJoltClothStressFrame part;
      plUInt32 uiFailed = 0;

      for (const plJoltClothStressFrame& frame : result.m_Frames)
      {
        if (frame.m_fTime < s_Parts[p].m_fStart - 1.0e-4f || frame.m_fTime >= fEnd - 1.0e-4f)
          continue;

        if (frame.m_fStretch > part.m_fStretch)
        {
          part.m_uiStretchedEdge[0] = frame.m_uiStretchedEdge[0];
          part.m_uiStretchedEdge[1] = frame.m_uiStretchedEdge[1];
        }

        part.m_fStretch = plMath::Max(part.m_fStretch, frame.m_fStretch);
        part.m_fRelativeSpeed = plMath::Max(part.m_fRelativeSpeed, frame.m_fRelativeSpeed);
        part.m_uiInsideBody = plMath::Max(part.m_uiInsideBody, frame.m_uiInsideBody);
        uiFailed += frame.m_uiFailedGates != 0 ? 1 : 0;
      }

      // the edge that stretched most: its length at rest and how far its two particles may leave their skinned place
      const plUInt32 a = part.m_uiStretchedEdge[0];
      const plUInt32 b = part.m_uiStretchedEdge[1];

      std::printf("    %-17s stretch %6.1f %%  inside %3u  speed %5.1f m/s  failed frames %3u  edge %u-%u, %.1f cm long, max distance %.1f and %.1f cm\n", s_Parts[p].m_szName, part.m_fStretch * 100.0f, part.m_uiInsideBody, part.m_fRelativeSpeed, uiFailed, a, b, (desc.m_Positions[a] - desc.m_Positions[b]).GetLength() * 100.0f, desc.m_MaxDistance[a] * 100.0f, desc.m_MaxDistance[b] * 100.0f);
    }
  }

  // the first particle that went into the body, and the shape it went into
  for (const plJoltClothStressFrame& frame : result.m_Frames)
  {
    if (frame.m_uiInsideParticle == plInvalidIndex)
      continue;

    const plUInt32 p = frame.m_uiInsideParticle;
    const plVec3& vPosition = desc.m_Positions[p];
    const char* szJoint = frame.m_uiInsideShape < desc.m_Colliders.GetCount() ? desc.m_Colliders[frame.m_uiInsideShape].m_sJoint.GetData() : "?";

    std::printf("    first inside the body at %.2f s: particle %u, bound at (%.3f, %.3f, %.3f), max distance %.1f cm, in the shape of joint '%s'\n", frame.m_fTime, p, vPosition.x, vPosition.y, vPosition.z, desc.m_MaxDistance[p] * 100.0f, szJoint);
    break;
  }

  plDynamicArray<plString> suggestions;
  result.GetSuggestions(suggestions);
  for (const plString& sSuggestion : suggestions)
  {
    std::printf("  -> %s\n", sSuggestion.GetData());
  }

  Check(result.Passed(), "the cloth passes every gate of the stress test");

  if (szBaselineFile == nullptr)
    return;

  if (bWriteBaseline)
  {
    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    result.SerializeBaseline(writer).IgnoreResult();

    plOSFile file;
    const bool bWritten = file.Open(szBaselineFile, plFileOpenMode::Write).Succeeded() && file.Write(storage.GetData(), storage.GetStorageSize32()).Succeeded();
    Check(bWritten, "the baseline is written");
    return;
  }

  plOSFile file;
  plDynamicArray<plUInt8> content;
  plJoltClothStressFrame baseline;

  if (file.Open(szBaselineFile, plFileOpenMode::Read).Failed())
  {
    Check(false, "the baseline file opens");
    return;
  }

  content.SetCountUninitialized(static_cast<plUInt32>(file.GetFileSize()));
  file.Read(content.GetData(), content.GetCount());

  plRawMemoryStreamReader reader(content.GetData(), content.GetCount());
  if (plJoltClothStressResult::DeserializeBaseline(reader, baseline).Failed())
  {
    Check(false, "the baseline file is one this build can read");
    return;
  }

  plStringBuilder sWhy;
  const bool bWorse = result.IsWorseThan(baseline, 0.2f, sWhy);
  if (bWorse)
    std::printf("  worse than the baseline: %s\n", sWhy.GetData());

  Check(!bWorse, "the cloth is no worse than its baseline");
}

/// The command line runner reads a transformed cloth asset, runs the script on it and keeps a baseline.
static void TestStressFileRunner()
{
  Cape cape;
  Check(MakeCape(cape, 10, 14), "a cape builds");

  plStringBuilder sFolder = plOSFile::GetTempDataFolder("JoltTests");
  plOSFile::CreateDirectoryStructure(sFolder).IgnoreResult();

  plStringBuilder sClothFile = sFolder;
  sClothFile.AppendPath("StressCape.plBinJoltCloth");

  plStringBuilder sBaselineFile = sFolder;
  sBaselineFile.AppendPath("StressCape.plClothBaseline");

  // what the cloth asset's transform writes
  {
    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);

    plAssetFileHeader header;
    header.SetFileHashAndVersion(1, 1);
    header.Write(writer).IgnoreResult();

    writer << plJoltClothResource::FileVersion;
    Check(cape.m_Desc.Serialize(writer).Succeeded(), "the cape serializes");

    plOSFile file;
    Check(file.Open(sClothFile, plFileOpenMode::Write).Succeeded() && file.Write(storage.GetData(), storage.GetStorageSize32()).Succeeded(), "the cloth file is written");
  }

  RunClothStressFile(sClothFile, sBaselineFile, true);
  RunClothStressFile(sClothFile, sBaselineFile, false);

  BeginGroup("cloth");
  Check(plOSFile::ExistsFile(sBaselineFile), "the runner leaves a baseline file next to the cloth");

  plOSFile::DeleteFile(sClothFile).IgnoreResult();
  plOSFile::DeleteFile(sBaselineFile).IgnoreResult();
}

void RunClothTests()
{
  BeginGroup("cloth");

  TestBuilderWeldsSeams();
  TestBuilderBindsToProxy();
  TestImportedMasks();
  TestSkeletonColliders();
  TestFitCollidersFromSkin();
  TestBoneChainStandIn();
  TestPaintedMaps();
  TestMapTools();
  TestDescriptorRoundTrip();
  TestMaxDistanceHoldsOnSpinningJoint();
  TestClothTeleport();
  TestFrameRateIndependence();
  TestScaledAndMirroredBindSpace();
  TestBackstop();
  TestBodyColliders();
  TestWorldCollision();
  TestInstanceChurn();
  TestComponent();
  TestLodAndBudget();
  TestStressTest();
  TestCostAndParallelStepping();
  TestStressFileRunner();
}
