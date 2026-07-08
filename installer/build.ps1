#!/usr/bin/env pwsh
# DX12 Engine 配布物ビルド（個人用）。
#   1) Inno Setup インストーラを生成（installer\output\DX12Engine-Setup-vX.Y.Z.exe）
#   2) 自動アップデート用 zip を生成（installer\output\dx12-engine-vX.Y.Z.zip）
#      … exe 隣に assets/ が入った構成。GitHub Releases に上げると
#        起動時オートアップデートで配布される。
#
# ★ MCP サーバは配布物に同梱しない。別リポジトリ https://github.com/ryuto-alt/dx12-mcp から
#   インストールする（エディタの「MCP / AI Bridge」パネルが手順を案内する）。
#
# 前提: build\release が先にビルド済みであること（build_release.bat 等）。ISCC は任意。
# 使い方: pwsh -ExecutionPolicy Bypass -File installer\build.ps1
$ErrorActionPreference = "Stop"

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path   # installer/
$repoRoot = Split-Path -Parent $here
$srcDir   = Join-Path $repoRoot "build\release"               # exe + dll + shaders
$outDir   = Join-Path $here "output"

# --- バージョンを Version.h から取得 ---
$verLine = Select-String -Path (Join-Path $repoRoot "src\core\Version.h") -Pattern 'kEngineVersion\s*=\s*"([0-9.]+)"'
if (-not $verLine) { Write-Error "Version.h から kEngineVersion を取得できへんかった" }
$version = $verLine.Matches[0].Groups[1].Value
Write-Host "DX12 Engine v$version の配布物をビルドするで" -ForegroundColor Cyan

if (-not (Test-Path (Join-Path $srcDir "DX12Engine.exe"))) {
  Write-Error "build\release\DX12Engine.exe が無い。先に build_release.bat を実行してや。"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# --- 1) Inno Setup インストーラ ---
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue)
if (-not $iscc) {
  foreach ($p in @("$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                   "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $p) { $iscc = $p; break }
  }
}
if ($iscc) {
  Write-Host "[1/2] インストーラを生成 (ISCC)..." -ForegroundColor Yellow
  & $iscc "/DMyAppVersion=$version" (Join-Path $here "dx12engine.iss")
} else {
  Write-Warning "[1/2] ISCC.exe が見つからへんのでインストーラはスキップ（Inno Setup 6 を入れてな）。"
}

# --- 2) 自動アップデート用 zip ---
Write-Host "[2/2] 自動アップデート用 zip を生成..." -ForegroundColor Yellow
$stage = Join-Path $outDir "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# exe + dll（build\release 直下）
Get-ChildItem -Path $srcDir -File | Where-Object { $_.Extension -in ".exe", ".dll" } |
  ForEach-Object { Copy-Item $_.FullName -Destination $stage }
# shaders（.cso）
Copy-Item (Join-Path $srcDir "shaders") -Destination (Join-Path $stage "shaders") -Recurse -ErrorAction SilentlyContinue
# shaders-src（.hlsl/.hlsli ソース。配布エディタでのシェーダーホットリロード/エンジンシェーダー編集用。
# PathResolver::ShaderSourceDirW() が配布時に exe 隣の shaders-src/ を見る）
Copy-Item (Join-Path $repoRoot "shaders") -Destination (Join-Path $stage "shaders-src") -Recurse -ErrorAction SilentlyContinue
# assets
Copy-Item (Join-Path $repoRoot "assets") -Destination (Join-Path $stage "assets") -Recurse
# ★ tools\mcp-server は同梱しない（別リポジトリ ryuto-alt/dx12-mcp から配布）
# tools\lua-defs（VSCode 補完用の Lua API 型定義）
$luaDefs = Join-Path $repoRoot "tools\lua-defs"
if (Test-Path $luaDefs) {
  Copy-Item $luaDefs -Destination (Join-Path $stage "tools\lua-defs") -Recurse
}
# version.txt（更新後の確認用）
Set-Content -Path (Join-Path $stage "version.txt") -Value $version -NoNewline
# README
Copy-Item (Join-Path $here "README.txt") -Destination $stage -ErrorAction SilentlyContinue

$zip = Join-Path $outDir "dx12-engine-v$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage

Write-Host ""
Write-Host "=== 完了 ===" -ForegroundColor Green
Write-Host "  zip: $zip"
Write-Host "  リリース公開: gh release create v$version -R ryuto-alt/dx12 --target <branch> `"$zip`""
Write-Host "  ※ MCP サーバは同梱しない。https://github.com/ryuto-alt/dx12-mcp からインストール。"
Write-Host "  ※ zip はこのファイル名のままアップロードすること（#別名 で rename しない）。"
Write-Host "     Updater.cpp が api.github.com 不要の直リンク "
Write-Host "     (releases/download/vX.Y.Z/dx12-engine-vX.Y.Z.zip) をこの名前で決め打ちしている。"
