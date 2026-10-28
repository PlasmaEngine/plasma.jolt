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

void RunStackingTests()
{
  BeginGroup("Stacking");

  TestTowerComesToRest();
  TestDropLandsOnFloor();
  TestPileDoesNotTunnel();
}
