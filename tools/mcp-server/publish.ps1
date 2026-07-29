#!/usr/bin/env pwsh
# MCP サーバを配布リポジトリ https://github.com/ryuto-alt/dx12-mcp へ同期する。
# ソース・オブ・トゥルースはこのフォルダ(tools/mcp-server)。変更をコミットしたら実行してや。
# 使い方: pwsh -ExecutionPolicy Bypass -File tools\mcp-server\publish.ps1 [-Message "コミットメッセージ"]
param([string]$Message = "sync from dx12 engine repo")
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path   # tools/mcp-server
$work = Join-Path ([IO.Path]::GetTempPath()) "dx12-mcp-publish"

# clone or pull
if (Test-Path (Join-Path $work ".git")) {
  git -C $work pull --ff-only
} else {
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
  git clone https://github.com/ryuto-alt/dx12-mcp $work
}

# コミット identity をエンジン repo から引き継ぐ（グローバル未設定の環境対策）
$engineRepo = Split-Path -Parent (Split-Path -Parent $here)
git -C $work config user.name  (git -C $engineRepo config user.name)
git -C $work config user.email (git -C $engineRepo config user.email)

# 配布対象ファイルをミラー（node_modules / ログ / .git / publish.ps1 自身は除外）
# ponytail: 列挙やなくglob。ファイル追加したのにここに書き忘れる事故(ui*.ts 漏れ)を防ぐ。
$files = Get-ChildItem $here -File -Force | Where-Object {
  $_.Name -ne "publish.ps1" -and
  ($_.Extension -in ".ts", ".json", ".ps1", ".sh", ".md" -or $_.Name -in ".mcp.json.example", ".gitignore")
}
foreach ($f in $files) {
  Copy-Item $f.FullName -Destination $work -Force
}
Write-Host ("同期対象: " + ($files.Name -join ", "))

git -C $work add -A
if (git -C $work status --porcelain) {
  git -C $work commit -m $Message
  git -C $work push
  Write-Host "dx12-mcp へ同期完了" -ForegroundColor Green
} else {
  Write-Host "差分なし。同期不要や" -ForegroundColor Yellow
}
