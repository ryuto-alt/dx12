<#
  枠(SlotLock.psm1)を取って、条件が揃うまで持ち続けるだけのプロセス。
  Node(slotLock.mjs)と、寿命の長い処理(engine_instance.ps1 のエンジン)が枠を持つために使う。
  Mutex は取ったスレッド/プロセスが持ち主なので、「持つ専用プロセス」を別に立てる。
  このプロセスが死ぬ(taskkill を含む)と OS が Mutex を放棄扱いにし、次の待ち手が引き継ぐ。

    pwsh -NoProfile -File tools/lib/slot_hold.ps1 -Kind gpu -Label golden -ParentPid 1234
  標準出力: 取れたら 1 行 "ACQUIRED slot=N/M priority=P waited=S"。取れなければ "TIMEOUT" で終了コード 3。
  終了する条件(どれか): -ParentPid が死んだ(-WatchPidFile があればエンジンの pid が現れるまで。待っている間に死んでも取らずに終わる) / -WatchPid が死んだ / -WatchPidFile の pid が死んだ(初出から) /
                         -ReleaseFile が出来た / 外から kill された
#>
param(
    [Parameter(Mandatory)][ValidateSet('gpu', 'test')][string]$Kind,
    [string]$Label = '',
    [int]$WaitMinutes = -1,   # -1 = 既定(8 分)。0 = 1 回だけ
    [int]$ParentPid = 0,
    [int]$WatchPid = 0,
    [string]$WatchPidFile = '',
    [int]$WatchGraceSec = 180,
    [string]$ReleaseFile = '',
    [string]$ReadyFile = ''
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SlotLock.psm1') -Force

# 待っている間に -ParentPid が死んだら(呼んだ側が kill された)、孤児のまま列に並び続けず、取らずに終わる
$h = Enter-DxSlot -Kind $Kind -Label $Label -WaitMinutes $WaitMinutes -AbortIfPidDead $ParentPid
if (-not $h) {
    Write-Output 'TIMEOUT'
    if ($ReadyFile) { Set-Content -LiteralPath $ReadyFile -Value 'timeout' -NoNewline }
    exit 3
}
$line = "ACQUIRED slot=$($h.Slot)/$($h.Count) priority=$($h.Priority) waited=$($h.WaitedSec)"
if ($ReadyFile) { Set-Content -LiteralPath $ReadyFile -Value $line -NoNewline }
Write-Output $line
[Console]::Out.Flush()

function Test-PidAlive([int]$p) { return [bool](Get-Process -Id $p -ErrorAction SilentlyContinue) }
$t0 = [DateTime]::UtcNow
$seenFilePid = 0
try {
    while ($true) {
        Start-Sleep -Milliseconds 700
        if ($WatchPid -gt 0 -and -not (Test-PidAlive $WatchPid)) { break }
        if ($ReleaseFile -and (Test-Path -LiteralPath $ReleaseFile)) { break }
        if ($WatchPidFile) {
            $p = 0
            if (Test-Path -LiteralPath $WatchPidFile) { try { $p = [int](Get-Content -LiteralPath $WatchPidFile -ErrorAction Stop | Select-Object -First 1) } catch { $p = 0 } }
            if ($p -gt 0) {
                if (Test-PidAlive $p) { $seenFilePid = $p } elseif ($seenFilePid -eq $p -or $seenFilePid -eq 0) { break }
            } elseif (([DateTime]::UtcNow - $t0).TotalSeconds -gt $WatchGraceSec) { break }   # 起動に失敗してエンジンが現れない
        }
        # (pid ファイルを先に読んでから親を見る: 親が起動直後に終わっても、エンジンの pid を見逃さない)
        # -WatchPidFile があるときは、エンジンの pid が現れるまでだけ -ParentPid(呼んだ engine_instance)を見る。エンジンが立てば engine_instance は終わってよい
        if ($ParentPid -gt 0 -and -not (Test-PidAlive $ParentPid) -and (-not $WatchPidFile -or $seenFilePid -eq 0)) { break }
    }
} finally { Exit-DxSlot $h }
exit 0
