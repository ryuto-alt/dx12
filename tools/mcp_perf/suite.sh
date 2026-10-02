#!/usr/bin/env bash
# MCP 全 method の計測を、シーンごとに read 系 → write 系の順で回す。
#   bash tools/mcp_perf/suite.sh <port> <出力フォルダ> <シーン(assets 相対)>... [-- --write-args file.json --read-args file.json]
#
# 前提(AGENT_RULES どおり):
#   - エンジンは tools/engine_instance.ps1 で背景起動済みで、-Project は使い捨てコピー(実プロジェクトは触らない)。
#   - write 系はシーンを書き換える(MCP 接続中の自動保存でシーン JSON が上書きされる)。実行後は使い捨てコピー側のシーンを元に戻すこと。
#   - 出力: <出力フォルダ>/read_<シーン名>.txt|json, write_<シーン名>.txt|json
here="$(cd "$(dirname "$0")" && pwd)"
port="$1"; out="$2"; shift 2
scenes=(); readArgs="$here/read_args.json"; writeArgs="$here/write_args.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --) shift ;;
    --write-args) writeArgs="$2"; shift 2 ;;
    --read-args) readArgs="$2"; shift 2 ;;
    *) scenes+=("$1"); shift ;;
  esac
done
mkdir -p "$out"
for sc in "${scenes[@]}"; do
  base="$(basename "$sc" .json)"
  echo "=== $sc"
  node "$here/call.mjs" "$port" open_scene "{\"path\":\"$sc\"}" 1 120 | head -1
  node "$here/mcp_perf.mjs" "$port" --effect read --args "$readArgs" --out "$out/read_$base.json" > "$out/read_$base.txt" 2>&1
  grep -E "超過|ms=" "$out/read_$base.txt" | head -20
  node "$here/mcp_perf.mjs" "$port" --effect write --args "$writeArgs" --timeout 600000 --out "$out/write_$base.json" > "$out/write_$base.txt" 2>&1
  grep -E "超過|ms=" "$out/write_$base.txt" | head -20
done
