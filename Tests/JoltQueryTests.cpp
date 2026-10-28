#include "JoltTestSupport.h"

using namespace JoltTest;

/// Scene shared by the query tests: a 2 m box at the origin and a second one 10 m further along +x.
class QueryScene
{
public:
  QueryScene()
  {
    m_pBoxA = m_World.AddStaticBox(plVec3(0, 0, 0), plVec3(1.0f));
    m_pBoxB = m_World.AddStaticBox(plVec3(10, 0, 0), plVec3(1.0f));
    m_World.Step(3);
  }

  plJoltWorldModule& Jolt() { return m_World.GetJolt(); }

  World m_World;
  plGameObject* m_pBoxA = nullptr;
  plGameObject* m_pBoxB = nullptr;
};

static void TestRaycast()
{
  QueryScene scene;
  plPhysicsCastResult result;

  Check(scene.Jolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 20.0f, AllShapes()), "ray hits the first box");
  CheckNear(result.m_fDistance, 4.0f, 0.02f, "ray hit distance");
  CheckNear(result.m_vPosition.x, -1.0f, 0.02f, "ray hit position");
  CheckNear(result.m_vNormal.x, -1.0f, 0.05f, "ray hit normal faces the ray");
  Check(result.m_hActorObject == scene.m_pBoxA->GetHandle(), "ray hit reports the actor object");

  Check(!scene.Jolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 3.0f, AllShapes()), "ray that is too short misses");
  Check(!scene.Jolt().Raycast(result, plVec3(-5, 5, 0), plVec3(1, 0, 0), 20.0f, AllShapes()), "ray beside the boxes misses");
  Check(scene.Jolt().Raycast(result, plVec3(-5, 0, 0), plVec3(1, 0, 0), 20.0f, AllShapes(), plPhysicsHitCollection::Any), "any-hit ray reports a hit");
}

static void TestRaycastAll()
{
  QueryScene scene;
  plPhysicsCastResultArray results;

  Check(scene.Jolt().RaycastAll(results, plVec3(-5, 0, 0), plVec3(1, 0, 0), 30.0f, AllShapes()), "ray through both boxes hits");
  Check(results.m_Results.GetCount() == 2, "ray through both boxes reports two hits");

  Check(!scene.Jolt().RaycastAll(results, plVec3(-5, 5, 0), plVec3(1, 0, 0), 30.0f, AllShapes()), "ray beside the boxes reports nothing");
}

static void TestSweeps()
{
  QueryScene scene;
  plPhysicsCastResult result;

  const plVec3 vDir(1, 0, 0);
  const plTransform start = plTransform(plVec3(-5, 0, 0));

  Check(scene.Jolt().SweepTestSphere(result, 0.25f, plVec3(-5, 0, 0), vDir, 20.0f, AllShapes()), "sphere sweep hits");
  CheckNear(result.m_fDistance, 3.75f, 0.05f, "sphere sweep distance");
  Check(!scene.Jolt().SweepTestSphere(result, 0.25f, plVec3(-5, 0, 0), vDir, 3.0f, AllShapes()), "short sphere sweep misses");

  Check(scene.Jolt().SweepTestBox(result, plVec3(0.5f), start, vDir, 20.0f, AllShapes()), "box sweep hits");
  CheckNear(result.m_fDistance, 3.75f, 0.05f, "box sweep distance");

  Check(scene.Jolt().SweepTestCapsule(result, 0.25f, 1.0f, start, vDir, 20.0f, AllShapes()), "capsule sweep hits");
  CheckNear(result.m_fDistance, 3.75f, 0.1f, "capsule sweep distance");

  Check(scene.Jolt().SweepTestCylinder(result, 0.25f, 1.0f, start, vDir, 20.0f, AllShapes()), "cylinder sweep hits");
  CheckNear(result.m_fDistance, 3.75f, 0.1f, "cylinder sweep distance");
}

/// A sweep that starts inside a shape reports the initial overlap unless told to ignore it.
static void TestSweepInitialOverlap()
{
  QueryScene scene;
  plPhysicsCastResult result;

  Check(scene.Jolt().SweepTestSphere(result, 0.25f, plVec3(0, 0, 0), plVec3(1, 0, 0), 5.0f, AllShapes()), "sweep from inside a box reports the overlap");

  plPhysicsQueryParameters ignore = AllShapes();
  ignore.m_bIgnoreInitialOverlap = true;
  Check(!scene.Jolt().SweepTestSphere(result, 0.25f, plVec3(0, 0, 0), plVec3(1, 0, 0), 0.5f, ignore), "sweep from inside a box that ignores the overlap finds nothing within the box", Category::R1Gate);
}

