#Requires -Version 7
<#
  性能のベースライン（W05-39）を計測窓の中で撮る。ビルドの Mutex（Global\dx12-build）を握ったまま baseline_perf.mjs を走らせる
  （計測中はビルドが走らない）。GPU の計測窓ロック（%TEMP%\uno-gpu-window.lock）は baseline_perf.mjs が取る。
  窓は出さない（エンジンは --headless）。クロックの固定はしない。

    pwsh tools/scorecard/baseline_perf.ps1 snapshot --label pre-overhaul --engine-dir $env:USERPROFILE\dx12-preoverhaul --no-dred
    pwsh tools/scorecard/baseline_perf.ps1 snapshot --label w05-end --engine-dir $env:USERPROFILE\dx12-nightly --no-dred
    pwsh tools/scorecard/baseline_perf.ps1 ab --id dred --a-dir <nightly> --a-label w05-end-nodred --a-no-dred --b-dir <nightly> --b-label w05-end-dred
    # F0 は M2 の前後、hooks は M4 の前後のバイナリを比べる（pre-overhaul と w05-end の比較にすると W0.5 の全変更が混ざる）。
    #   tools/nightly.ps1 -Ref <M2 の直前のコミット> -Out dx12-before-m2 と -Ref <M2 のコミット> -Out dx12-after-m2 で用意する。
    pwsh tools/scorecard/baseline_perf.ps1 ab --id f0 --a-dir <before-m2> --a-label before-m2 --a-no-dred --b-dir <after-m2> --b-label after-m2 --b-no-dred
    pwsh tools/scorecard/baseline_perf.ps1 ab --id hooks --a-dir <M4 の直前> --a-label before-m4 --a-no-dred --b-dir <M4> --b-label after-m4 --b-no-dred
    # VRAM の門: DXGI の Budget が 6500MB 以上（nvidia-smi の使用量は引かない。--vram-min-mb は protocol に記録され、試走の扱いにはならない）。
    # DRED の旗は --a-no-dred / --b-no-dred でも --a-args "--no-dred" でも同じ意味（--dred は拒否）。
    pwsh tools/scorecard/baseline_perf.ps1 probe --engine-dir <nightly>    # VRAM の門の値だけを表示（離席中に 1 回）
    # 試走（--frames などを変えた実行）は --out を明示しないと拒否される（公式の JSON に混ぜないため）。
    pwsh tools/scorecard/baseline_perf.ps1 validate --require pre-overhaul,w05-end,dred,f0,hooks

  先頭の引数が snapshot | ab | probe | validate。残りは baseline_perf.mjs にそのまま渡る。
  ビルドの待ち時間は環境変数 PERF_BUILD_WAIT_MIN（既定 0 = 取れなければ終了コード 3）。
#>
$ErrorActionPreference = 'Stop'
$node = (Get-Command node).Source
$mjs = Join-Path $PSScriptRoot 'baseline_perf.mjs'
if ($args.Count -gt 0 -and $args[0] -eq 'validate') { & $node $mjs @args; exit $LASTEXITCODE }

$wait = 0; if ($env:PERF_BUILD_WAIT_MIN) { $wait = [int]$env:PERF_BUILD_WAIT_MIN }
$mutex = [System.Threading.Mutex]::new($false, 'Global\dx12-build')
$got = $false
try {
    try { $got = $mutex.WaitOne([TimeSpan]::FromMinutes($wait)) } catch [System.Threading.AbandonedMutexException] { $got = $true }
    if (-not $got) { Write-Host '[perf] ビルドの Mutex を取れない（ビルド中）。計測窓を取れないので終了する。'; exit 3 }
    try { [Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal' } catch {}
    & $node $mjs @args
    exit $LASTEXITCODE
} finally {
    if ($got) { try { $mutex.ReleaseMutex() } catch {} }
    $mutex.Dispose()
}
