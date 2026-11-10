#!/bin/bash
# Runs the worktree's Player on a scene with Metal, saves screenshots of its own frame, then lets it quit.
#
# run_player.sh <binaries> <project> <scene> <out dir> <name> [player args...]
#
#   WAIT      ms before the first screenshot (default 20000)
#   SHOTS     number of screenshots (default 1)
#   INTERVAL  ms between screenshots (default 500)
#   PRE       console commands run before the first screenshot, separated by ';'
#   RECORD    "<frames> <frames per second>": records that many frames after the wait instead of taking screenshots.
#             They land in <out dir>/<name>_frames; make_video.swift next to this script turns them into a video.
#   SCRIPT    console commands run while recording, separated by ';' ('@N' waits N ms of wall time)
#   SIZE      window size, "<width>,<height>" (default 1280,720)
#   TIMEOUT   seconds after which the Player is ended (default 600)
#
# The screenshots are the Player's own (TakeScreenshot), never the screen. Only the process started here is ended.

set -u

binaries=$1; project=$2; scene=$3; out=$4; name=$5; shift 5

wait_ms=${WAIT:-20000}
shots=${SHOTS:-1}
interval=${INTERVAL:-500}
pre=${PRE:+$PRE;}
timeout=${TIMEOUT:-600}

mkdir -p "$out"

wnd="$out/$name.window.ddl"
cat > "$wnd" <<EOF
WindowDesc
{
	string %Title{"ClothLab - $name"}
	string %Mode{"Window"}
	Vec2u %Resolution{uint32{${SIZE:-1280,720}}}
	bool %ClipMouseCursor{false}
	bool %ShowMouseCursor{true}
	bool %SetForegroundOnInit{false}
	bool %CenterWindowOnDisplay{false}
}
EOF

exec_line="@$wait_ms;${pre}TakeScreenshot()"
for ((i = 1; i < shots; ++i)); do exec_line="$exec_line;@$interval;TakeScreenshot()"; done

recording=~/plPlayer/Recording
if [ -n "${RECORD:-}" ]; then
  set -- $RECORD "$@"; frames=$1; fps=$2; shift 2

  # Time stands still while the scene loads, so that a scripted scene starts with the recording. The pause only
  # reaches the worlds that exist when it is asked for, and the scene's world comes up a moment after the first
  # frame: it is asked for a few times. Saving a frame takes longer than showing it, so the wait after the
  # recording is generous; the clock is fixed while it records.
  exec_line="PauseTime(true)"
  for ((i = 0; i < 12; ++i)); do exec_line="$exec_line;@250;PauseTime(true)"; done
  exec_line="$exec_line;@$wait_ms;${pre}PauseTime(false);RecordFrames($frames,$fps);${SCRIPT:+$SCRIPT;}@$((frames * 150 + 4000))"
  rm -rf "$recording"
fi

exec_line="$exec_line;@3000;quit"

screenshots=~/plPlayer/Screenshots
mkdir -p "$screenshots"
marker="$out/$name.marker"
touch "$marker"

# A failed assert writes its text instead of opening a dialog. perl's alarm ends a Player that does not quit by itself.
PL_SILENT_ASSERTS=1 perl -e 'alarm shift; exec @ARGV' "$timeout" "$binaries/Player" -project "$project" -scene "$scene" \
  -wnd "$wnd" -no-file-cvars -renderer Metal "$@" -exec "$exec_line" > "$out/$name.log" 2>&1
echo "exit code $?"

index=0
for shot in $(find "$screenshots" -name "*.png" -newer "$marker" | sort); do
  cp "$shot" "$out/${name}_$index.png"
  echo "captured $out/${name}_$index.png"
  index=$((index + 1))
done

rm -f "$marker"

if [ -n "${RECORD:-}" ]; then
  rm -rf "$out/${name}_frames"
  mv "$recording" "$out/${name}_frames" 2>/dev/null
  index=$(ls "$out/${name}_frames" 2>/dev/null | wc -l | tr -d ' ')
  echo "recorded $index frames in $out/${name}_frames"
fi

[ "$index" -eq 0 ] && echo "nothing was captured, see $out/$name.log"
grep -i "error\|Seriously" "$out/$name.log" | cut -c1-240 | head -8
