#pragma once

#include <Core/Interfaces/PhysicsQuery.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/Startup.h>
#include <JoltPlugin/Actors/JoltDynamicActorComponent.h>
#include <JoltPlugin/Actors/JoltStaticActorComponent.h>
#include <JoltPlugin/Shapes/JoltShapeBoxComponent.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <cstdio>

/// \file
/// Shared pieces of the headless Jolt tests: result counting, a world fixture and scene builders.

namespace JoltTest
{
  /// \brief How a failing check is treated.
  enum class Category
  {
    /// Must pass. A failure fails the run.
    Regression,
    /// Describes behaviour that an R1 fix is supposed to deliver. A failure is reported as an open item and only fails the run with --strict.
    R1Gate,
  };

  /// \brief Records one check. Prints a line for every failure.
  void Check(bool bValue, const char* szMessage, Category category = Category::Regression);

  /// \brief Records a check that |fActual - fExpected| <= fTolerance. The values are printed on failure.
  void CheckNear(float fActual, float fExpected, float fTolerance, const char* szMessage, Category category = Category::Regression);

  /// \brief Names the test group that the following checks belong to.
  void BeginGroup(const char* szName);

  struct Totals
  {
    plUInt32 m_uiChecks = 0;
    plUInt32 m_uiFailures = 0;
    plUInt32 m_uiOpenR1Items = 0;
  };

  const Totals& GetTotals();

  /// \brief A world that updates with a fixed time step and has the Jolt world module.
  class World
  {
  public:
    /// \param fFrameRate The frame rate the world clock advances with. Physics uses its own stepping on top of that.
    explicit World(float fFrameRate = 60.0f);
    ~World();

    plWorld& GetWorld() { return *m_pWorld; }
    plJoltWorldModule& GetJolt() { return *m_pJolt; }
    float GetFrameRate() const { return m_fFrameRate; }

    /// \brief Updates the world for the given number of frames.
    void Step(plUInt32 uiFrames);

    /// \brief Updates the world for the given amount of simulated seconds.
    void StepSeconds(float fSeconds);

    plGameObject* CreateObject(const plVec3& vPosition, const plQuat& qRotation = plQuat::MakeIdentity());

    plVec3 GetPosition(const plGameObjectHandle& hObject);

    /// \brief A static box that is centered at the position and has the given half extents.
    plGameObject* AddStaticBox(const plVec3& vPosition, const plVec3& vHalfExtents, plUInt8 uiLayer = 0, const plQuat& qRotation = plQuat::MakeIdentity());

    /// \brief A dynamic box with the default mass. The gravity factor scales gravity for this body.
    plGameObject* AddDynamicBox(const plVec3& vPosition, const plVec3& vHalfExtents, plUInt8 uiLayer = 0, float fGravityFactor = 1.0f);

    /// \brief Gives the body a velocity by an impulse. Only valid after the body was created, i.e. after a Step().
    void SetVelocity(plGameObject* pObject, const plVec3& vVelocity);

    /// \brief A large static floor whose top is at z = 0.
    plGameObject* AddFloor(plUInt8 uiLayer = 0);

  private:
    float m_fFrameRate = 60.0f;
    plUniquePtr<plWorld> m_pWorld;
    plJoltWorldModule* m_pJolt = nullptr;
  };

  /// \brief Returns the default query parameters that find every kind of shape.
  plPhysicsQueryParameters AllShapes(plUInt32 uiLayer = 0);

  /// \brief Starts the engine systems the tests need. Call once before any test.
  void Startup();

  /// \brief Shuts down what Startup() started.
  void Shutdown();
} // namespace JoltTest

/// The test groups. Each starts its own worlds and records its checks through JoltTest::Check().
void RunP0Regressions();
void RunStackingTests();
void RunLayerTests();
void RunTriggerTests();
void RunQueryTests();
void RunBreakTests();
void RunBuoyancyTests();
void RunCharacterTests();
void RunLifecycleTests();

/// \brief Runs the soak scene for the given amount of wall-clock seconds. Returns the number of simulated frames.
plUInt64 RunStressScene(double fSeconds);
