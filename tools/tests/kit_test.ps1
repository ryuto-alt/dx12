<#
  tools/kit.ps1 のテスト(エンジン・ビルド・GPU は動かさない。引数の検査、出力の行数上限、終了コード、ログの書き出しを見る)
    pwsh tools/tests/kit_test.ps1
#>
$ErrorActionPreference = 'Stop'
$kit = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit.ps1'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('dx12-kit-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:KIT_LOG_DIR = Join-Path $tmp 'logs'
$fail = 0
function Assert-That([bool]$c, [string]$m) { if ($c) { Write-Host "  PASS: $m" } else { Write-Host "  FAIL: $m"; $script:fail++ } }
function Run-Kit([string[]]$a) {
    $o = @(& pwsh -NoProfile -File $kit @a 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = $o; Text = ($o -join "`n") }
}
try {
    Write-Host '(a) status'
    $r = Run-Kit @('status')
    Assert-That ($r.Code -eq 0) 'status exits 0'
    Assert-That ($r.Lines.Count -le 40) "status is at most 40 lines ($($r.Lines.Count))"
    Assert-That ($r.Text -match 'slots: gpu: \d+/\d+ busy \| test: \d+/\d+ busy') 'status shows gpu/test slot state'
    Assert-That ($r.Text -match 'ports 8881-8889 in use') 'status shows the allowed port range'

    Write-Host '(b) argument checks and exit codes'
    Assert-That ((Run-Kit @('bogus')).Code -eq 2) 'unknown subcommand -> 2'
    Assert-That ((Run-Kit @('screenshot')).Code -eq 2) 'screenshot without -Port -> 2'
    Assert-That ((Run-Kit @('verify')).Code -eq 2) 'verify without id -> 2'
    Assert-That ((Run-Kit @('golden-subset')).Code -eq 2) 'golden-subset without ids -> 2'
    $r = Run-Kit @('engine-start', '-Port', '8851')
    Assert-That (($r.Code -eq 2) -and ($r.Text -match '範囲外')) 'engine-start refuses a port outside 8881-8889 (8851-8860 belong to another workflow)'

    Write-Host '(c) a failing child: compact output, exit code passed through, full log written'
    $r = Run-Kit @('test-changed', '-Dir', (Join-Path $tmp 'no-such-build'))
    Assert-That ($r.Code -ne 0) "failure exit code passed through ($($r.Code))"
    Assert-That ($r.Lines.Count -le 40) "output at most 40 lines ($($r.Lines.Count))"
    Assert-That ($r.Text -match '\[kit\] test-changed: FAILED exit=\d+ .* log=') 'summary line has exit code and log path'
    $log = [regex]::Match($r.Text, 'log=(.+)$', 'Multiline').Groups[1].Value.Trim()
    Assert-That ((Test-Path -LiteralPath $log) -and ((Get-Content -LiteralPath $log -Raw) -match 'CTestTestfile')) 'full log was written'

    Write-Host '(d) screenshot without an engine reports a JSON error'
    $r = Run-Kit @('screenshot', '-Port', '8889')
    Assert-That (($r.Code -ne 0) -and ($r.Text -match '"ok":false')) 'no engine -> ok:false, non-zero'

    Write-Host '(e) golden-subset confines golden.mjs engines to the kit port range (8851-8860 are another workflow)'
    $fakeBin = Join-Path $tmp 'fakebin'; New-Item -ItemType Directory -Force $fakeBin | Out-Null
    Set-Content (Join-Path $fakeBin 'node.cmd') '@echo PORTRANGE=%DX12_BENCH_PORT_START%-%DX12_BENCH_PORT_END%' -Encoding ascii
    $savedPath = $env:PATH; $env:PATH = "$fakeBin;$env:PATH"
    try { $r = Run-Kit @('golden-subset', 'a,b') } finally { $env:PATH = $savedPath }
    Assert-That ($r.Text -match 'PORTRANGE=8881-8889') "golden child gets DX12_BENCH_PORT_START/END ($($r.Text -replace '\s+', ' '))"

    Write-Host '(f) port reservation: a reserved port is not handed out twice'
    $savedLocal = $env:LOCALAPPDATA; $env:LOCALAPPDATA = Join-Path $tmp 'local'
    $pd = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-ports'; New-Item -ItemType Directory -Force $pd | Out-Null
    $sl = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep 120')
    try {
        foreach ($p in 8881..8889) { (@{ name = 'other-agent'; kitPid = $sl.Id; port = $p } | ConvertTo-Json -Compress) | Set-Content (Join-Path $pd "$p.json") }
        $r = Run-Kit @('engine-start', '-Port', '8885')
        Assert-That (($r.Code -eq 2) -and ($r.Text -match '予約中')) "explicit port reserved by a live owner is refused ($($r.Text -replace '\s+', ' '))"
        $r = Run-Kit @('engine-start')
        Assert-That (($r.Code -ne 0) -and ($r.Text -match '空きポート')) 'all ports reserved -> no free port (no double hand-out)'
        $sl.Kill(); $null = $sl.WaitForExit(5000)   # 持ち主が死んだ予約は古いものとして捨てられる(ここでは予約だけ確かめ、エンジンは起動しない)
        $r = Run-Kit @('engine-start', '-Port', '8885', '-Mode', 'headless', '-Project', (Join-Path $tmp 'noproj'), '-BuildDir', (Join-Path $tmp 'no-such-build'))
        Assert-That ($r.Text -notmatch '予約中') 'stale reservation (dead owner) is dropped'
        & pwsh -NoProfile -File $kit engine-stop -Name "kit-$(Split-Path -Leaf (Split-Path -Parent $PSScriptRoot))" | Out-Null
    } finally { if (-not $sl.HasExited) { $sl.Kill() }; $env:LOCALAPPDATA = $savedLocal }
}
finally { Remove-Item Env:\KIT_LOG_DIR -ErrorAction SilentlyContinue; Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
if ($fail -gt 0) { Write-Host "FAILED: $fail"; exit 1 }
Write-Host 'ALL PASSED'
