<#
  UI 自動テスト(ImGuiTestEngine)を全件、窓なし・背景・仮想入力で流す 1 コマンド。人の PC 操作を一切奪わない。

    pwsh tools/run_ui_tests.ps1                       # 全件(既定ポート 8822)
    pwsh tools/run_ui_tests.ps1 -Only open_all_tool_windows,build_game   # 指定テストだけ
    pwsh tools/run_ui_tests.ps1 -Skip build_game      # 除外
    pwsh tools/run_ui_tests.ps1 -Port 8830 -Name my   # 並走する別エージェントとポートを分ける

  やっていること:
    1. 使い捨てプロジェクトを作る(実プロジェクトには触れない。.autosave も残らない)。
    2. tools/engine_instance.ps1 で `--background`(オフスクリーン・仮想入力・前面化しない)+ `--ui-tests-run-all` を起動。
       exe はインスタンス専用コピーなので build\release の exe を握らない。
    3. 実行中 0.5 秒ごとに GetForegroundWindow を記録し、開始時と変わったら「前面が奪われた」として失敗にする。
    4. 終了後に JUnit XML を集計して 失敗テスト名と理由を出す。終了コード 0 = 全件緑・前面不変。
  配布ゲームの起動確認(build_game)も --background=offscreen,tool で動くので除外は不要。
#>
param(
    [string]$Name = 'uitests',
    [int]$Port = 8822,
    [string[]]$Only = @(),
    [string[]]$Skip = @(),
    [int]$TimeoutMin = 30,
    [int]$StallSec = 240,
    [int]$Speed = 0,
    [switch]$KeepProject
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$inst = Join-Path $root 'tools\engine_instance.ps1'
$base = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-instances'
$dir  = Join-Path $base $Name

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class FgProbe { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); }
'@
function Get-Fg { [int64]([FgProbe]::GetForegroundWindow()) }

# 使い捨てプロジェクト
$proj = Join-Path $env:TEMP "dx12_uitest_proj_$Name"
if (Test-Path $proj) { Remove-Item $proj -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $proj 'assets\scenes'), (Join-Path $proj 'scripts') | Out-Null
@'
{ "assetsDir": "assets", "defaultScene": "scenes/test.json", "lastOpenedScene": "scenes/test.json", "name": "UiTestProj", "scriptsDir": "scripts", "version": "0.1.0" }
'@ | Set-Content (Join-Path $proj 'UiTestProj.dx12proj') -Encoding UTF8

# 前回のインスタンスを止めて結果を消す
& $inst -Name $Name -Stop | Out-Null
if (Test-Path $dir) { Remove-Item (Join-Path $dir 'ui_test_report.txt'), (Join-Path $dir 'ui_test_results.xml') -ErrorAction SilentlyContinue }

$extra = @('--ui-tests-run-all', "--ui-tests-speed=$Speed")
if ($Only.Count -gt 0) { $extra += @('--ui-tests-only', ($Only -join ',')) }
if ($Skip.Count -gt 0) { $extra += @('--ui-tests-skip', ($Skip -join ',')) }

$fg0 = Get-Fg
$sw = [Diagnostics.Stopwatch]::StartNew()
$info = (& $inst -Name $Name -Port $Port -Project $proj -IdleExitMin 60 -ExtraArgs $extra) | Select-Object -Last 1 | ConvertFrom-Json
$enginePid = [int]$info.pid
Write-Host "[uitests] engine pid=$enginePid port=$Port fg0=$fg0"

$fgChanges = New-Object System.Collections.Generic.List[string]
$logFile = Join-Path $dir 'dx12_engine.log'
$lastMark = ''; $lastMarkAt = $sw.Elapsed; $stalled = $false; $lastCheck = [TimeSpan]::Zero
while ($sw.Elapsed.TotalMinutes -lt $TimeoutMin) {
    $p = Get-Process -Id $enginePid -ErrorAction SilentlyContinue
    $fg = Get-Fg
    if ($fg -ne $fg0) { $fgChanges.Add(("{0:n1}s fg={1}" -f $sw.Elapsed.TotalSeconds, $fg)); $fg0 = $fg }   # 変わるたび記録(基準も更新)
    if (-not $p) { break }
    # ハング検出: [uitest] の進捗行が StallSec 秒進まなければ、そのテストで止まっているとみなして打ち切る
    if (($sw.Elapsed - $lastCheck).TotalSeconds -ge 10 -and (Test-Path $logFile)) {
        $lastCheck = $sw.Elapsed
        try {
            $m = Get-Content $logFile -Tail 300 -ErrorAction Stop | Where-Object { $_ -match '\[uitest\]' } | Select-Object -Last 1
            if ($m -and $m -ne $lastMark) { $lastMark = $m; $lastMarkAt = $sw.Elapsed }
        } catch {}
        if (($sw.Elapsed - $lastMarkAt).TotalSeconds -gt $StallSec) { $stalled = $true; break }
    }
    Start-Sleep -Milliseconds 500
}
$timedOut = $false
$p = Get-Process -Id $enginePid -ErrorAction SilentlyContinue
if ($p) { $timedOut = -not $stalled; & $inst -Name $Name -Stop | Out-Null }
$elapsed = $sw.Elapsed

# 結果の集計(エンジンが終了時に書く ui_test_report.txt。1 テスト 1 行 + 失敗は「理由:」行)
$rep = Join-Path $dir 'ui_test_report.txt'
$fail = @(); $total = 0; $notRun = @()
if (Test-Path $rep) {
    $cur = $null
    foreach ($line in (Get-Content $rep -Encoding UTF8)) {
        if ($line -match '^\[ OK \]') { $total++ }
        elseif ($line -match '^\[失敗\] (.+)$') { $total++; $cur = [pscustomobject]@{ name = $Matches[1]; msg = '' }; $fail += $cur }
        elseif ($line -match '^\[未実行\] (.+)$') { $notRun += $Matches[1] }
        elseif ($cur -and $line -match '^\s+(理由:)?\s*(.+)$') { $cur.msg += ' ' + $Matches[2] }
    }
}
if (-not $KeepProject) { Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue }
& $inst -Name $Name -Stop | Out-Null

Write-Host ("[uitests] {0} run, {1} failed, {2} not run, {3:n0}s{4}" -f $total, $fail.Count, $notRun.Count, $elapsed.TotalSeconds, $(if ($timedOut) { ' (TIMEOUT)' } elseif ($stalled) { ' (STALLED)' } else { '' }))
if ($stalled) { Write-Host ("  STALLED after: {0}" -f $lastMark) }
foreach ($f in $fail) { Write-Host ("  FAIL {0}: {1}" -f $f.name, $f.msg.Trim().Substring(0, [Math]::Min(300, $f.msg.Trim().Length))) }
if ($fgChanges.Count -gt 0) { Write-Host "[uitests] FOREGROUND CHANGED during run (ユーザー自身の操作でも変わる):"; $fgChanges | ForEach-Object { Write-Host "  $_" } }
else { Write-Host "[uitests] foreground window unchanged" }
# 期待する件数: -Only 指定ならその数、なければ未実行が 0 であること(Skip 指定ぶんは除く)
$ok = ($total -gt 0) -and ($fail.Count -eq 0) -and (-not $timedOut) -and (-not $stalled) -and ($fgChanges.Count -eq 0)
exit $(if ($ok) { 0 } else { 1 })
