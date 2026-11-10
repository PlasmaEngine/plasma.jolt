# Runs the worktree's Player on a scene without taking focus, captures its own window, then ends it.
#
# Only the process started here is touched: the window is found through its process id, captured with PrintWindow
# (never from the screen), and the process tree is ended by pid in the finally block.

param(
  [Parameter(Mandatory = $true)] [string] $Binaries,
  [Parameter(Mandatory = $true)] [string] $Project,
  [Parameter(Mandatory = $true)] [string] $Scene,
  [Parameter(Mandatory = $true)] [string] $OutDir,
  [string] $Name = 'shot',
  [int] $WaitMs = 20000,
  [int] $Shots = 1,
  [int] $ShotIntervalMs = 500,
  [int] $Width = 1280,
  [int] $Height = 720,
  [string[]] $PlayerArgs = @(),
  [string] $Exec = '',
  [int] $ExitWaitMs = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class ClothLabWin32
{
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }

  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int x, int y, int cx, int cy, uint flags);
}
'@

function Save-Window([IntPtr] $hwnd, [string] $path)
{
  $rect = New-Object ClothLabWin32+RECT
  [void][ClothLabWin32]::GetClientRect($hwnd, [ref] $rect)

  $w = $rect.Right - $rect.Left
  $h = $rect.Bottom - $rect.Top

  if ($w -le 0 -or $h -le 0) { throw "The Player window has no client area." }

  $bitmap = New-Object System.Drawing.Bitmap $w, $h
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $hdc = $graphics.GetHdc()

  try
  {
    # 1 = client area only, 2 = render the full content (works for flip-model swap chains and occluded windows).
    [void][ClothLabWin32]::PrintWindow($hwnd, $hdc, 3)
  }
  finally
  {
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
  }

  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bitmap.Dispose()
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

$freeGb = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
if ($freeGb -lt 8) { throw ("Only {0:N1} GB of memory is free; not starting a Player." -f $freeGb) }

$wnd = Join-Path $OutDir "$Name.window.ddl"
@"
WindowDesc
{
	string %Title{"ClothLab - $Name"}
	string %Mode{"Window"}
	Vec2u %Resolution{uint32{$Width,$Height}}
	bool %ClipMouseCursor{false}
	bool %ShowMouseCursor{true}
	bool %SetForegroundOnInit{false}
	bool %CenterWindowOnDisplay{false}
}
"@ | Out-File -FilePath $wnd -Encoding ascii

$log = Join-Path $OutDir "$Name.log"
$err = Join-Path $OutDir "$Name.err.log"

$arguments = @('-project', "`"$Project`"", '-scene', $Scene, '-wnd', "`"$wnd`"", '-renderer', 'DX12', '-no-file-cvars') + $PlayerArgs
if ($Exec -ne '') { $arguments += @('-exec', "`"$Exec`"") }

# A failed assert writes its text and a dump instead of opening a dialog on the desktop.
$env:PL_SILENT_ASSERTS = '1'

$player = Start-Process -FilePath (Join-Path $Binaries 'Player.exe') -ArgumentList $arguments -WorkingDirectory $Binaries -RedirectStandardOutput $log -RedirectStandardError $err -PassThru -WindowStyle Hidden

try
{
  # Wait for the window of this process.
  $hwnd = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds(90)

  while ($hwnd -eq [IntPtr]::Zero)
  {
    if ($player.HasExited) { throw "The Player exited with code $($player.ExitCode) before it opened a window. See $log" }
    if ((Get-Date) -gt $deadline) { throw "The Player opened no window within 90 seconds." }

    Start-Sleep -Milliseconds 250
    $player.Refresh()
    $hwnd = $player.MainWindowHandle
  }

  # Behind every other window, without activating it.
  [void][ClothLabWin32]::SetWindowPos($hwnd, [IntPtr] 1, 0, 0, 0, 0, 0x0013)

  Start-Sleep -Milliseconds $WaitMs

  for ($i = 0; $i -lt $Shots; ++$i)
  {
    if ($player.HasExited) { throw "The Player exited with code $($player.ExitCode) before capture $i. See $log" }

    $path = Join-Path $OutDir ("{0}_{1}.png" -f $Name, $i)
    Save-Window $hwnd $path
    Write-Output "captured $path"

    if ($i + 1 -lt $Shots) { Start-Sleep -Milliseconds $ShotIntervalMs }
  }

  # A run whose -exec ends in 'quit' leaves by itself, which also writes the whole log.
  if ($ExitWaitMs -gt 0 -and -not $player.WaitForExit($ExitWaitMs))
  {
    Write-Output "still running after $ExitWaitMs ms"
  }
}
finally
{
  if (-not $player.HasExited)
  {
    & taskkill /T /F /PID $player.Id | Out-Null
  }

  Write-Output "log $log"
}
