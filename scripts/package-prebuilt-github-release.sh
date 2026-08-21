#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
output_file=''

usage() {
  printf '%s\n' \
    'Package the locally built, committed, and runtime-verified Win64 DLL.' \
    '' \
    'Usage:' \
    '  scripts/package-prebuilt-github-release.sh [--output PATH]'
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
    --output)
      [[ $# -ge 2 ]] || { printf '%s\n' 'Missing value for --output.' >&2; exit 2; }
      output_file=$2
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

plugin_source="$repository_root/ModelContextProtocol"
binaries="$plugin_source/Binaries/Win64"
dll="$binaries/UnrealEditor-ModelContextProtocol.dll"
modules="$binaries/UnrealEditor.modules"
provenance="$binaries/ReleaseProvenance.json"
for required_input in "$dll" "$modules" "$provenance"; do
  [[ -f "$required_input" ]] || { printf 'Missing locally built release input: %s\n' "$required_input" >&2; exit 1; }
done

dll_sha=$(/usr/bin/sha256sum "$dll" | /usr/bin/awk '{print $1}')
modules_sha=$(/usr/bin/sha256sum "$modules" | /usr/bin/awk '{print $1}')
source_sha=$(
  cd "$repository_root"
  /usr/bin/find ModelContextProtocol/Source ModelContextProtocol/Config ModelContextProtocol/ModelContextProtocol.uplugin \
    -type f -print0 | /usr/bin/sort -z | /usr/bin/xargs -0 /usr/bin/sha256sum | \
    /usr/bin/sha256sum | /usr/bin/awk '{print $1}'
)

node -e '
  const fs = require("fs");
  const [manifestPath, version, dllSha, modulesSha, sourceSha] = process.argv.slice(1);
  const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
  const smoke = manifest.runtime_smoke || {};
  const valid = manifest.version === version &&
    manifest.dll_sha256 === dllSha &&
    manifest.modules_sha256 === modulesSha &&
    manifest.source_sha256 === sourceSha &&
    smoke.class_path === "/Script/ModelContextProtocol.ModelContextProtocolSettings" &&
    smoke.default_port === 18777 && smoke.enumerated === true &&
    smoke.enumerated_count === 1 && smoke.is_developer_settings === true;
  if (!valid) {
    console.error("Release provenance does not match the current source, DLL, modules, and runtime smoke requirements.");
    process.exit(1);
  }
' "$provenance" "$version" "$dll_sha" "$modules_sha" "$source_sha"

work_root=$(mktemp -d "${TMPDIR:-/tmp}/unrealmcp-prebuilt-package.XXXXXX")
staging_root="$work_root/Staging"
plugin_package="$staging_root/Plugins/ModelContextProtocol"
mkdir -p "$plugin_package/Binaries/Win64"

cleanup_work() {
  local temp_root=${TMPDIR:-/tmp}
  if [[ -d "$work_root" && "$work_root" == "$temp_root"/unrealmcp-prebuilt-package.* ]]; then
    rm -rf -- "$work_root"
  fi
}
trap cleanup_work EXIT

/usr/bin/tar \
  --exclude='./Binaries' \
  --exclude='./Intermediate' \
  --exclude='./DerivedDataCache' \
  --exclude='./Saved' \
  -C "$plugin_source" \
  -cf - . | /usr/bin/tar -C "$plugin_package" -xf -
cp "$dll" "$modules" "$provenance" "$plugin_package/Binaries/Win64/"
cp "$repository_root/README.md" "$plugin_package/README.md"
cp "$repository_root/README.zh-CN.md" "$plugin_package/README.zh-CN.md"
cp "$repository_root/LICENSE" "$plugin_package/LICENSE"
cp -R "$repository_root/docs" "$plugin_package/docs"

required_paths=(
  "$plugin_package/ModelContextProtocol.uplugin"
  "$plugin_package/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll"
  "$plugin_package/Binaries/Win64/UnrealEditor.modules"
  "$plugin_package/Binaries/Win64/ReleaseProvenance.json"
  "$plugin_package/Source/ModelContextProtocol/Public/ModelContextProtocolSettings.h"
)
for required_path in "${required_paths[@]}"; do
  [[ -e "$required_path" ]] || { printf 'GitHub package is missing: %s\n' "$required_path" >&2; exit 1; }
done

if [[ -z "${SYSTEMROOT:-}" ]]; then
  printf '%s\n' 'SYSTEMROOT is unavailable; this release packager requires Windows.' >&2
  exit 1
fi
windows_tar=$(cygpath -u "$SYSTEMROOT/System32/tar.exe")
if [[ ! -x "$windows_tar" ]]; then
  printf 'Windows bsdtar was not found: %s\n' "$windows_tar" >&2
  exit 1
fi
"$windows_tar" --format zip -cf "$output_file" -C "$staging_root" Plugins
printf 'Packaged locally verified GitHub release asset: %s\n' "$output_file"
printf 'SHA-256: '
/usr/bin/sha256sum "$output_file" | /usr/bin/awk '{print $1}'
