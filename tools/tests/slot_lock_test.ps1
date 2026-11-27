<#
  tools/lib/SlotLock.psm1 + slot_hold.ps1 のテスト(偽の無操作秒数・テスト専用の Mutex 接頭辞。実 GPU もビルドも動かさない)
    pwsh tools/tests/slot_lock_test.ps1
#>
$ErrorActionPreference = 'Stop'
$hold = Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\slot_hold.ps1'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('dx12-slotlock-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$env:DX12_GPU_SLOT_PREFIX = "Global\dx12-gpu-test-$tag"
$env:DX12_TEST_SLOT_PREFIX = "Global\dx12-test-test-$tag"
$env:DX12_BUILD_MIN_FREE_GB = '0'
$env:DX12_BUILD_FAKE_IDLE_SEC = '99999'
$env:DX12_GPU_WINDOW_FILE = Join-Path $tmp 'window.lock'
Remove-Item Env:\DX12_SLOTS_DISABLE, Env:\DX12_SLOT_HELD_GPU, Env:\DX12_SLOT_HELD_TEST -ErrorAction SilentlyContinue
$fail = 0
function Assert-That([bool]$c, [string]$m) { if ($c) { Write-Host "  PASS: $m" } else { Write-Host "  FAIL: $m"; $script:fail++ } }

