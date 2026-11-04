#include "JoltTestSupport.h"
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <cstdlib>
#include <cstring>

/// \brief Command line:
///   JoltTests.exe [--group <name>] [--strict] [--stress <seconds>] [--record-ragdolls <folder> [--record-filter <prefix>]]
///
/// Without arguments all groups run. Failed checks make the exit code 1. Checks that describe R1 fixes are listed as open
/// items and only fail the run with --strict. --stress runs the soak scene instead of the groups. --record-ragdolls writes
/// the active ragdoll scenarios to the folder instead of running the groups.
struct Options
{
  const char* m_szGroup = nullptr;
  bool m_bStrict = false;
  double m_fStressSeconds = 0.0;
  const char* m_szRecordFolder = nullptr;
  const char* m_szRecordFilter = nullptr;
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
    else if (std::strcmp(argv[i], "--record-ragdolls") == 0 && i + 1 < argc)
      options.m_szRecordFolder = argv[++i];
    else if (std::strcmp(argv[i], "--record-filter") == 0 && i + 1 < argc)
      options.m_szRecordFilter = argv[++i];
  }

  return options;
}

/// Prints the assertion and ends the run with exit code 2. The tests run headless, so the default handler's modal dialog
/// would block the run until someone clicks it away.
static bool PrintAssertAndExit(const char* szSourceFile, plUInt32 uiLine, const char* szFunction, const char* szExpression, const char* szAssertMsg)
{
  std::printf("\n*** Assertion: %s\n    %s(%u) in %s\n    %s\n", szExpression, szSourceFile, uiLine, szFunction, szAssertMsg ? szAssertMsg : "");
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 2);
  return false;
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
    {"ragdoll", RunRagdollTests},
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

  plSetAssertHandler(PrintAssertAndExit);

  std::printf("starting engine systems\n");
  JoltTest::Startup();
  std::printf("engine systems started\n");

  if (options.m_szRecordFolder != nullptr)
  {
    RunRagdollRecordings(options.m_szRecordFolder, options.m_szRecordFilter);
  }
  else if (options.m_fStressSeconds > 0.0)
  {
    const plUInt64 uiFrames = RunStressScene(options.m_fStressSeconds);
    std::printf("Soak scene: %llu frames\n", static_cast<unsigned long long>(uiFrames));
  }
  else
  {
    RunGroups(options);
  }

  const JoltTest::Totals& totals = JoltTest::GetTotals();
  std::printf("Jolt tests: %u checks, %u failure(s), %u open R1 item(s), %u open plan gate(s)\n", totals.m_uiChecks, totals.m_uiFailures, totals.m_uiOpenR1Items, totals.m_uiOpenPlanGates);

  const int iExitCode = (totals.m_uiFailures > 0 || (options.m_bStrict && (totals.m_uiOpenR1Items > 0 || totals.m_uiOpenPlanGates > 0))) ? 1 : 0;

  // Shutting down the core systems unloads resources through the renderer, which asserts in Debug without a GPU device,
  // and skipping it makes the leak tracker assert instead, also from the plugin's DLL detach. The tests are done, so end
  // the process without running either.
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(iExitCode));
  return iExitCode;
}
