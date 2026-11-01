#pragma once

#include <Core/Interfaces/PhysicsQuery.h>
#include <Core/World/World.h>
#include <JoltPlugin/Actors/JoltDynamicActorComponent.h>
#include <JoltPlugin/Actors/JoltStaticActorComponent.h>
#include <JoltPlugin/Shapes/JoltShapeBoxComponent.h>
#include <JoltPlugin/System/JoltCore.h>
#include <JoltPlugin/System/JoltWorldModule.h>
#include <TestFramework/Framework/TestFramework.h>
#include <source_location>

/// \file
/// Shared pieces of the headless Jolt tests: check helpers, a world fixture and scene builders.

namespace JoltTest
{
  /// Sets up the layers used by these tests: 1 and 2 collide with the default layer and with themselves, but not with each other.
  class LayerConfig
  {
  public:
    LayerConfig()
      : m_pConfig(const_cast<plCollisionFilterConfig*>(&plJoltCore::GetCollisionFilterConfig()))
    {
      for (plUInt32 uiGroup = 0; uiGroup < 3; ++uiGroup)
        m_Saved[uiGroup] = m_pConfig->GetFilterMask(uiGroup);

      m_pConfig->EnableCollision(0, 0, true);
      m_pConfig->EnableCollision(0, 1, true);
      m_pConfig->EnableCollision(0, 2, true);
      m_pConfig->EnableCollision(1, 1, true);
      m_pConfig->EnableCollision(2, 2, true);
      m_pConfig->EnableCollision(1, 2, false);
    }

    /// rief Changes whether two layers collide. Restored when the object goes out of scope.
    void Allow(plUInt32 uiLayerA, plUInt32 uiLayerB, bool bCollide) { m_pConfig->EnableCollision(uiLayerA, uiLayerB, bCollide); }

    ~LayerConfig()
    {
      for (plUInt32 uiGroup = 0; uiGroup < 3; ++uiGroup)
      {
        for (plUInt32 uiOther = 0; uiOther < 3; ++uiOther)
          m_pConfig->EnableCollision(uiGroup, uiOther, (m_Saved[uiGroup] & PL_BIT(uiOther)) != 0);
      }
    }

  private:
    plCollisionFilterConfig* m_pConfig = nullptr;
    plUInt32 m_Saved[3] = {};
  };

  /// \brief Records a check with the engine test framework. A failure is reported at the caller's file and line.
  void Check(bool bValue, const char* szMessage, std::source_location location = std::source_location::current());

  /// \brief Records a check that |fActual - fExpected| <= fTolerance. The values are printed on failure.
  void CheckNear(float fActual, float fExpected, float fTolerance, const char* szMessage, std::source_location location = std::source_location::current());

  /// \brief A check that describes the behaviour a roadmap bug (for example "PH-B14") is supposed to deliver.
  ///
  /// While the bug is open a failure is logged as "OPEN PH-B14: ..." and does not fail the run. A pass is logged as a prompt to turn
  /// the check into a regular one. With -joltStrictDefects a failure fails the run.
  void OpenDefect(const char* szBugId, bool bValue, const char* szMessage, std::source_location location = std::source_location::current());

  /// \brief OpenDefect() for |fActual - fExpected| <= fTolerance.
  void OpenDefectNear(const char* szBugId, float fActual, float fExpected, float fTolerance, const char* szMessage, std::source_location location = std::source_location::current());

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

  /// \brief Counts the different actor objects among the results of a ray cast.
  plUInt32 CountDistinctActors(const plPhysicsCastResultArray& results);
} // namespace JoltTest
