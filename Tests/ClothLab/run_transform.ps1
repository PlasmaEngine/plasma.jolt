# Transforms assets of a project with the worktree's EditorProcessor on DX12, without assert dialogs.
# Only the process tree started here is ended.
param(
  [Parameter(Mandatory = $true)] [string] $Binaries,
  [Parameter(Mandatory = $true)] [string] $Project,
  [Parameter(Mandatory = $true)] [string] $Log,
  [string] $Assets = '',
  [int] $TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

$freeGb = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
if ($freeGb -lt 8) { throw ("Only {0:N1} GB of memory is free; not starting a transform." -f $freeGb) }

$env:PL_SILENT_ASSERTS = '1'

# -renderer reaches the engine process; the editor process draws its own interface with -editorUiRenderer.
$arguments = @('-project', "`"$Project`"", '-transform', 'Default', '-renderer', 'DX12', '-editorUiRenderer', 'DX12', '-priority', 'BelowNormal')
if ($Assets -ne '') { $arguments += @('-assets', "`"$Assets`"") }

$started = Get-Date
$process = Start-Process -FilePath (Join-Path $Binaries 'EditorProcessor.exe') -ArgumentList $arguments -WorkingDirectory $Binaries -RedirectStandardOutput $Log -RedirectStandardError "$Log.err" -PassThru -WindowStyle Hidden

try
{
  if (-not $process.WaitForExit($TimeoutSeconds * 1000))
  {
    Write-Output "timed out after $TimeoutSeconds seconds"
  }
  else
  {
    Write-Output "exit code $($process.ExitCode)"
  }
}
finally
{
  if (-not $process.HasExited)
  {
    & taskkill /T /F /PID $process.Id | Out-Null
  }

  # An engine process this run started can outlive a crashed parent. It is found by its parent id and start time.
  $orphans = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($process.Id)" | Where-Object { $_.CreationDate -ge $started }

  foreach ($orphan in $orphans)
  {
    Write-Output "ending leftover child $($orphan.Name) ($($orphan.ProcessId))"
    & taskkill /T /F /PID $orphan.ProcessId | Out-Null
  }
}
