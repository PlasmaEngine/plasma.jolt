#include <JoltPlugin/JoltPluginPCH.h>

#include <Foundation/Types/SharedPtr.h>
#include <JoltPlugin/System/JoltJobSystem.h>

plJoltJobSystem::plJoltJobSystem(plUInt32 uiMaxJobs, plUInt32 uiMaxBarriers)
{
  JobSystemWithBarrier::Init(uiMaxBarriers);

  m_Jobs.Init(uiMaxJobs, uiMaxJobs);

  m_Tasks.SetCount(uiMaxJobs);
  for (plUInt32 i = 0; i < m_Tasks.GetCount(); ++i)
  {
    m_Tasks[i] = PL_DEFAULT_NEW(plJoltTask);
    m_Tasks[i]->ConfigureTask("Jolt", plTaskNesting::Never, &plJoltJobSystem::OnTaskFinished);
  }
}

int plJoltJobSystem::GetMaxConcurrency() const
{
  return plTaskSystem::GetWorkerThreadCount(plWorkerThreadType::ShortTasks);
}

JPH::JobHandle plJoltJobSystem::CreateJob(const char* szName, JPH::ColorArg color, const JobFunction& jobFunction, plUInt32 uiNumDependencies)
{
  // Loop until we can get a job from the free list
  plUInt32 index;
  for (;;)
  {
    index = m_Jobs.ConstructObject(szName, color, this, jobFunction, uiNumDependencies);
    if (index != AvailableJobs::cInvalidObjectIndex)
      break;

    PL_ASSERT_DEBUG(false, "No Jolt jobs available!");
    plThreadUtils::YieldTimeSlice();
  }

  CustomJob* job = &m_Jobs.Get(index);
  job->m_uiJobIndex = index;

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
  {
    plStringBuilder name("Jolt-", szName);
    m_Tasks[index]->ConfigureTask(name, plTaskNesting::Never, &plJoltJobSystem::OnTaskFinished);
  }
#endif

  // Construct handle to keep a reference, the job is queued below and may immediately complete
  JobHandle handle(job);

  // If there are no dependencies, queue the job now
  if (uiNumDependencies == 0)
    QueueJob(job);

  // Return the handle
  return handle;
}

void plJoltJobSystem::FreeJob(Job* pJob)
{
  m_Jobs.DestructObject(static_cast<CustomJob*>(pJob));
}

void plJoltJobSystem::OnTaskFinished(const plSharedPtr<plTask>& task)
{
  plJoltTask* pTask = static_cast<plJoltTask*>(task.Borrow());

  auto* pJob = static_cast<JPH::JobSystem::Job*>(pTask->m_pJob);
  pTask->m_pJob = nullptr;

  // doing this here prevens a race condition in reusing tasks
  pJob->Release();
}

void plJoltJobSystem::QueueJob(Job* pJob)
{
  auto* pMyJob = static_cast<CustomJob*>(pJob);
  pMyJob->AddRef();

  const auto& pTask = m_Tasks[pMyJob->m_uiJobIndex];

  pTask->m_pJob = pMyJob;

  plTaskSystem::StartSingleTask(pTask, plTaskPriority::EarlyThisFrame);
}

void plJoltJobSystem::QueueJobs(Job** pJob, plUInt32 uiNum_Jobs)
{
  for (plUInt32 i = 0; i < uiNum_Jobs; ++i)
  {
    QueueJob(pJob[i]);
  }
}

void plJoltJobSystem::plJoltTask::Execute()
{
  m_pJob->Execute();
}
