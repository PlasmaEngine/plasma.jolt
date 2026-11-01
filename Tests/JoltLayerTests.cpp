#include "JoltTestSupport.h"

using namespace JoltTest;

/// Two dynamic bodies on layers that do not collide fall through each other, bodies on the same layer stack.
static void TestSimulationFiltering()
{
  LayerConfig layers;

  World test;
  test.AddFloor();

  const plGameObjectHandle hLower = test.AddDynamicBox(plVec3(0, 0, 0.5f), plVec3(0.5f), 1)->GetHandle();
  const plGameObjectHandle hUpperOther = test.AddDynamicBox(plVec3(0, 0, 3.0f), plVec3(0.5f), 2)->GetHandle();

  const plGameObjectHandle hLowerSame = test.AddDynamicBox(plVec3(10, 0, 0.5f), plVec3(0.5f), 1)->GetHandle();
  const plGameObjectHandle hUpperSame = test.AddDynamicBox(plVec3(10, 0, 3.0f), plVec3(0.5f), 1)->GetHandle();

  test.StepSeconds(4.0f);

  CheckNear(test.GetPosition(hUpperOther).z, test.GetPosition(hLower).z, 0.1f, "bodies on non-colliding layers end up at the same height");
  CheckNear(test.GetPosition(hUpperSame).z, test.GetPosition(hLowerSame).z + 1.0f, 0.1f, "bodies on the same layer stack");
}

/// Queries only find shapes on layers the query layer collides with.
static void TestQueryFiltering()
{
  LayerConfig layers;

  World test;
  test.AddStaticBox(plVec3(0, 0, 0), plVec3(1.0f), 1);
  test.AddStaticBox(plVec3(10, 0, 0), plVec3(1.0f), 2);
  test.Step(3);

  plPhysicsCastResult result;

  Check(test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 30.0f, AllShapes(1)), "a layer 1 ray finds the layer 1 box");
  CheckNear(result.m_fDistance, 4.0f, 0.05f, "the layer 1 ray stops at the first box");

  Check(!test.GetJolt().Raycast(result, plVec3(5, 0, 0), plVec3(1, 0, 0), 20.0f, AllShapes(1)), "a layer 1 ray does not see the layer 2 box");
  Check(test.GetJolt().Raycast(result, plVec3(5, 0, 0), plVec3(1, 0, 0), 20.0f, AllShapes(2)), "a layer 2 ray finds the layer 2 box");

  Check(test.GetJolt().OverlapTestSphere(0.5f, plVec3(10, 0, 0), AllShapes(2)), "a layer 2 overlap test finds the layer 2 box");
  Check(!test.GetJolt().OverlapTestSphere(0.5f, plVec3(10, 0, 0), AllShapes(1)), "a layer 1 overlap test does not find the layer 2 box");
}

/// Queries can be restricted to shape types.
static void TestShapeTypeFiltering()
{
  World test;
  test.AddStaticBox(plVec3(0, 0, 0), plVec3(1.0f));
  test.Step(3);

  plPhysicsCastResult result;
  Check(test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, plPhysicsQueryParameters(0, plPhysicsShapeType::Static)), "static filter finds a static box");
  Check(!test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, plPhysicsQueryParameters(0, plPhysicsShapeType::Dynamic)), "dynamic filter skips a static box");
}

/// The collision group a body is filtered with matches the layer of its actor, and ignoring an object works.
static void TestIgnoreObjectFilter()
{
  World test;
  test.AddStaticBox(plVec3(0, 0, 0), plVec3(1.0f));
  test.Step(3);

  plPhysicsCastResult result;
  Check(test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, AllShapes()), "the ray hits the box");

  plPhysicsQueryParameters ignore = AllShapes();
  ignore.m_uiIgnoreObjectFilterID = result.m_uiObjectFilterID;

  Check(!test.GetJolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 10.0f, ignore), "the ray skips the ignored object");
}

PL_CREATE_SIMPLE_TEST_GROUP(JoltLayers);

PL_CREATE_SIMPLE_TEST(JoltLayers, SimulationFiltering)
{
  TestSimulationFiltering();
}

PL_CREATE_SIMPLE_TEST(JoltLayers, QueryFiltering)
{
  TestQueryFiltering();
}

PL_CREATE_SIMPLE_TEST(JoltLayers, ShapeTypeFiltering)
{
  TestShapeTypeFiltering();
}

PL_CREATE_SIMPLE_TEST(JoltLayers, IgnoreObjectFilter)
{
  TestIgnoreObjectFilter();
}

/// Layers that do not collide with themselves let bodies pass through each other, and still land on the default layer.
static void TestNoSelfCollision()
{
  LayerConfig layers;
  layers.Allow(1, 1, false);

  World test;
  test.AddFloor();

  const plGameObjectHandle hA = test.AddDynamicBox(plVec3(0, 0, 0.5f), plVec3(0.5f), 1)->GetHandle();
  const plGameObjectHandle hB = test.AddDynamicBox(plVec3(0, 0, 0.5f), plVec3(0.5f), 1)->GetHandle();

  test.StepSeconds(3.0f);

  const plVec3 vA = test.GetPosition(hA);
  const plVec3 vB = test.GetPosition(hB);
  CheckNear(vA.z, 0.5f, 0.05f, "the first body lands on the default layer floor");
  CheckNear(vB.z, 0.5f, 0.05f, "the second body lands on the default layer floor");
  Check((vA - vB).GetLength() < 0.05f, "bodies of a layer that does not collide with itself stay overlapped");
}

PL_CREATE_SIMPLE_TEST(JoltLayers, NoSelfCollision)
{
  TestNoSelfCollision();
}
