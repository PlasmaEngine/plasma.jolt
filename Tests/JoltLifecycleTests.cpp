#include "JoltTestSupport.h"
#include <JoltPlugin/Components/JoltRagdollComponent.h>

using namespace JoltTest;

/// Creating and deleting actors while the simulation runs must not leave anything behind that the step touches.
static void TestActorChurn()
{
  World test;
  test.AddFloor();

  plHybridArray<plGameObjectHandle, 64> live;
  plUInt32 uiSeed = 4711;

  for (plUInt32 uiFrame = 0; uiFrame < 600; ++uiFrame)
  {
    uiSeed = uiSeed * 1664525u + 1013904223u;

    if (live.GetCount() < 48 && (uiSeed >> 24) % 2 == 0)
    {
      const float x = static_cast<float>((uiSeed >> 8) % 20) - 10.0f;
      live.PushBack(test.AddDynamicBox(plVec3(x, 0, 3.0f), plVec3(0.4f))->GetHandle());
    }
    else if (!live.IsEmpty() && (uiSeed >> 20) % 3 == 0)
    {
      const plUInt32 uiIndex = (uiSeed >> 4) % live.GetCount();

      PL_LOCK(test.GetWorld().GetWriteMarker());
      test.GetWorld().DeleteObjectNow(live[uiIndex]);
      live.RemoveAtAndSwap(uiIndex);
    }

    test.Step(1);
  }

  bool bAllValid = true;
  for (const plGameObjectHandle& hBox : live)
    bAllValid = bAllValid && test.GetPosition(hBox).IsValid();

  Check(bAllValid, "all remaining bodies have valid transforms after churn");
}

/// Actors that fall asleep and are deleted afterwards are removed from the sleeping bookkeeping.
static void TestDeletingSleepingActors()
{
  World test;
  test.AddFloor();

  plHybridArray<plGameObjectHandle, 16> boxes;
  for (plUInt32 i = 0; i < 16; ++i)
    boxes.PushBack(test.AddDynamicBox(plVec3(static_cast<float>(i) * 2.0f, 0, 0.5f), plVec3(0.5f))->GetHandle());

  test.StepSeconds(3.0f);
  Check(test.GetJolt().GetActiveActors().GetCount() == 0, "the boxes at rest are asleep");

  {
    PL_LOCK(test.GetWorld().GetWriteMarker());
    for (const plGameObjectHandle& hBox : boxes)
      test.GetWorld().DeleteObjectNow(hBox);
  }

  test.StepSeconds(1.0f);
  Check(test.GetJolt().GetActiveActors().GetCount() == 0, "no deleted actor stays in the active set");
}

/// Ragdoll components can be created and removed repeatedly, also without a skeleton to build limbs from.
static void TestRagdollComponentChurn()
{
  World test;
  test.AddFloor();

  for (plUInt32 uiRound = 0; uiRound < 10; ++uiRound)
  {
    plHybridArray<plGameObjectHandle, 8> objects;
    for (plUInt32 i = 0; i < 8; ++i)
    {
      plGameObject* pObject = test.CreateObject(plVec3(static_cast<float>(i) * 2.0f, 0, 1.0f));
      objects.PushBack(pObject->GetHandle());

      PL_LOCK(test.GetWorld().GetWriteMarker());
      plJoltRagdollComponent* pRagdoll = nullptr;
      plJoltRagdollComponent::CreateComponent(pObject, pRagdoll);
    }

    test.Step(5);

    PL_LOCK(test.GetWorld().GetWriteMarker());
    for (const plGameObjectHandle& hObject : objects)
      test.GetWorld().DeleteObjectNow(hObject);
  }

  test.Step(5);
  Check(test.GetJolt().GetActiveRagdolls().GetCount() == 0, "no deleted ragdoll stays registered");
}

/// A world with Jolt bodies can be destroyed while bodies are still awake and queued.
static void TestWorldTeardownWithLiveBodies()
{
  for (plUInt32 uiRound = 0; uiRound < 5; ++uiRound)
  {
    World test;
    test.AddFloor();

    for (plUInt32 i = 0; i < 32; ++i)
      test.AddDynamicBox(plVec3(static_cast<float>(i % 8), static_cast<float>(i / 8), 2.0f + static_cast<float>(i)), plVec3(0.4f));

    test.Step(10);
  }

  Check(true, "worlds with live bodies shut down without a crash");
}

void RunLifecycleTests()
{
  BeginGroup("Lifecycle");

  TestActorChurn();
  TestDeletingSleepingActors();
  TestRagdollComponentChurn();
  TestWorldTeardownWithLiveBodies();
}
