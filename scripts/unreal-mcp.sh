#!/usr/bin/env bash
set -euo pipefail

url="${UE_MCP_URL:-http://127.0.0.1:18777/mcp}"
session_id="${UE_MCP_SESSION_ID:-00000000-0000-4000-8000-000000000001}"
request_id="${UE_MCP_REQUEST_ID:-1}"

usage() {
  printf '%s\n' \
    'Usage:' \
    '  scripts/unreal-mcp.sh --initialize' \
    '  scripts/unreal-mcp.sh --list' \
    '  scripts/unreal-mcp.sh '\''{"action":"health"}'\''' \
    '' \
    'Environment: UE_MCP_URL, UE_MCP_TOKEN, UE_MCP_SESSION_ID, UE_MCP_REQUEST_ID'
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

case "$1" in
  --initialize)
    payload=$(printf '{"jsonrpc":"2.0","id":%s,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"unreal-mcp-bash","version":"1.0"}}}' "$request_id")
    ;;
  --list)
    payload=$(printf '{"jsonrpc":"2.0","id":%s,"method":"tools/list","params":{}}' "$request_id")
    ;;
  --help|-h)
    usage
    exit 0
    ;;
  *)
    payload=$(printf '{"jsonrpc":"2.0","id":%s,"method":"tools/call","params":{"name":"unreal","arguments":%s}}' "$request_id" "$1")
    ;;
esac

curl_args=(
  --silent
  --show-error
  --max-time "${UE_MCP_CURL_TIMEOUT:-3600}"
  --header 'Content-Type: application/json'
  --header 'Accept: application/json, text/event-stream'
  --header "Mcp-Session-Id: $session_id"
  --data-binary @-
)
if [[ -n "${UE_MCP_TOKEN:-}" ]]; then
  curl_args+=(--header "Authorization: Bearer $UE_MCP_TOKEN")
fi

printf '%s' "$payload" | curl "${curl_args[@]}" "$url"
printf '\n'
