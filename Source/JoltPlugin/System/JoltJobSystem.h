#pragma once

#include <Foundation/Threading/TaskSystem.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

class plJoltJobSystem final : public JPH::JobSystemWithBarrier
{
public:
  plJoltJobSystem(plUInt32 uiMaxJobs, plUInt32 uiMaxBarriers);

  virtual int GetMaxConcurrency() const override;
  virtual JPH::JobHandle CreateJob(const char* szName, JPH::ColorArg color, const JobFunction& jobFunction, plUInt32 uiNumDependencies = 0) override;

  virtual void QueueJob(Job* pJob) override;
  virtual void QueueJobs(Job** pJobs, plUInt32 uiNumJobs) override;
  virtual void FreeJob(Job* pJob) override;

private:
  static void OnTaskFinished(const plSharedPtr<plTask>& task);

  class CustomJob : public JPH::JobSystem::Job
  {
  public:
    CustomJob(const char* szJobName, JPH::ColorArg color, JPH::JobSystem* pJobSystem, const JobFunction& jobFunction, plUInt32 uiNumDependencies)
      : Job(szJobName, color, pJobSystem, jobFunction, uiNumDependencies)
    {
    }

    plUInt32 m_uiJobIndex = plInvalidIndex;
  };

  class plJoltTask : public plTask
  {
  public:
    CustomJob* m_pJob = nullptr;

    virtual void Execute() override;
  };


  using AvailableJobs = JPH::FixedSizeFreeList<CustomJob>;
  AvailableJobs m_Jobs;

  plDynamicArray<plSharedPtr<plJoltTask>> m_Tasks;
};
