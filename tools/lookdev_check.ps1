<#
  MH-1 ルックデブと検証のハーネス。tests/golden/entries/lookdev_*.json の 4 シーン（grid / grazing / glass / emissive_room）を
  shadingRevision 1（V1）と 2（UNO_SHADING_V2）の両方で撮り、JSON と終了コードで合否を返す。
  エンジンは tools/engine_instance.ps1 経由の --headless（窓もカーソルも使わない）。止めるのは自分のインスタンスだけ。

    pwsh tools\lookdev_check.ps1                       # 4 シーン x V1/V2 を撮って判定
    pwsh tools\lookdev_check.ps1 -Only lookdev_grid    # 1 シーンだけ
    pwsh tools\lookdev_check.ps1 -V2RecordOnly         # V2 の差を記録だけ（合否に使わない。MH-2 で V2 を意図して変えた後）

  出力: <OutDir>\lookdev_check.json（既定 %USERPROFILE%\dx12-scorecard\lookdev）、撮った絵、V1/V2 の差分画像。
  終了コード: 0 合格 / 1 不合格 / 2 エラー。
  GPU の時間（mainScene）は JSON に出すが合否には使わない（性能の合否は夜間の計測窓）。
  FLIP は tools\parity の venv（pwsh tools\parity\setup.ps1）があるときだけ。無ければ JSON に skipped と出る。
  並行レーンと取り合わないよう、インスタンス名はリポジトリのパスから決まる。ポートは -Port か環境変数 LOOKDEV_PORT
  （worktree_setup.ps1 が表示した自分のレーンのもの）。省略時は 8850..8899 の空き。
#>
param(
    [string[]]$Only = @(),
    [string]$OutDir = '',
    [string]$Exe = '',
    [int]$Port = 0,
    [int]$PerfWindow = 120,
    [double]$V2MaxMeanAbs = 0.25,
    [double]$V2MaxDiffRatio = 0.001,
    [switch]$V2RecordOnly,
    [string]$ReferenceDir = '',
    [string]$FlipPython = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$node = (Get-Command node -ErrorAction Stop).Source
$argv = @((Join-Path $root 'tools\lookdev\lookdev_check.mjs'))
if ($Only.Count) { $argv += @('--only', ($Only -join ',')) }
if ($OutDir) { $argv += @('--out', $OutDir) }
if ($Exe) { $argv += @('--exe', $Exe) }
if ($Port -gt 0) { $argv += @('--port', "$Port") }
$argv += @('--perf-window', "$PerfWindow", '--v2-max-mean-abs', "$V2MaxMeanAbs", '--v2-max-diff-ratio', "$V2MaxDiffRatio")
if ($V2RecordOnly) { $argv += '--v2-record-only' }
if ($ReferenceDir) { $argv += @('--reference-dir', $ReferenceDir) }
if ($FlipPython) { $argv += @('--flip-python', $FlipPython) }
Push-Location $root
try { & $node @argv; exit $LASTEXITCODE } finally { Pop-Location }
