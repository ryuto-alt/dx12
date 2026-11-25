<#
  ctest のラッパー。昼に GPU テストや UI テストを走らせてしまわないための既定を持つ。

    pwsh tools/ctest.ps1                       # 既定: gpu と ui のラベルを除外して実行
    pwsh tools/ctest.ps1 -R Anchor             # 名前の正規表現（ctest の -R をそのまま渡す）
    pwsh tools/ctest.ps1 -Gpu                  # gpu ラベルも含める（オーナーの離席中か、メインの指示があるときだけ）
    pwsh tools/ctest.ps1 -Ui                   # ui ラベルも含める（UiTests はエンジンを起動する。同上）
    pwsh tools/ctest.ps1 -N                    # 実行せず一覧だけ
    pwsh tools/ctest.ps1 -Dir build\tracy      # 別ビルドディレクトリ

  やっていること:
    1. VS 同梱の ctest.exe を自動で探す（PATH の ctest は使わない）。
    2. プロセス優先度を BelowNormal にする（子プロセスに継承される）。
    3. -j3 --timeout 300 --output-on-failure。
    4. 既定で -LE "^(gpu|ui)$"（ラベルは小文字。-Gpu で gpu、-Ui で ui を除外から外す）。
  ビルドはしない。先に pwsh tools/build.ps1 -Tests で建てておくこと。
#>
param(
    [string]$R = '',
    [switch]$Gpu,
    [switch]$Ui,
    [switch]$N,
    [string]$Dir = 'build\release',
    [int]$Jobs = 3,
    [int]$Timeout = 300,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# --- 1. ctest.exe を探す（VS 同梱）
$ctest = $null
$candidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
)
foreach ($c in $candidates) { if (Test-Path $c) { $ctest = $c; break } }
if (-not $ctest) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vs) {
            $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
            if (Test-Path $p) { $ctest = $p }
        }
    }
}
if (-not $ctest) {
    $cmd = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($cmd) { $ctest = $cmd.Source }
}
if (-not $ctest) { Write-Host '[ctest] ctest.exe が見つかりません'; exit 2 }

$buildDir = if ([IO.Path]::IsPathRooted($Dir)) { $Dir } else { Join-Path $root $Dir }
if (-not (Test-Path (Join-Path $buildDir 'CTestTestfile.cmake'))) {
    Write-Host "[ctest] CTestTestfile.cmake がありません: $buildDir（先に configure / build.ps1 -Tests）"
    exit 2
}

# --- 2. 優先度
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}

# --- 3/4. 引数
$exclude = @()
if (-not $Gpu) { $exclude += 'gpu' }
if (-not $Ui)  { $exclude += 'ui' }

$args2 = @('--test-dir', $buildDir, '-j', "$Jobs", '--timeout', "$Timeout", '--output-on-failure')
if ($exclude.Count -gt 0) { $args2 += @('-LE', ('^(' + ($exclude -join '|') + ')$')) }
if ($R) { $args2 += @('-R', $R) }
if ($N) { $args2 += '-N' }
if ($Rest) { $args2 += $Rest }

Write-Host "[ctest] $ctest $($args2 -join ' ')"
& $ctest @args2
exit $LASTEXITCODE
