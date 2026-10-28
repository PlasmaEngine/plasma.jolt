<#
.SYNOPSIS
  Builds and runs the headless plasma.jolt tests. This is the entry point for package CI.

.DESCRIPTION
  Needs an engine SDK that has been built for the chosen configuration. Pass it with -SdkRoot or set PLASMA_SDK_ROOT.
  The test executable starts no editor and no GPU device and does not touch any package store.
  Exit code 0 means every regression check passed. -Strict also fails on open R1 items.
  -StressSeconds runs the soak scene instead of the check groups (600 for the R0 gate, use the Debug configuration).

.EXAMPLE
  ./Tests/Run-JoltTests.ps1 -SdkRoot E:\PlasmaTech\PlasmaEngine2
  ./Tests/Run-JoltTests.ps1 -Configuration Debug -StressSeconds 600
#>
[CmdletBinding()]
param(
  [string]$SdkRoot = $env:PLASMA_SDK_ROOT,
  [ValidateSet('Debug', 'Development', 'Release')]
  [string]$Configuration = 'Development',
  [string]$Group = '',
  [double]$StressSeconds = 0,
  [switch]$Strict,
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

if (-not $SdkRoot -or -not (Test-Path -LiteralPath (Join-Path $SdkRoot 'Code\Engine'))) {
  throw "Set PLASMA_SDK_ROOT or pass -SdkRoot to an engine checkout that has been built for $Configuration."
}

$packageRoot = Split-Path -Parent $PSScriptRoot
$env:PLASMA_SDK_ROOT = (Resolve-Path -LiteralPath $SdkRoot).Path

Push-Location $packageRoot
try {
  if (-not $SkipBuild) {
    plasmabuild build JoltTests --configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw "Building JoltTests failed with exit code $LASTEXITCODE." }
  }

  $engineBinaries = Join-Path $env:PLASMA_SDK_ROOT "Binaries\Windows_$Configuration"
  $env:PATH = "$engineBinaries;$env:PATH"

  $exe = Join-Path $packageRoot "Bin\Windows_x64_$Configuration\JoltTests.exe"
  if (-not (Test-Path -LiteralPath $exe)) { throw "JoltTests.exe was not built: $exe" }

  $arguments = @()
  if ($Group) { $arguments += @('--group', $Group) }
  if ($Strict) { $arguments += '--strict' }
  if ($StressSeconds -gt 0) { $arguments += @('--stress', $StressSeconds) }

  & $exe @arguments
  exit $LASTEXITCODE
}
finally {
  Pop-Location
}
