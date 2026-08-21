#!/usr/bin/env bash
set -euo pipefail

engine_root='C:/Program Files/Epic Games/UE_5.7'
asset=''
keep_work=false

usage() {
  printf '%s\n' \
    'Launch Unreal Engine against the plugin extracted from a final release ZIP.' \
    '' \
    'Usage:' \
    '  scripts/test-release-asset.sh --asset ZIP [--engine-root PATH] [--keep-work]'
}

normalize_path() {
  local candidate=$1
  if command -v cygpath >/dev/null 2>&1 && [[ "$candidate" =~ ^[A-Za-z]:[\\/].* ]]; then
    cygpath -u "$candidate"
  else
    printf '%s\n' "$candidate"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --asset)
      [[ $# -ge 2 ]] || { printf '%s\n' 'Missing value for --asset.' >&2; exit 2; }
      asset=$2
      shift 2
      ;;
    --engine-root)
      [[ $# -ge 2 ]] || { printf '%s\n' 'Missing value for --engine-root.' >&2; exit 2; }
      engine_root=$2
      shift 2
      ;;
    --keep-work)
      keep_work=true
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown argument: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$asset" ]]; then
  printf '%s\n' '--asset is required.' >&2
  usage >&2
  exit 2
fi

asset=$(normalize_path "$asset")
engine_root=$(normalize_path "$engine_root")
editor="$engine_root/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
if [[ ! -f "$asset" ]]; then
  printf 'Release asset not found: %s\n' "$asset" >&2
  exit 1
fi
if [[ ! -x "$editor" ]]; then
  printf 'UnrealEditor-Cmd.exe not found: %s\n' "$editor" >&2
  exit 1
fi

work_root=$(mktemp -d "${TMPDIR:-/tmp}/unrealmcp-release-smoke.XXXXXX")
extract_root="$work_root/Extracted"
project_root="$work_root/HostProject"
project="$project_root/ReleaseSmoke.uproject"
probe="$work_root/settings_probe.py"
result="$work_root/settings_probe_result.json"
log_file="$work_root/UnrealEditor.log"

cleanup_work() {
  local temp_root=${TMPDIR:-/tmp}
  if [[ "$keep_work" == false && -d "$work_root" && "$work_root" == "$temp_root"/unrealmcp-release-smoke.* ]]; then
    rm -rf -- "$work_root"
  else
    printf 'Retained smoke-test work directory: %s\n' "$work_root"
  fi
}
trap cleanup_work EXIT

mkdir -p "$extract_root" "$project_root"
unzip -q "$asset" -d "$extract_root"
plugin="$extract_root/Plugins/ModelContextProtocol"
if [[ ! -f "$plugin/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll" ]]; then
  printf '%s\n' 'Final ZIP does not contain the packaged plugin DLL.' >&2
  exit 1
fi

plugin_parent_windows=$(cygpath -m "$extract_root/Plugins")
printf '%s\n' \
  '{' \
  '  "FileVersion": 3,' \
  '  "EngineAssociation": "5.7",' \
  '  "Category": "Tests",' \
  '  "Description": "Runtime smoke host for the final UnrealMCP release ZIP.",' \
  "  \"AdditionalPluginDirectories\": [\"$plugin_parent_windows\"]," \
  '  "Plugins": [' \
  '    { "Name": "ModelContextProtocol", "Enabled": true, "SupportedTargetPlatforms": ["Win64"] },' \
  '    { "Name": "PythonScriptPlugin", "Enabled": true }' \
  '  ]' \
  '}' > "$project"

printf '%s\n' \
  'import json' \
  'import os' \
  'import unreal' \
  '' \
  'settings_path = "/Script/ModelContextProtocol.ModelContextProtocolSettings"' \
  'developer_settings_path = "/Script/DeveloperSettings.DeveloperSettings"' \
  'settings_class = unreal.load_class(None, settings_path)' \
  'developer_settings_class = unreal.load_class(None, developer_settings_path)' \
  'if settings_class is None:' \
  '    raise RuntimeError(f"unreal.load_class returned None for {settings_path}")' \
  'if developer_settings_class is None:' \
  '    raise RuntimeError(f"unreal.load_class returned None for {developer_settings_path}")' \
  'settings = unreal.get_default_object(settings_class)' \
  'default_port = settings.get_editor_property("port")' \
  'enumerated_classes = [candidate for candidate in unreal.ObjectIterator(unreal.Class) if candidate == settings_class]' \
  'is_developer_settings = isinstance(settings, unreal.DeveloperSettings)' \
  'result = {' \
  '    "class_path": settings_class.get_path_name(),' \
  '    "default_port": default_port,' \
  '    "is_developer_settings": is_developer_settings,' \
  '    "enumerated": len(enumerated_classes) == 1,' \
  '    "enumerated_count": len(enumerated_classes),' \
  '}' \
  'with open(os.environ["UE_MCP_SETTINGS_SMOKE_RESULT"], "w", encoding="utf-8") as output:' \
  '    json.dump(result, output, indent=2, sort_keys=True)' \
  'if default_port != 18777 or not is_developer_settings or len(enumerated_classes) != 1:' \
  '    raise RuntimeError(f"Unexpected settings reflection result: {result}")' > "$probe"

project_windows=$(cygpath -m "$project")
probe_windows=$(cygpath -m "$probe")
result_windows=$(cygpath -m "$result")
log_windows=$(cygpath -m "$log_file")

if ! UE_MCP_PORT=18789 UE_MCP_SETTINGS_SMOKE_RESULT="$result_windows" \
  "$editor" "$project_windows" \
  -run=pythonscript \
  "-script=$probe_windows" \
  -unattended -nullrhi -nosplash -nosound -NoAssetRegistryCache \
  -DDC=InstalledNoZenLocalFallback -trace=none \
  "-abslog=$log_windows"; then
  printf '%s\n' 'Unreal runtime smoke test failed. Log tail:' >&2
  tail -120 "$log_file" >&2 || true
  exit 1
fi

if [[ ! -f "$result" ]]; then
  printf '%s\n' 'Unreal exited without producing the settings smoke-test result.' >&2
  tail -120 "$log_file" >&2 || true
  exit 1
fi

grep -Fq '"class_path": "/Script/ModelContextProtocol.ModelContextProtocolSettings"' "$result"
grep -Fq '"default_port": 18777' "$result"
grep -Fq '"is_developer_settings": true' "$result"
grep -Fq '"enumerated": true' "$result"

printf '%s\n' 'Final release asset runtime smoke test passed:'
sed -n '1,80p' "$result"
printf 'Asset SHA-256: '
sha256sum "$asset" | awk '{print $1}'