$n = 0
# 枠を取るプロセスを起動する。-Hold なら取れたあと ReleaseFile を作るまで持つ。戻り値: Proc, Ready, Release, Out
function Start-Holder([string]$kind, [int]$waitMin = 0, [switch]$NoWaitReady) {
    $script:n++
    $o = [pscustomobject]@{ Ready = (Join-Path $tmp "r$n.ready"); Release = (Join-Path $tmp "r$n.release"); Out = (Join-Path $tmp "r$n.out"); Proc = $null }
    $o.Proc = Start-Process pwsh -PassThru -WindowStyle Hidden -RedirectStandardOutput $o.Out -ArgumentList @(
        '-NoProfile', '-File', $hold, '-Kind', $kind, '-WaitMinutes', $waitMin, '-ReadyFile', $o.Ready, '-ReleaseFile', $o.Release, '-ParentPid', $PID)
    $t0 = Get-Date
    while (-not (Test-Path $o.Ready) -and ((Get-Date) - $t0).TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
    $o | Add-Member -NotePropertyName Line -NotePropertyValue $(if (Test-Path $o.Ready) { Get-Content $o.Ready -Raw } else { '' })
    return $o
}
function Stop-Holder($h) { Set-Content -LiteralPath $h.Release -Value 1; $null = $h.Proc.WaitForExit(15000); if (-not $h.Proc.HasExited) { $h.Proc.Kill() } }

try {
    Write-Host '(a) test slots=2, user away: 2 holders get slots 1 and 2, the third times out'
    $env:DX12_TEST_SLOTS = '2'
    $a1 = Start-Holder 'test'; $a2 = Start-Holder 'test'; $a3 = Start-Holder 'test'
    Assert-That ($a1.Line -match 'slot=1/2 priority=BelowNormal') "first gets slot 1 BelowNormal ($($a1.Line))"
    Assert-That ($a2.Line -match 'slot=2/2 priority=Idle') "second gets slot 2 Idle ($($a2.Line))"
    Assert-That ($a3.Line -eq 'timeout') 'third times out (exit 3)'
    $null = $a3.Proc.WaitForExit(10000)
    Assert-That ($a3.Proc.ExitCode -eq 3) 'timeout exit code is 3'

    Write-Host '(b) a released/killed holder frees its slot'
    Stop-Holder $a1
    $b1 = Start-Holder 'test'
    Assert-That ($b1.Line -match 'slot=1/2') "slot 1 is free again ($($b1.Line))"
    $a2.Proc.Kill(); $null = $a2.Proc.WaitForExit(5000)
    $b2 = Start-Holder 'test'
    Assert-That ($b2.Line -match 'slot=2/2') "killed holder's slot 2 is taken over ($($b2.Line))"
    Stop-Holder $b1; Stop-Holder $b2

    Write-Host '(c) user active: only slot 1'
    $env:DX12_BUILD_FAKE_IDLE_SEC = '5'
    $c1 = Start-Holder 'test'; $c2 = Start-Holder 'test'
    Assert-That ($c1.Line -match 'slot=1/2') 'first gets slot 1'
    Assert-That ($c2.Line -eq 'timeout') 'second does not get slot 2 while the user is active'
    Stop-Holder $c1
    $env:DX12_BUILD_FAKE_IDLE_SEC = '99999'

    Write-Host '(d) gpu and test are independent kinds'
    $env:DX12_GPU_SLOTS = '1'; $env:DX12_TEST_SLOTS = '1'
    $d1 = Start-Holder 'gpu'; $d2 = Start-Holder 'test'; $d3 = Start-Holder 'gpu'
    Assert-That ($d1.Line -match 'slot=1/1') 'gpu slot taken'
    Assert-That ($d2.Line -match 'slot=1/1') 'test slot taken at the same time'
    Assert-That ($d3.Line -eq 'timeout') 'second gpu holder times out'
    Stop-Holder $d1; Stop-Holder $d2

    Write-Host '(e) gpu measurement window lock (uno-gpu-window.lock) is respected'
    $sleeper = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep 60')
    (@{ pid = $sleeper.Id; owner = 'test'; startedAt = (Get-Date).ToString('o') } | ConvertTo-Json -Compress) | Set-Content $env:DX12_GPU_WINDOW_FILE
    $e1 = Start-Holder 'gpu'
    Assert-That ($e1.Line -eq 'timeout') 'gpu slot waits (times out) while a live window lock exists'
    $env:DX12_GPU_WINDOW_IGNORE = '1'
    $e2 = Start-Holder 'gpu'
    Assert-That ($e2.Line -match 'slot=1/1') 'DX12_GPU_WINDOW_IGNORE=1 skips the window lock'
    Stop-Holder $e2
    Remove-Item Env:\DX12_GPU_WINDOW_IGNORE
    $sleeper.Kill(); $null = $sleeper.WaitForExit(5000)
    $e3 = Start-Holder 'gpu'
    Assert-That ($e3.Line -match 'slot=1/1') 'a stale window lock (dead pid) is ignored'
    Stop-Holder $e3
    # test kind never looks at the window lock
    $sleeper = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep 60')
    (@{ pid = $sleeper.Id } | ConvertTo-Json -Compress) | Set-Content $env:DX12_GPU_WINDOW_FILE
    $e4 = Start-Holder 'test'
    Assert-That ($e4.Line -match 'slot=1/1') 'test kind ignores the window lock'
    Stop-Holder $e4; $sleeper.Kill()

    Write-Host '(f) DX12_SLOTS_DISABLE=1 and nested env'
    $env:DX12_SLOTS_DISABLE = '1'
    $f1 = Start-Holder 'test'; $f2 = Start-Holder 'test'
    Assert-That (($f1.Line -match 'slot=0/') -and ($f2.Line -match 'slot=0/')) 'disabled: no locking, both pass'
    Stop-Holder $f1; Stop-Holder $f2
    Remove-Item Env:\DX12_SLOTS_DISABLE
    $busy = Start-Holder 'test'
    $env:DX12_SLOT_HELD_TEST = '1'
    $f3 = Start-Holder 'test'
    Assert-That ($f3.Line -match 'slot=1/') 'nested (parent holds): passes without taking the slot'
    Remove-Item Env:\DX12_SLOT_HELD_TEST
    Stop-Holder $f3; Stop-Holder $busy

    Write-Host '(g) with_slot.ps1 runs the command and returns its exit code'
    $ws = Join-Path (Split-Path -Parent $PSScriptRoot) 'with_slot.ps1'
    & pwsh -NoProfile -File $ws -Kind test pwsh -NoProfile -Command 'exit 7' | Out-Null
    Assert-That ($LASTEXITCODE -eq 7) 'exit code passes through'

    Write-Host '(h) engine_instance.ps1 holds the gpu slot while the engine lives and releases it when it exits'
    $env:LOCALAPPDATA = Join-Path $tmp 'localappdata'   # インスタンスの置き場を隔離
    $fake = Join-Path $tmp 'fakebuild'; New-Item -ItemType Directory -Force $fake, (Join-Path $fake 'shaders') | Out-Null
    Copy-Item (Join-Path $env:SystemRoot 'System32\ping.exe') (Join-Path $fake 'DX12Engine.exe')   # すぐ終了する偽エンジン
    $inst = Join-Path (Split-Path -Parent $PSScriptRoot) 'engine_instance.ps1'
    $env:DX12_GPU_SLOTS = '1'
    $json = & pwsh -NoProfile -File $inst -Name slottest -Port 8889 -BuildDir $fake -SlotWaitMin 1 | Select-Object -Last 1 | ConvertFrom-Json
    Assert-That ($json.slot -eq 'gpu:1') "engine_instance reports the gpu slot ($($json.slot))"
    Start-Sleep -Seconds 4   # 偽エンジンが終了 -> holder が検知して解放
    $h1 = Start-Holder 'gpu'
    Assert-That ($h1.Line -match 'slot=1/1') 'slot is released after the engine exits'
    Stop-Holder $h1
    $env:DX12_SLOT_HELD_GPU = '1'
    $json2 = & pwsh -NoProfile -File $inst -Name slottest2 -Port 8888 -BuildDir $fake | Select-Object -Last 1 | ConvertFrom-Json
    Assert-That ($json2.slot -eq 'nested:1') 'nested parent slot: engine_instance does not take another'
    Remove-Item Env:\DX12_SLOT_HELD_GPU
    & pwsh -NoProfile -File $inst -Name slottest -Stop | Out-Null; & pwsh -NoProfile -File $inst -Name slottest2 -Stop | Out-Null
}
finally {
    Get-Process pwsh -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $PID -and $_.MainWindowHandle -eq 0 -and $false } | Out-Null
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($fail -gt 0) { Write-Host "FAILED: $fail"; exit 1 }
Write-Host 'ALL PASSED'
