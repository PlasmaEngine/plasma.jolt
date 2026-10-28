#include "JoltTestSupport.h"
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <cstdlib>
#include <cstring>

/// \brief Command line:
///   JoltTests.exe [--group <name>] [--strict] [--stress <seconds>]
///
/// Without arguments all groups run. Failed checks make the exit code 1. Checks that describe R1 fixes are listed as open
/// items and only fail the run with --strict. --stress runs the soak scene instead of the groups.
struct Options
{
  const char* m_szGroup = nullptr;
  bool m_bStrict = false;
  double m_fStressSeconds = 0.0;
};

static Options ParseOptions(int argc, char** argv)
{
  Options options;

  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--strict") == 0)
      options.m_bStrict = true;
    else if (std::strcmp(argv[i], "--group") == 0 && i + 1 < argc)
      options.m_szGroup = argv[++i];
    else if (std::strcmp(argv[i], "--stress") == 0 && i + 1 < argc)
      options.m_fStressSeconds = std::atof(argv[++i]);
  }

  return options;
}

static bool Selected(const Options& options, const char* szGroup)
{
  return options.m_szGroup == nullptr || std::strcmp(options.m_szGroup, szGroup) == 0;
}

static void RunGroups(const Options& options)
{
  struct Group
  {
    const char* m_szName;
    void (*m_Run)();
  };

  const Group groups[] = {
    {"p0", RunP0Regressions},
    {"stacking", RunStackingTests},
    {"layers", RunLayerTests},
    {"triggers", RunTriggerTests},
    {"queries", RunQueryTests},
    {"breaks", RunBreakTests},
    {"buoyancy", RunBuoyancyTests},
    {"character", RunCharacterTests},
    {"lifecycle", RunLifecycleTests},
  };

  for (const Group& group : groups)
  {
    if (Selected(options, group.m_szName))
      group.m_Run();
  }
}

int main(int argc, char** argv)
{
  // unbuffered, so the output up to a crash is not lost
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const Options options = ParseOptions(argc, argv);

  std::printf("starting engine systems\n");
  JoltTest::Startup();
  std::printf("engine systems started\n");

  if (options.m_fStressSeconds > 0.0)
  {
    const plUInt64 uiFrames = RunStressScene(options.m_fStressSeconds);
    std::printf("Soak scene: %llu frames\n", static_cast<unsigned long long>(uiFrames));
  }
  else
  {
    RunGroups(options);
  }

  const JoltTest::Totals& totals = JoltTest::GetTotals();
  std::printf("Jolt tests: %u checks, %u failure(s), %u open R1 item(s)\n", totals.m_uiChecks, totals.m_uiFailures, totals.m_uiOpenR1Items);

  const int iExitCode = (totals.m_uiFailures > 0 || (options.m_bStrict && totals.m_uiOpenR1Items > 0)) ? 1 : 0;

  // Shutting down the core systems unloads resources through the renderer, which asserts in Debug without a GPU device,
  // and skipping it makes the leak tracker assert instead, also from the plugin's DLL detach. The tests are done, so end
  // the process without running either.
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(iExitCode));
  return iExitCode;
}