static void TestOverlapTests()
{
  QueryScene scene;
  const plPhysicsQueryParameters params = AllShapes();

  Check(scene.Jolt().OverlapTestSphere(0.5f, plVec3(1.2f, 0, 0), params), "sphere overlaps the box");
  Check(!scene.Jolt().OverlapTestSphere(0.5f, plVec3(4, 0, 0), params), "sphere between the boxes overlaps nothing");

  // the box is placed by its transform; the position argument is not used
  const plTransform atBox = plTransform(plVec3(10, 0, 0));
  const plTransform atGap = plTransform(plVec3(5, 0, 0));
  Check(scene.Jolt().OverlapTestBox(plVec3(1.0f), atBox.m_vPosition, atBox, params), "box overlaps the second box");
  Check(!scene.Jolt().OverlapTestBox(plVec3(1.0f), atGap.m_vPosition, atGap, params), "box in the gap overlaps nothing");
  Check(scene.Jolt().OverlapTestBox(plVec3(1.0f), plVec3(1000, 0, 0), atBox, params), "box overlap uses the transform, not the position argument");
  Check(!scene.Jolt().OverlapTestBox(plVec3(1.0f), plVec3(10, 0, 0), atGap, params), "box overlap does not use the position argument");

  const plTransform nearBox = plTransform(plVec3(1.2f, 0, 0));
  const plTransform farAway = plTransform(plVec3(5, 0, 0));
  Check(scene.Jolt().OverlapTestCapsule(0.25f, 1.0f, nearBox, params), "capsule overlaps the box");
  Check(!scene.Jolt().OverlapTestCapsule(0.25f, 1.0f, farAway, params), "capsule in the gap overlaps nothing");
  Check(scene.Jolt().OverlapTestCylinder(0.25f, 1.0f, nearBox, params), "cylinder overlaps the box");
  Check(!scene.Jolt().OverlapTestCylinder(0.25f, 1.0f, farAway, params), "cylinder in the gap overlaps nothing");
}

static void TestShapeQueries()
{
  QueryScene scene;
  const plPhysicsQueryParameters params = AllShapes();
  plPhysicsOverlapResultArray results;

  scene.Jolt().QueryShapesInSphere(results, 0.5f, plVec3(1.2f, 0, 0), params);
  Check(results.m_Results.GetCount() == 1, "sphere query finds one box");
  if (results.m_Results.GetCount() == 1)
    Check(results.m_Results[0].m_hActorObject == scene.m_pBoxA->GetHandle(), "sphere query reports the actor object");

  scene.Jolt().QueryShapesInSphere(results, 20.0f, plVec3(5, 0, 0), params);
  Check(results.m_Results.GetCount() == 2, "large sphere query finds both boxes");

  const plTransform wide = plTransform(plVec3(5, 0, 0));
  scene.Jolt().QueryShapesInBox(results, plVec3(30.0f, 4.0f, 4.0f), wide, params);
  Check(results.m_Results.GetCount() == 2, "box query finds both boxes");

  scene.Jolt().QueryShapesInBox(results, plVec3(2.0f, 2.0f, 2.0f), wide, params);
  Check(results.m_Results.IsEmpty(), "box query in the gap finds nothing");

  const plTransform atSecond = plTransform(plVec3(10, 0, 0));
  scene.Jolt().QueryShapesInCapsule(results, 0.5f, 1.0f, atSecond, params);
  Check(results.m_Results.GetCount() == 1, "capsule query finds one box");

  scene.Jolt().QueryShapesInCylinder(results, 0.5f, 1.0f, atSecond, params);
  Check(results.m_Results.GetCount() == 1, "cylinder query finds one box");
}

static void TestWorldSpaceBounds()
{
  QueryScene scene;

  PL_LOCK(scene.m_World.GetWorld().GetReadMarker());
  const plBoundingBoxSphere bounds = scene.Jolt().GetWorldSpaceBounds(scene.m_pBoxB, 0, plPhysicsShapeType::Default, false);

  Check(bounds.IsValid(), "the box has world space bounds");
  CheckNear(bounds.GetBox().GetCenter().x, 10.0f, 0.05f, "bounds center");
  CheckNear(bounds.GetBox().GetHalfExtents().x, 1.0f, 0.05f, "bounds half extent");
}

/// The queries answer for a body that moved, not for where it was created.
static void TestQueriesFollowDynamicBodies()
{
  World test;
  test.AddFloor();
  test.AddDynamicBox(plVec3(0, 0, 8.0f), plVec3(0.5f));
  test.Step(3);

  plPhysicsCastResult result;
  Check(!test.GetJolt().Raycast(result, plVec3(-5, 0, 0.5f), plVec3(1, 0, 0), 10.0f, AllShapes()), "the box is not resting on the floor yet");

  test.StepSeconds(3.0f);
  Check(test.GetJolt().Raycast(result, plVec3(-5, 0, 0.5f), plVec3(1, 0, 0), 10.0f, AllShapes()), "the landed box is found where it came to rest");
}

void RunQueryTests()
{
  BeginGroup("Queries");

  TestRaycast();
  TestRaycastAll();
  TestSweeps();
  TestSweepInitialOverlap();
  TestOverlapTests();
  TestShapeQueries();
  TestWorldSpaceBounds();
  TestQueriesFollowDynamicBodies();
}
