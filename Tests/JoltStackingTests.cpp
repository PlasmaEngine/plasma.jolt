#include "JoltTestSupport.h"

using namespace JoltTest;

/// A tower of boxes that start almost touching must stay where it is and fall asleep.
static void TestTowerComesToRest()
{
  World test;
  test.AddFloor();

  const plUInt32 uiBoxes = 5;
  plHybridArray<plGameObjectHandle, 8> boxes;
  for (plUInt32 i = 0; i < uiBoxes; ++i)
    boxes.PushBack(test.AddDynamicBox(plVec3(0, 0, 0.5f + 1.001f * i), plVec3(0.5f))->GetHandle());

  test.StepSeconds(10.0f);

  for (plUInt32 i = 0; i < uiBoxes; ++i)
  {
    const plVec3 vPos = test.GetPosition(boxes[i]);
    CheckNear(vPos.z, 0.5f + static_cast<float>(i), 0.05f, "box rests at its stack height");
    Check(plMath::Abs(vPos.x) < 0.05f && plMath::Abs(vPos.y) < 0.05f, "box does not drift sideways");
  }

  Check(test.GetJolt().GetActiveActors().GetCount() == 0, "the resting tower is asleep");
}

/// A box dropped onto the floor lands at the floor height and does not tunnel.
static void TestDropLandsOnFloor()
{
  World test;
  test.AddFloor();
  const plGameObjectHandle hBox = test.AddDynamicBox(plVec3(0, 0, 5.0f), plVec3(0.5f))->GetHandle();

  test.StepSeconds(3.0f);

  CheckNear(test.GetPosition(hBox).z, 0.5f, 0.05f, "dropped box rests on the floor");
}

/// A pile of many boxes keeps every body above the floor and inside the world.
static void TestPileDoesNotTunnel()
{
  World test;
  test.AddFloor();

  plHybridArray<plGameObjectHandle, 64> boxes;
  for (plUInt32 i = 0; i < 64; ++i)
  {
    const float x = static_cast<float>(i % 4) * 1.2f;
    const float y = static_cast<float>((i / 4) % 4) * 1.2f;
    const float z = 1.0f + static_cast<float>(i / 16) * 1.5f;
    boxes.PushBack(test.AddDynamicBox(plVec3(x, y, z), plVec3(0.5f))->GetHandle());
  }

  test.StepSeconds(8.0f);

  bool bAllAboveFloor = true;
  for (const plGameObjectHandle& hBox : boxes)
  {
    const plVec3 vPos = test.GetPosition(hBox);
    bAllAboveFloor = bAllAboveFloor && vPos.z > 0.3f && vPos.IsValid();
  }

  Check(bAllAboveFloor, "no box of a 64 box pile fell through the floor");
}

PL_CREATE_SIMPLE_TEST_GROUP(JoltStacking);

PL_CREATE_SIMPLE_TEST(JoltStacking, TowerComesToRest)
{
  TestTowerComesToRest();
}

PL_CREATE_SIMPLE_TEST(JoltStacking, DropLandsOnFloor)
{
  TestDropLandsOnFloor();
}

PL_CREATE_SIMPLE_TEST(JoltStacking, PileDoesNotTunnel)
{
  TestPileDoesNotTunnel();
}

/// Runs a pyramid of ten boxes at the given world frame rate and returns the height of the top box. Every box must rest at its row height.
static float SettlePyramid(float fFrameRate)
{
  World test(fFrameRate);
  test.AddFloor();

  plHybridArray<plGameObjectHandle, 16> boxes;
  plHybridArray<float, 16> heights;
  for (plUInt32 uiRow = 0; uiRow < 4; ++uiRow)
  {
    const plUInt32 uiCount = 4 - uiRow;
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      const float x = (static_cast<float>(i) - 0.5f * static_cast<float>(uiCount - 1)) * 1.02f;
      boxes.PushBack(test.AddDynamicBox(plVec3(x, 0, 0.5f + static_cast<float>(uiRow) * 1.001f), plVec3(0.5f))->GetHandle());
      heights.PushBack(0.5f + static_cast<float>(uiRow));
    }
  }

  test.StepSeconds(6.0f);

  bool bAtRest = true;
  for (plUInt32 i = 0; i < boxes.GetCount(); ++i)
    bAtRest = bAtRest && plMath::Abs(test.GetPosition(boxes[i]).z - heights[i]) < 0.1f;

  Check(bAtRest, "every box of the pyramid rests at its row height");
  return test.GetPosition(boxes.PeekBack()).z;
}

