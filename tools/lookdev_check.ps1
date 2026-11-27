<#
  MH-1 ルックデブと検証のハーネス。tests/golden/entries/lookdev_*.json の 4 シーン（grid / grazing / glass / emissive_room）を
  shadingRevision 1（V1）と 2（UNO_SHADING_V2）の両方で撮り、JSON と終了コードで合否を返す。
  エンジンは tools/engine_instance.ps1 経由の --headless（窓もカーソルも使わない）。止めるのは自分のインスタンスだけ。

    pwsh tools\lookdev_check.ps1                       # 4 シーン x V1/V2 を撮って判定
    pwsh tools\lookdev_check.ps1 -Only lookdev_grid    # 1 シーンだけ
    pwsh tools\lookdev_check.ps1 -V2RecordOnly         # V2 の差を記録だけ（合否に使わない。MH-2 で V2 を意図して変えた後）

  出力: <OutDir>\lookdev_check.json（既定 %USERPROFILE%\dx12-scorecard\lookdev）、撮った絵、V1/V2 の差分画像。
  終了コード: 0 合格 / 1 不合格 / 2 エラー / 3 GPU の枠が空かなかった(あとでもう一度。待ちの既定 8 分、DX12_SLOT_WAIT_MIN)。
  GPU の時間（mainScene）は JSON に出すが合否には使わない（性能の合否は夜間の計測窓）。
  FLIP は tools\parity の venv（pwsh tools\parity\setup.ps1）があるときだけ。無ければ JSON に skipped と出る。
  並行レーンと取り合わないよう、インスタンス名はリポジトリのパスから決まる。ポートは -Port か環境変数 LOOKDEV_PORT
  （worktree_setup.ps1 が表示した自分のレーンのもの）が必須。他レーンのポートを取らないため、空きの自動選択はしない（無ければ終了コード 2）。
  -PtReference: PT（render_reference）で基準画像を作り、最後のリビジョン（V2）と FLIP を比べて -PtFlipMax（既定 0.08）で判定する。
    1 シーン 20〜40 秒の GPU を食うので離席中に回す。FLIP は tools\parity の venv が要る。
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
    [string]$FlipPython = '',
    [int]$Repeat = 0,
    [string]$Revisions = '',
    [switch]$NoSaveImages,
    [switch]$PtReference,
    [int]$PtSpp = 0,
    [double]$PtFlipMax = 0.08
)
$ErrorActionPreference = 'Stop'
# 起動前の失敗（node が無い等）は「不合格(1)」ではなく「エラー(2)」で返す
try {
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
    if ($Repeat -gt 0) { $argv += @('--repeat', "$Repeat") }
    if ($Revisions) { $argv += @('--revisions', $Revisions) }
    if ($NoSaveImages) { $argv += '--no-save-images' }
    if ($PtReference) { $argv += @('--pt-reference', '--pt-flip-max', "$PtFlipMax") }
    if ($PtSpp -gt 0) { $argv += @('--pt-spp', "$PtSpp") }
    # GPU の枠(tools/lib/SlotLock.psm1)。取れなければ終了コード 3(あとでもう一度。0/1/2 とは別)。子の engine_instance は入れ子で通る
    Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
    $gpuSlot = Enter-DxSlot -Kind gpu -Label 'lookdev_check'
    if (-not $gpuSlot) { Write-Host '[lookdev] retry: GPU の枠が空かなかった(終了コード 3)'; exit 3 }
    Set-DxSlotPriority $gpuSlot
    Push-Location $root
    try { & $node @argv; $code = $LASTEXITCODE } finally { Pop-Location; Exit-DxSlot $gpuSlot }
    exit $code
} catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 2
}
