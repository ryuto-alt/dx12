<#
  NVIDIA DLSS SDK(NGX)を third_party/dlss へ取得する。

    pwsh tools/fetch_dlss.ps1              # 固定のタグ(下の $Tag)とコミット($ExpectedCommit)を取得
    pwsh tools/fetch_dlss.ps1 -Tag vX.Y.Z -ExpectedCommit <sha>   # 版を上げるとき(両方を一緒に変える)

  - 版の正はこのファイルの $Tag と $ExpectedCommit(注釈付きタグの指すコミット)。取得後に HEAD が
    $ExpectedCommit と一致しなければ失敗する(タグの付け替え・取得先のすり替えへの備え)。
    DLL の実体は git-lfs が pointer の sha256 で照合する。

  - SDK はリポジトリにコミットしない(.gitignore 済み)。NVIDIA RTX SDK ライセンスは
    「SDK をオープンソースのライセンス条件に服させる形で結合・配布しない」ことを求めるため、
    MIT で公開しているこのリポジトリには入れず、各自がここで取得する方式にしている。
    取得後のライセンス本文は third_party/dlss/LICENSE.txt。
  - 実行に要る nvngx_dlss.dll は CMake(UNO_ENABLE_DLSS)が exe の隣へコピーする。
  - 外へ出るのは github.com への git clone(+ LFS の取得)だけ。
#>
param(
    [string]$Tag = 'v310.9.16',
    # タグ v310.9.16 が指すコミット(git ls-remote の ^{} の値)。-Tag を変えるときは一緒に変える。
    [string]$ExpectedCommit = '9981c6350dc58c0ce21f024fd65e31e851c31689'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'third_party\dlss'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Write-Host '[dlss] git が見つからない'; exit 2 }
& git lfs version *> $null
if ($LASTEXITCODE -ne 0) { Write-Host '[dlss] git-lfs が要る(DLL は LFS で配られている)'; exit 2 }

if (Test-Path (Join-Path $dest '.git')) {
    Write-Host "[dlss] 既存の $dest を $Tag へ合わせる"
    & git -C $dest fetch --depth 1 origin "refs/tags/${Tag}:refs/tags/${Tag}"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & git -C $dest -c advice.detachedHead=false checkout $Tag
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} elseif (Test-Path $dest) {
    Write-Host "[dlss] $dest が git 以外の中身で存在する。手で退避してから実行して"
    exit 2
} else {
    Write-Host "[dlss] clone NVIDIA/DLSS $Tag -> $dest"
    & git -c advice.detachedHead=false clone --depth 1 --branch $Tag https://github.com/NVIDIA/DLSS.git $dest
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 固定したコミットと一致するか(LFS の取得より前に確かめる)
$head = (& git -C $dest rev-parse HEAD).Trim()
if ($head -ne $ExpectedCommit) {
    Write-Host "[dlss] コミットが固定値と違う: $head (期待 $ExpectedCommit)。タグが付け替えられたか、取得先が違う。中身を信用しない"
    exit 4
}
Write-Host "[dlss] コミット照合 OK: $head"

# 必要な Windows のバイナリだけ LFS から実体化する(Linux 向けや dlssd は取らない)
& git -C $dest lfs pull --include 'lib/Windows_x86_64/**'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$dll = Get-ChildItem -Path (Join-Path $dest 'lib\Windows_x86_64\rel') -Filter 'nvngx_dlss.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dll -or $dll.Length -lt 1MB) {
    Write-Host '[dlss] nvngx_dlss.dll が LFS のポインタのまま(実体が取れていない)'
    exit 1
}
Write-Host ("[dlss] OK: {0} ({1:n1} MB)。cmake を構成し直すと UNO_ENABLE_DLSS が自動で ON になる" -f $dll.FullName, ($dll.Length / 1MB))
