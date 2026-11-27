<#
  tools/build.ps1 のスロット(同時ビルド数)制御のテスト。cmake は動かさず -SlotProbe だけを使う。
  テスト専用の Mutex 接頭辞(DX12_BUILD_SLOT_PREFIX)を使うので、他のエージェントの実ビルドには影響しない。
    pwsh tools/tests/build_slots_test.ps1
#>
$ErrorActionPreference = 'Stop'
$build = Join-Path (Split-Path -Parent $PSScriptRoot) 'build.ps1'
$tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ("dx12-slots-test-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
$env:DX12_BUILD_SLOT_PREFIX = 'Global\dx12-build-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$env:DX12_BUILD_MIN_FREE_GB = '0'
$env:DX12_BUILD_FAKE_IDLE_SEC = '99999'   # 既定は「利用者は離席中」
$fail = 0

function Assert-That([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "  PASS: $msg" } else { Write-Host "  FAIL: $msg"; $script:fail++ }
}

# probes = @( @{ Dir=...; Sec=... }, ... ) を一斉に起動し、各プローブの [acquired, released] (ms) と待ち時間を返す
function Invoke-Probes($probes, [int]$slots, [string]$tag) {
    $env:DX12_BUILD_SLOTS = "$slots"
    $procs = @(); $i = 0
    $t0 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    foreach ($p in $probes) {
        $i++
        $out = Join-Path $tmpRoot "$tag-$i.out"
        New-Item -ItemType Directory -Force -Path $p.Dir | Out-Null
        $procs += [pscustomobject]@{ Out = $out; Proc = (Start-Process pwsh -PassThru -WindowStyle Hidden -RedirectStandardOutput $out `
            -ArgumentList @('-NoProfile', '-File', $build, '-SlotProbe', $p.Sec, '-Dir', $p.Dir, '-WaitMinutes', 5)) }
        Start-Sleep -Milliseconds 700   # 起動順を安定させる
    }
    $res = @()
    foreach ($p in $procs) {
        $p.Proc.WaitForExit()
        $txt = Get-Content $p.Out -Raw
        $acq = if ($txt -match 'acquired=(\d+)') { [int64]$Matches[1] } else { 0 }
        $rel = if ($txt -match 'released=(\d+)') { [int64]$Matches[1] } else { 0 }
        $slot = if ($txt -match 'slot=(\d+)/') { [int]$Matches[1] } else { 0 }
        $res += [pscustomobject]@{ Acq = $acq; Rel = $rel; Slot = $slot; Exit = $p.Proc.ExitCode; WaitedMs = $acq - $t0 }
        Write-Host ("    {0}: exit={1} slot={2} acquired=+{3:n1}s held={4:n1}s" -f (Split-Path -Leaf $p.Out), $p.Proc.ExitCode, $slot, (($acq - $t0) / 1000), (($rel - $acq) / 1000))
    }
    return $res
}
function Get-MaxConcurrency($res) {
    $max = 0
    foreach ($r in $res) {
        $n = @($res | Where-Object { $_.Acq -le $r.Acq -and $_.Rel -gt $r.Acq }).Count
        if ($n -gt $max) { $max = $n }
    }
    return $max
}
function Test-NoOverlap($res) {
    $s = @($res | Sort-Object Acq)
    for ($i = 1; $i -lt $s.Count; $i++) { if ($s[$i].Acq -lt $s[$i - 1].Rel) { return $false } }
    return $true
}

try {
    Write-Host '(a) 3 probes x 15s, different dirs, DX12_BUILD_SLOTS=2, user idle'
    $a = Invoke-Probes @(1..3 | ForEach-Object { @{ Dir = (Join-Path $tmpRoot "a$_"); Sec = 15 } }) 2 'a'
    Assert-That (@($a | Where-Object { $_.Exit -ne 0 }).Count -eq 0) 'all probes exit 0'
    Assert-That ((Get-MaxConcurrency $a) -eq 2) "max concurrency is 2 (got $(Get-MaxConcurrency $a))"
    $third = $a | Sort-Object Acq | Select-Object -Last 1
    Assert-That ($third.WaitedMs -ge 12000) ('third probe waited for a slot ({0:n1}s)' -f ($third.WaitedMs / 1000))
    Assert-That ((($a | ForEach-Object Slot | Sort-Object -Unique) -join ',') -match '^1,2$') 'slots 1 and 2 were both used'

    Write-Host '(b) 2 probes x 6s, SAME dir, DX12_BUILD_SLOTS=2'
    $same = Join-Path $tmpRoot 'same'
    $b = Invoke-Probes @(1..2 | ForEach-Object { @{ Dir = $same; Sec = 6 } }) 2 'b'
    Assert-That (@($b | Where-Object { $_.Exit -ne 0 }).Count -eq 0) 'all probes exit 0'
    Assert-That (Test-NoOverlap $b) 'same-dir probes serialize'

    Write-Host '(d) 3 probes x 5s, different dirs, DX12_BUILD_SLOTS=2, user ACTIVE (idle 5s)'
    $env:DX12_BUILD_FAKE_IDLE_SEC = '5'
    $d = Invoke-Probes @(1..3 | ForEach-Object { @{ Dir = (Join-Path $tmpRoot "d$_"); Sec = 5 } }) 2 'd'
    $env:DX12_BUILD_FAKE_IDLE_SEC = '99999'
    Assert-That (@($d | Where-Object { $_.Exit -ne 0 }).Count -eq 0) 'all probes exit 0'
    Assert-That (Test-NoOverlap $d) 'user active: serialized (slot 1 only)'
    Assert-That (@($d | Where-Object { $_.Slot -ne 1 }).Count -eq 0) 'user active: only slot 1 used'

    Write-Host '(c) 3 probes x 5s, different dirs, DX12_BUILD_SLOTS=1'
    $c = Invoke-Probes @(1..3 | ForEach-Object { @{ Dir = (Join-Path $tmpRoot "c$_"); Sec = 5 } }) 1 'c'
    Assert-That (@($c | Where-Object { $_.Exit -ne 0 }).Count -eq 0) 'all probes exit 0'
    Assert-That (Test-NoOverlap $c) 'slots=1 serializes everything'
    Assert-That ((Get-MaxConcurrency $c) -eq 1) 'max concurrency is 1'
}
finally {
    Remove-Item Env:\DX12_BUILD_FAKE_IDLE_SEC, Env:\DX12_BUILD_SLOT_PREFIX, Env:\DX12_BUILD_SLOTS, Env:\DX12_BUILD_MIN_FREE_GB -ErrorAction SilentlyContinue
    try { Remove-Item -Recurse -Force -LiteralPath $tmpRoot -ErrorAction SilentlyContinue } catch {}
}
if ($fail -eq 0) { Write-Host 'ALL PASS'; exit 0 } else { Write-Host "FAILED: $fail"; exit 1 }
