#!/bin/bash
# Transforms assets of a project with the worktree's EditorProcessor on Metal, without assert dialogs.
#
# run_transform.sh <binaries> <project> <log> [assets]
#
#   assets   paths or GUIDs separated by ';'. Without it every asset of the project and of the base data is transformed.
#   TIMEOUT  seconds after which the run is ended (default 1500)
#
# In a fresh engine checkout transform the Data/Base/RenderPipelines assets first.

set -u

binaries=$1; project=$2; log=$3; assets=${4:-}
timeout=${TIMEOUT:-1500}

arguments=(-project "$project" -transform Default -renderer Metal -editorUiRenderer Metal)
[ -n "$assets" ] && arguments+=(-assets "$assets")

PL_SILENT_ASSERTS=1 perl -e 'alarm shift; exec @ARGV' "$timeout" "$binaries/EditorProcessor" "${arguments[@]}" > "$log" 2>&1
echo "exit code $?"

grep -i "error" "$log" | cut -c1-300 | head -20
