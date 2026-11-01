<#
.SYNOPSIS
  Builds and runs the headless plasma.jolt tests (JoltTest) on the engine test framework.

.DESCRIPTION
  Needs an engine SDK that has been built for the chosen configuration, including TestFramework. Pass it with -SdkRoot or set
  PLASMA_SDK_ROOT. The test executable starts no editor and no GPU device and does not touch any package store.
  Exit code 0 means every regression check passed. Checks for roadmap bugs that are still open are reported as warnings and listed
  at the end of the run; -StrictDefects fails the run on them.
  -SoakSeconds runs the soak scene as well (600 is the R0 gate, use the Debug configuration).

.EXAMPLE
  ./Tests/Run-JoltTests.ps1 -SdkRoot E:\PlasmaTech\PlasmaEngine2
  ./Tests/Run-JoltTests.ps1 -Filter JoltQueries
  ./Tests/Run-JoltTests.ps1 -Configuration Debug -SoakSeconds 600 -Filter JoltSoak
#>
[CmdletBinding()]
param(
  [string]$SdkRoot = $env:PLASMA_SDK_ROOT,
  [ValidateSet('Debug', 'Development', 'Release')]
  [string]$Configuration = 'Development',
  [string]$Filter = '',
  [int]$SoakSeconds = 0,
  [switch]$StrictDefects,
  [switch]$SkipBuild,
  [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if (-not $SdkRoot -or -not (Test-Path -LiteralPath (Join-Path $SdkRoot 'Code/Engine'))) {
  throw "Set PLASMA_SDK_ROOT or pass -SdkRoot to an engine checkout that has been built for $Configuration."
}

$onLinux = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)
$platform = if ($onLinux) { 'Linux' } else { 'Windows' }
$packageRoot = Split-Path -Parent $PSScriptRoot
$env:PLASMA_SDK_ROOT = (Resolve-Path -LiteralPath $SdkRoot).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $packageRoot 'Output/JoltTest' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

Push-Location $packageRoot
try {
  if (-not $SkipBuild) {
    plasmabuild build JoltTest -p $platform -a x64 -c $Configuration --no-color
    if ($LASTEXITCODE -ne 0) { throw "Building JoltTest failed with exit code $LASTEXITCODE." }
  }

  $engineBinaries = Join-Path $env:PLASMA_SDK_ROOT "Binaries/${platform}_$Configuration"
  if ($onLinux) { $env:LD_LIBRARY_PATH = "${engineBinaries}:$($env:LD_LIBRARY_PATH)" }
  else { $env:PATH = "$engineBinaries;$env:PATH" }

  $exe = Join-Path $packageRoot ("Bin/${platform}_x64_$Configuration/JoltTest" + $(if ($onLinux) { '' } else { '.exe' }))
  if (-not (Test-Path -LiteralPath $exe)) { throw "JoltTest was not built: $exe" }

  $json = Join-Path $OutputDirectory 'results.json'
  if (Test-Path -LiteralPath $json) { Remove-Item -LiteralPath $json }

  $arguments = @('-run', '-close', '-noGui', '-noSave', '-all', '-assert', '0', '-outputDir', $OutputDirectory, '-json', $json)
  if ($Filter) { $arguments += @('-filter', $Filter) }
  if ($StrictDefects) { $arguments += '-joltStrictDefects' }
  if ($SoakSeconds -gt 0) { $arguments += @('-joltSoakSeconds', $SoakSeconds) }

  & $exe @arguments
  $code = $LASTEXITCODE
  if ($code -eq 0 -and -not (Test-Path -LiteralPath $json)) {
    Write-Host "JoltTest exited with 0 but wrote no report: $json"
    $code = 1
  }
  Write-Host "Report: $json"
  exit $code
}
finally {
  Pop-Location
}
