#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
engine_root='C:/Program Files/Epic Games/UE_5.7'
output_directory=''

usage() {
  printf '%s\n' \
    'Build the Win64 UnrealMCP plugin from Git Bash.' \
    '' \
    'Usage:' \
    '  scripts/build-plugin.sh [--engine-root PATH] [--output PATH]' \
    '' \
    'Defaults:' \
    '  --engine-root  C:/Program Files/Epic Games/UE_5.7' \
    '  --output       artifacts/ModelContextProtocol-build-<timestamp>-<pid>' \
    '' \
    'UE_DOTNET_ROOT may override bundled .NET discovery for diagnostics.'
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
    --engine-root)
      [[ $# -ge 2 ]] || { printf '%s\n' 'Missing value for --engine-root.' >&2; exit 2; }
      engine_root=$2
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { printf '%s\n' 'Missing value for --output.' >&2; exit 2; }
      output_directory=$2
      shift 2
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

engine_root=$(normalize_path "$engine_root")
if [[ ! -d "$engine_root" ]]; then
  printf 'Unreal Engine root not found: %s\n' "$engine_root" >&2
  exit 1
fi
engine_root=$(cd "$engine_root" && pwd -P)

plugin="$repository_root/ModelContextProtocol/ModelContextProtocol.uplugin"
uat="$engine_root/Engine/Build/BatchFiles/RunUAT.sh"
if [[ ! -f "$plugin" ]]; then
  printf 'Plugin descriptor not found: %s\n' "$plugin" >&2
  exit 1
fi
if [[ ! -f "$uat" ]]; then
  printf 'RunUAT.sh not found below engine root: %s\n' "$uat" >&2
  exit 1
fi

if [[ -z "$output_directory" ]]; then
  output_directory="$repository_root/artifacts/ModelContextProtocol-build-$(date +%Y%m%d-%H%M%S)-$$"
fi
output_directory=$(normalize_path "$output_directory")
output_parent=$(dirname "$output_directory")
mkdir -p "$output_parent"
output_parent=$(cd "$output_parent" && pwd -P)
output_directory="$output_parent/$(basename "$output_directory")"
if [[ -e "$output_directory" ]]; then
  printf 'Output path already exists; choose a fresh path: %s\n' "$output_directory" >&2
  exit 1
fi

if [[ -n "${UE_DOTNET_ROOT:-}" ]]; then
  dotnet_root=$(normalize_path "$UE_DOTNET_ROOT")
  [[ -d "$dotnet_root" ]] || { printf 'UE_DOTNET_ROOT not found: %s\n' "$dotnet_root" >&2; exit 1; }
  dotnet_root=$(cd "$dotnet_root" && pwd -P)
  dotnet_exe="$dotnet_root/dotnet.exe"
else
  bundled_dotnet="$engine_root/Engine/Binaries/ThirdParty/DotNet"
  if [[ ! -d "$bundled_dotnet" ]]; then
    printf 'UE-bundled .NET directory was not found: %s\n' "$bundled_dotnet" >&2
    exit 1
  fi
  dotnet_exe=$(find "$bundled_dotnet" -mindepth 2 -maxdepth 3 -type f -path '*/win-x64/dotnet.exe' -print 2>/dev/null | sort -V | tail -n 1)
  if [[ -z "$dotnet_exe" ]]; then
    printf 'UE-bundled Win64 dotnet.exe was not found below: %s\n' "$bundled_dotnet" >&2
    exit 1
  fi
  dotnet_root=$(dirname "$dotnet_exe")
fi
if [[ ! -x "$dotnet_exe" ]]; then
  printf 'UE-bundled dotnet.exe is not executable: %s\n' "$dotnet_exe" >&2
  exit 1
fi

# RunUAT.sh does not set up .NET on Windows/MSYS. Put UE's own runtime first so
# an unrelated system SDK cannot win resolution and miss WindowsDesktop 8.x.
export DOTNET_ROOT
if command -v cygpath >/dev/null 2>&1; then
  DOTNET_ROOT=$(cygpath -m "$dotnet_root")
else
  DOTNET_ROOT=$dotnet_root
fi
export DOTNET_MULTILEVEL_LOOKUP=0
export PATH="$dotnet_root:$PATH"
hash -r

resolved_dotnet=$(command -v dotnet || true)
if [[ -z "$resolved_dotnet" ]]; then
  printf '%s\n' 'dotnet was not found after configuring UE_DOTNET_ROOT.' >&2
  exit 1
fi
if ! dotnet --list-runtimes | grep -q '^Microsoft.WindowsDesktop.App 8\.'; then
  printf 'UE-bundled .NET lacks Microsoft.WindowsDesktop.App 8.x: %s\n' "$dotnet_root" >&2
  exit 1
fi

if command -v cygpath >/dev/null 2>&1; then
  plugin_argument=$(cygpath -m "$plugin")
  package_argument=$(cygpath -m "$output_directory")
else
  plugin_argument=$plugin
  package_argument=$output_directory
fi

printf 'Using UE .NET: %s\n' "$resolved_dotnet"
printf 'Building plugin package: %s\n' "$output_directory"
"$uat" BuildPlugin \
  "-Plugin=$plugin_argument" \
  "-Package=$package_argument" \
  -TargetPlatforms=Win64 \
  -Rocket

required_outputs=(
  "$output_directory/ModelContextProtocol.uplugin"
  "$output_directory/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll"
  "$output_directory/Binaries/Win64/UnrealEditor.modules"
)
for required_output in "${required_outputs[@]}"; do
  if [[ ! -f "$required_output" ]]; then
    printf 'BuildPlugin succeeded but required output is missing: %s\n' "$required_output" >&2
    exit 1
  fi
done

cp -f "$repository_root/README.md" "$output_directory/README.md"
cp -f "$repository_root/README.zh-CN.md" "$output_directory/README.zh-CN.md"
cp -R "$repository_root/docs" "$output_directory/docs"

printf 'Built plugin package: %s\n' "$output_directory"
