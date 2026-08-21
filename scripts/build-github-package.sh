#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
engine_root='C:/Program Files/Epic Games/UE_5.7'
output_file=''
keep_work=false

usage() {
  printf '%s\n' \
    'Build the installable GitHub ZIP from a fresh UHT/UBT BuildPlugin output.' \
    '' \
    'Usage:' \
    '  scripts/build-github-package.sh [--engine-root PATH] [--output PATH] [--keep-work]'
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
      output_file=$2
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

engine_root=$(normalize_path "$engine_root")
descriptor="$repository_root/ModelContextProtocol/ModelContextProtocol.uplugin"
version=$(sed -n 's/^[[:space:]]*"VersionName":[[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$descriptor")
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf 'Invalid plugin VersionName in %s: %s\n' "$descriptor" "$version" >&2
  exit 1
fi

if [[ -z "$output_file" ]]; then
  output_file="$repository_root/artifacts/UnrealMCP-$version-UE5.7-Win64-GitHub.zip"
fi
output_file=$(normalize_path "$output_file")
output_parent=$(dirname "$output_file")
mkdir -p "$output_parent"
output_parent=$(cd "$output_parent" && pwd -P)
output_file="$output_parent/$(basename "$output_file")"
if [[ -e "$output_file" ]]; then
  printf 'Output already exists; choose a fresh path: %s\n' "$output_file" >&2
  exit 1
fi

work_root=$(mktemp -d "${TMPDIR:-/tmp}/unrealmcp-github-package.XXXXXX")
build_output="$work_root/BuildPlugin"
staging_root="$work_root/Staging"
plugin_package="$staging_root/Plugins/ModelContextProtocol"

cleanup_work() {
  local temp_root=${TMPDIR:-/tmp}
  if [[ "$keep_work" == false && -d "$work_root" && "$work_root" == "$temp_root"/unrealmcp-github-package.* ]]; then
    rm -rf -- "$work_root"
  else
    printf 'Retained release work directory: %s\n' "$work_root"
  fi
}
trap cleanup_work EXIT

"$repository_root/scripts/build-plugin.sh" \
  --engine-root "$engine_root" \
  --output "$build_output"

mkdir -p "$plugin_package"
tar \
  --exclude='./Intermediate' \
  --exclude='./Binaries/Win64/*.pdb' \
  -C "$build_output" \
  -cf - . | tar -C "$plugin_package" -xf -

required_paths=(
  "$plugin_package/ModelContextProtocol.uplugin"
  "$plugin_package/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll"
  "$plugin_package/Binaries/Win64/UnrealEditor.modules"
  "$plugin_package/Source/ModelContextProtocol/Public/ModelContextProtocolSettings.h"
  "$plugin_package/Config/DefaultModelContextProtocol.ini"
  "$plugin_package/Resources/ModelContextProtocol/metadata.json"
  "$plugin_package/README.md"
  "$plugin_package/README.zh-CN.md"
  "$plugin_package/LICENSE"
  "$plugin_package/THIRD_PARTY_NOTICES.md"
)
for required_path in "${required_paths[@]}"; do
  if [[ ! -e "$required_path" ]]; then
    printf 'GitHub package is missing required path: %s\n' "$required_path" >&2
    exit 1
  fi
done

if /usr/bin/find "$plugin_package" -type d \( -name Intermediate -o -name Saved -o -name DerivedDataCache \) -print -quit | /usr/bin/grep -q .; then
  printf '%s\n' 'GitHub package unexpectedly contains a generated directory.' >&2
  exit 1
fi
if /usr/bin/find "$plugin_package" -type f \( -name '*.exe' -o -name '*.pdb' -o -name '*.obj' -o -name '*.lib' -o -name '*.exp' \) -print -quit | /usr/bin/grep -q .; then
  printf '%s\n' 'GitHub package unexpectedly contains a stripped build by-product.' >&2
  exit 1
fi

tar --format zip -cf "$output_file" -C "$staging_root" Plugins

printf 'Built GitHub release asset: %s\n' "$output_file"
printf 'SHA-256: '
sha256sum "$output_file" | awk '{print $1}'
