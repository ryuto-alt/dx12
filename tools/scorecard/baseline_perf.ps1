#Requires -Version 7
<#
  性能のベースライン（W05-39）を計測窓の中で撮る。ビルドの Mutex（Global\dx12-build）を握ったまま baseline_perf.mjs を走らせる
  （計測中はビルドが走らない）。GPU の計測窓ロック（%TEMP%\uno-gpu-window.lock）は baseline_perf.mjs が取る。
  窓は出さない（エンジンは --headless）。クロックの固定はしない。

    pwsh tools/scorecard/baseline_perf.ps1 snapshot --label pre-overhaul --engine-dir $env:USERPROFILE\dx12-preoverhaul --no-dred
    pwsh tools/scorecard/baseline_perf.ps1 snapshot --label w05-end --engine-dir $env:USERPROFILE\dx12-nightly --no-dred
    pwsh tools/scorecard/baseline_perf.ps1 ab --id dred --a-dir <nightly> --a-label w05-end-nodred --a-no-dred --b-dir <nightly> --b-label w05-end-dred
    pwsh tools/scorecard/baseline_perf.ps1 ab --id f0 --a-dir <preoverhaul> --a-label pre-overhaul --a-no-dred --b-dir <nightly> --b-label w05-end --b-no-dred
    pwsh tools/scorecard/baseline_perf.ps1 ab --id hooks --a-dir <hook 前> ... --b-dir <hook 後> ...
    pwsh tools/scorecard/baseline_perf.ps1 validate --require pre-overhaul,w05-end,dred,f0,hooks

  先頭の引数が snapshot | ab | validate。残りは baseline_perf.mjs にそのまま渡る。
  ビルドの待ち時間は環境変数 PERF_BUILD_WAIT_MIN（既定 0 = 取れなければ終了コード 3）。
  ANTHROPIC_API_KEY があれば起動を拒む。
#>
$ErrorActionPreference = 'Stop'
foreach ($scope in 'Process', 'User', 'Machine') {
    if ([Environment]::GetEnvironmentVariable('ANTHROPIC_API_KEY', $scope)) {
        Write-Host "[perf] ANTHROPIC_API_KEY が $scope に設定されている。API 課金を避けるため起動を拒否する。"; exit 2
    }
}
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