/// The pyramid settles to the same height with the world clock at 30, 60 and 144 Hz.
static void TestPyramidAtFrameRates()
{
  const float f30 = SettlePyramid(30.0f);
  const float f60 = SettlePyramid(60.0f);
  const float f144 = SettlePyramid(144.0f);

  CheckNear(f30, f60, 0.05f, "pyramid top height agrees at 30 and 60 fps");
  CheckNear(f144, f60, 0.05f, "pyramid top height agrees at 144 and 60 fps");
}

PL_CREATE_SIMPLE_TEST(JoltStacking, PyramidAtFrameRates)
{
  TestPyramidAtFrameRates();
}

/// Deleting the box under a sleeping tower wakes the rest, which then fall onto the floor.
static void TestRemovingBaseWakesStack()
{
  World test;
  test.AddFloor();

  plHybridArray<plGameObjectHandle, 3> boxes;
  for (plUInt32 i = 0; i < 3; ++i)
    boxes.PushBack(test.AddDynamicBox(plVec3(0, 0, 0.5f + 1.001f * static_cast<float>(i)), plVec3(0.5f))->GetHandle());

  test.StepSeconds(6.0f);
  Check(test.GetJolt().GetActiveActors().GetCount() == 0, "the tower is asleep before its base is removed");

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    test.GetWorld().DeleteObjectNow(boxes[0]);
  }

  test.StepSeconds(3.0f);

  Check(!test.GetPosition(boxes[0]).IsValid(), "the base box was deleted");

  // A sleeping box is not woken when the body it rests on is removed, so it keeps floating. There is no roadmap item for this yet.
  OpenDefectNear("UNLISTED-wake-on-remove", test.GetPosition(boxes[1]).z, 0.5f, 0.1f, "the middle box fell to the floor");
  OpenDefectNear("UNLISTED-wake-on-remove", test.GetPosition(boxes[2]).z, 1.5f, 0.1f, "the top box fell with it");
}

PL_CREATE_SIMPLE_TEST(JoltStacking, RemovingBaseWakesStack)
{
  TestRemovingBaseWakesStack();
}

/// Drops a box at 60 m/s onto a 10 cm slab and returns its height one second later.
static float DropFastBox(bool bCCD)
{
  World test;
  test.AddStaticBox(plVec3(0, 0, 0), plVec3(5.0f, 5.0f, 0.05f));

  plGameObject* pBox = test.AddDynamicBox(plVec3(0, 0, 3.0f), plVec3(0.25f), 0, 0.0f);
  const plGameObjectHandle hBox = pBox->GetHandle();

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    plJoltDynamicActorComponent* pActor = nullptr;
    pBox->TryGetComponentOfBaseType(pActor);
    pActor->m_bCCD = bCCD;
  }

  test.Step(3);
  test.SetVelocity(pBox, plVec3(0, 0, -60.0f));
  test.StepSeconds(1.0f);
  return test.GetPosition(hBox).z;
}

/// With continuous collision detection a fast box cannot get through a thin slab.
static void TestFastBoxWithCCD()
{
  const float fWithoutCCD = DropFastBox(false);
  plTestFramework::Output(plTestOutput::Message, "fast box without CCD ends at z = %.3f (not asserted)", fWithoutCCD);

  Check(DropFastBox(true) > 0.0f, "a fast box with CCD stays above a 10 cm slab");
}

PL_CREATE_SIMPLE_TEST(JoltStacking, FastBoxWithCCD)
{
  TestFastBoxWithCCD();
}
