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

# --- バージョンを Version.cpp から取得（定数の実体は Version.cpp に一本化した） ---
$verLine = Select-String -Path (Join-Path $repoRoot "src\core\Version.cpp") -Pattern 'kEngineVersion\s*=\s*"([0-9.]+)"'
if (-not $verLine) { Write-Error "Version.cpp から kEngineVersion を取得できへんかった" }
$version = $verLine.Matches[0].Groups[1].Value
Write-Host "DX12 Engine v$version の配布物をビルドするで" -ForegroundColor Cyan

if (-not (Test-Path (Join-Path $srcDir "DX12Engine.exe"))) {
  Write-Error "build\release\DX12Engine.exe が無い。先に build_release.bat を実行してや。"
}

# --- 再コンパイル漏れ(stale obj)検査 ---
# v1.4.2 と v1.4.4 の 2 度、「ヘッダー変更後に一部 .obj だけ ninja が再コンパイルを
# 取りこぼした混成 exe」を配布する事故が起きた(構造体レイアウト不一致で即クラッシュ)。
# ここでは src/ 以下の最新ソース更新時刻より古い .obj が build\release に残っていたら
# パッケージを中断する。誤検知(git checkout での mtime 更新など)でも、クリーン
# ビルドし直せば必ず通る=安全側に倒す。
# .cpp は自 TU の直接依存なので ninja が取りこぼさない。事故るのは「ヘッダー変更 →
# それを include する他 TU の再コンパイル漏れ」なので、比較対象はヘッダーの最新時刻のみ。
$newestSrc = Get-ChildItem (Join-Path $repoRoot "src") -Recurse -Include *.h, *.hpp |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
$staleObjs = Get-ChildItem $srcDir -Recurse -Filter *.obj |
  Where-Object { $_.LastWriteTime -lt $newestSrc.LastWriteTime }
if ($staleObjs) {
  $names = ($staleObjs | Select-Object -First 8 | ForEach-Object Name) -join ", "
  Write-Error ("再コンパイル漏れの疑い: 最新ヘッダー ($($newestSrc.Name) " +
    "$($newestSrc.LastWriteTime)) より古い .obj が $($staleObjs.Count) 個ある ($names ...)。" +
    " build\release を削除してクリーンビルドしてからやり直してや。")
}
Write-Host "stale obj 検査 OK (全 obj が最新ソース以降にコンパイル済み)" -ForegroundColor Green

# --- ビルド済み exe の版がソースと一致するか検証 ---
# 過去に Version 変更後の再コンパイルを VS が一部 obj で取りこぼし、「自分を旧版と思い込む
# Updater」入りの exe を配布 → 自動更新が無限ループする事故があった（v1.2.2〜v1.4.2）。
# exe 自身に --write-version で版を自己申告させ、ズレていたらパッケージを中断する。
$verProbe = Join-Path $outDir "exe_version_probe.txt"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path $verProbe) { Remove-Item -Force $verProbe }
$proc = Start-Process -FilePath (Join-Path $srcDir "DX12Engine.exe") `
  -ArgumentList "--write-version", "`"$verProbe`"" -WorkingDirectory $srcDir -Wait -PassThru
$exeVersion = if (Test-Path $verProbe) { (Get-Content $verProbe -Raw).Trim() } else { "" }
Remove-Item -Force $verProbe -ErrorAction SilentlyContinue
if ($proc.ExitCode -ne 0 -or -not $exeVersion) {
  Write-Error "exe の版の自己申告(--write-version)に失敗した。旧版の exe が残ってへんか、build_release.bat からやり直してや。"
}
if ($exeVersion -ne $version) {
  Write-Error "版の不一致: Version.cpp は $version やのに exe は $exeVersion と自己申告した。再コンパイル漏れの疑い。build\release を消してクリーンビルドしてや。"
}
Write-Host "exe の版検証 OK (v$exeVersion)" -ForegroundColor Green

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
