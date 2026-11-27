<#
  共有の「枠(スロット)」ロック。tools/build.ps1 のスロット設計(名前付き Mutex・スロット 1 は常に使える・
  スロット 2 以降は利用者が DX12_BUILD_IDLE_MIN 分以上操作していないときだけ・Idle 優先度)を、
  GPU とテストにも広げたもの。docs/AGENT_KIT.md の「枠」の節を参照。

    Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
    $h = Enter-DxSlot -Kind gpu -Label 'ui-tests'     # 取れるまで待つ(既定 8 分。DX12_SLOT_WAIT_MIN で変更。タイムアウトは $null)
    try { ... } finally { Exit-DxSlot $h }

  種類(Kind):
    gpu  … Global\dx12-gpu(スロット 2 以降は Global\dx12-gpu-2, -3)。GPU を使う処理(ゴールデン、UI テスト、エンジン起動、ctest の gpu/ui)
    test … Global\dx12-test(同 -2, -3)。CPU のテスト(ctest、npm test)
  枠の数: DX12_GPU_SLOTS(既定 2、最大 3)、DX12_TEST_SLOTS(既定 3、最大 3)。昼(操作中)は常に 1 本。
  環境変数:
    DX12_SLOTS_DISABLE=1          … 枠を一切使わない(従来の挙動)
    DX12_BUILD_IDLE_MIN           … 「離席」とみなす無操作の分数(既定 10。build.ps1 と共通)
    DX12_BUILD_FAKE_IDLE_SEC      … テスト用: 最後の入力からの秒数を差し替える(build.ps1 と共通)
    DX12_BUILD_MIN_FREE_GB        … スロット 2 以降に要る空き物理メモリ(既定 8。build.ps1 と共通)
    DX12_GPU_SLOT_PREFIX / DX12_TEST_SLOT_PREFIX … Mutex 名の接頭辞(テスト用。既定 Global\dx12-gpu / Global\dx12-test)
    DX12_SLOT_HELD_GPU / _TEST    … 入れ子用: 親が既に枠を持っているとき(値は枠番号)、子は取らずに通す
    DX12_SLOT_PRIORITY            … 親が取った枠の優先度(Idle / BelowNormal)。子プロセスが従う
    DX12_GPU_WINDOW_IGNORE=1      … 性能計測の窓ロックを無視する(下記)
    DX12_SLOT_WAIT_MIN            … 枠を待つ分数の既定(8)。エージェントの前景ツールは 10 分で切れるので短くしてある
    DX12_GPU_MARKER_DIR           … GPU 枠のマーカーの置き場(テスト用。既定 %TEMP%\uno-gpu-slot-holders)
    DX12_GPU_WINDOW_FILE          … 窓ロックのパス(テスト用。既定 %TEMP%\uno-gpu-window.lock)

  性能計測の窓ロック(%TEMP%\uno-gpu-window.lock。tools/mcp-server/testing/perfCore.ts と
  tools/scorecard/baseline_perf.mjs が JSON {pid, owner, startedAt} で書く)とは別物で、併用する:
    GPU の枠を取った後、窓ロックの pid が生きている間は待つ(計測中の GPU を邪魔しない)。
    計測ツールは従来どおり窓ロックだけを取る(変更なし)。
#>

function Get-DxSlotUserIdleSec {
    # 最後のキー/マウス入力からの経過秒。取れなければ 0(= 操作中扱いで安全側)
    if ($env:DX12_BUILD_FAKE_IDLE_SEC -match '^\d+(\.\d+)?$') { return [double]$env:DX12_BUILD_FAKE_IDLE_SEC }
    try {
        if (-not ('Dx12SlotIdle' -as [type])) {
            Add-Type -Name 'Dx12SlotIdle' -Namespace 'Dx12' -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
public static double IdleSec() {
    LASTINPUTINFO li = new LASTINPUTINFO(); li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
    if (!GetLastInputInfo(ref li)) return 0;
    uint now = unchecked((uint)Environment.TickCount64);
    return unchecked(now - li.dwTime) / 1000.0;
}
'@ -UsingNamespace 'System.Runtime.InteropServices'
        }
        return [Dx12.Dx12SlotIdle]::IdleSec()
    } catch { return 0 }
}

function Get-DxSlotFreePhysicalGb {
    try { return [double](Get-CimInstance Win32_OperatingSystem -Property FreePhysicalMemory).FreePhysicalMemory / 1MB } catch { return 1e9 }
}

function Get-DxIdleMinutesThreshold {
    $v = 10.0
    if ($env:DX12_BUILD_IDLE_MIN -match '^\d+(\.\d+)?$') { $v = [double]$env:DX12_BUILD_IDLE_MIN }
    return $v
}

# 利用者が離席中か(スロット 2 以降を使ってよい状態か)。ctest の -j6 判定などにも使う
function Test-DxUserAway {
    return ((Get-DxSlotUserIdleSec) -ge ((Get-DxIdleMinutesThreshold) * 60))
}

# 枠を待つ既定の分数。エージェントの前景ツールは 10 分(timeout 600000)で切れるので 8 分にする。
# 取れなければ終了コード 3(= あとでもう一度)。長く待ちたい人・夜間は DX12_SLOT_WAIT_MIN で伸ばす
function Get-DxSlotDefaultWaitMin {
    if ($env:DX12_SLOT_WAIT_MIN -match '^\d+$' -and [int]$env:DX12_SLOT_WAIT_MIN -gt 0) { return [int]$env:DX12_SLOT_WAIT_MIN }
    return 8
}

# --- GPU 枠のマーカー: 性能計測ツール(perfCore.ts / baseline_perf.mjs)は窓ロックしか取らないので、
# GPU の枠を持っているプロセスは %TEMP%\uno-gpu-slot-holders\<pid>-<slot>.json を置く。計測側は生きたマーカーがある間は待つ。
function Get-DxGpuMarkerDir {
    if ($env:DX12_GPU_MARKER_DIR) { return $env:DX12_GPU_MARKER_DIR }
    return (Join-Path ([IO.Path]::GetTempPath()) 'uno-gpu-slot-holders')
}
function Write-DxGpuMarker([int]$Slot, [string]$Label) {
    try {
        $d = Get-DxGpuMarkerDir
        New-Item -ItemType Directory -Force $d | Out-Null
        $f = Join-Path $d ("{0}-{1}.json" -f $PID, $Slot)
        (@{ pid = $PID; slot = $Slot; label = $Label; startedAt = (Get-Date).ToString('o') } | ConvertTo-Json -Compress) | Set-Content -LiteralPath $f -Encoding utf8
        return $f
    } catch { return $null }
}

function Get-DxSlotConfig([string]$Kind) {
    $k = $Kind.ToLowerInvariant()
    $up = $k.ToUpperInvariant()
    $defPrefix = "Global\dx12-$k"
    $prefix = $defPrefix
    $envPrefix = [Environment]::GetEnvironmentVariable("DX12_${up}_SLOT_PREFIX")
    if ($envPrefix) { $prefix = $envPrefix }
    $count = if ($k -eq 'test') { 3 } else { 2 }
    $envCount = [Environment]::GetEnvironmentVariable("DX12_${up}_SLOTS")
    if ($envCount -match '^\d+$') { $count = [int]$envCount }
    $count = [Math]::Max(1, [Math]::Min(3, $count))
    return [pscustomobject]@{ Kind = $k; Prefix = $prefix; Count = $count }
}

function Get-DxSlotName([string]$Prefix, [int]$Index) {
    if ($Index -le 1) { return $Prefix } else { return "$Prefix-$Index" }
}

function Enter-DxSlotMutex([System.Threading.Mutex]$m, [int]$ms) {
    try { return $m.WaitOne($ms) }
    catch [System.Threading.AbandonedMutexException] { return $true }   # 前の持ち主が異常終了。引き継ぐ
}

# 性能計測の窓ロックが生きていれば、その pid を返す(無い/壊れている/死んでいれば 0)
function Get-DxGpuWindowOwnerPid {
    if ($env:DX12_GPU_WINDOW_IGNORE -eq '1') { return 0 }
    $f = if ($env:DX12_GPU_WINDOW_FILE) { $env:DX12_GPU_WINDOW_FILE } else { Join-Path ([IO.Path]::GetTempPath()) 'uno-gpu-window.lock' }
    if (-not (Test-Path -LiteralPath $f)) { return 0 }
    try {
        $j = Get-Content -LiteralPath $f -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        $p = [int]$j.pid
        if ($p -gt 0 -and $p -ne $PID -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { return $p }
    } catch {}
    return 0
}

<#
  枠を取る。戻り値のハンドル: Kind, Slot(0 = 枠なしで通した), Count, Priority, Nested, Disabled, WaitedSec, 内部の Mutex
  -WaitMinutes を超えて取れなければ $null(呼び出し側は終了コード 3 にする)
#>
function Enter-DxSlot {
    param(
        [Parameter(Mandatory)][ValidateSet('gpu', 'test')][string]$Kind,
        [string]$Label = '',
        [int]$WaitMinutes = -1,   # -1 = 既定(Get-DxSlotDefaultWaitMin)。0 = 1 回だけ試す
        [int]$AbortIfPidDead = 0,   # この pid が死んだら待つのをやめて $null(孤児になった待ち手を残さない)
        [switch]$Quiet
    )
    $cfg = Get-DxSlotConfig $Kind
    $up = $cfg.Kind.ToUpperInvariant()
    if ($WaitMinutes -lt 0) { $WaitMinutes = Get-DxSlotDefaultWaitMin }
    $mk = {
        param($slot, $nested, $disabled, $waited, $mutexes, $prio)
        [pscustomobject]@{ Kind = $cfg.Kind; Slot = $slot; Count = $cfg.Count; Nested = $nested; Disabled = $disabled
                           WaitedSec = $waited; Priority = $prio; Label = $Label; Mutexes = $mutexes; EnvSet = $false }
    }
    if ($env:DX12_SLOTS_DISABLE -eq '1') { return (& $mk 0 $false $true 0 @() 'BelowNormal') }
    $held = [Environment]::GetEnvironmentVariable("DX12_SLOT_HELD_$up")
    if ($held -match '^\d+$') {
        $pr = if ($env:DX12_SLOT_PRIORITY) { $env:DX12_SLOT_PRIORITY } else { 'BelowNormal' }
        return (& $mk ([int]$held) $true $false 0 @() $pr)
    }

    # 順序は gpu -> test。test を持ったまま gpu を取りに行くと、gpu -> test の相手と互いに待ち合う。1 回だけ試して取れなければ断る
    $once = $false
    if ($cfg.Kind -eq 'gpu' -and ($env:DX12_SLOT_HELD_TEST -match '^\d+$')) {
        $once = $true
        if (-not $Quiet) { Write-Host '[slot:gpu] lock order: gpu must be taken before test (holding test now) - trying once, no waiting' }
    }
    $idleMin = Get-DxIdleMinutesThreshold
    $minFree = 8.0
    if ($env:DX12_BUILD_MIN_FREE_GB -match '^\d+(\.\d+)?$') { $minFree = [double]$env:DX12_BUILD_MIN_FREE_GB }
    $mutexes = @()
    for ($k = 1; $k -le $cfg.Count; $k++) { $mutexes += , [System.Threading.Mutex]::new($false, (Get-DxSlotName $cfg.Prefix $k)) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $deadline = if ($once) { [DateTime]::UtcNow } else { [DateTime]::UtcNow.AddMinutes($WaitMinutes) }
    $lastReport = [DateTime]::UtcNow
    $announced = $false
    $got = 0
    while ($got -eq 0) {
        $state = @()
        for ($k = 1; $k -le $cfg.Count; $k++) {
            if ($k -ge 2) {
                $idle = Get-DxSlotUserIdleSec
                if ($idle -lt $idleMin * 60) { $state += ("slot {0} not used: user active (idle {1:n0}s < {2:n0}s)" -f $k, $idle, ($idleMin * 60)); continue }
                $free = Get-DxSlotFreePhysicalGb
                if ($free -lt $minFree) { $state += ("slot {0} not used: low memory (free {1:n1} GB < {2:n1} GB)" -f $k, $free, $minFree); continue }
            }
            if (Enter-DxSlotMutex $mutexes[$k - 1] 0) { $got = $k; break }
            $state += "slot $k busy"
        }
        if ($got -ne 0) { break }
        if (-not $announced -and -not $Quiet) { $announced = $true; Write-Host "[slot:$($cfg.Kind)] busy. waiting for a slot (up to $WaitMinutes min)... $Label" }
        if ($AbortIfPidDead -gt 0 -and -not (Get-Process -Id $AbortIfPidDead -ErrorAction SilentlyContinue)) {
            foreach ($m in $mutexes) { try { $m.Dispose() } catch {} }
            return $null
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            foreach ($m in $mutexes) { try { $m.Dispose() } catch {} }
            if (-not $Quiet) { Write-Host "[slot:$($cfg.Kind)] lock timeout" }
            return $null
        }
        if (-not $Quiet -and ([DateTime]::UtcNow - $lastReport).TotalSeconds -ge 30) {
            Write-Host ("[slot:{0}] waiting {1:n0}s: {2}" -f $cfg.Kind, $sw.Elapsed.TotalSeconds, ($state -join ', '))
            $lastReport = [DateTime]::UtcNow
        }
        Start-Sleep -Milliseconds 500
    }
    $prio = if ($got -ge 2) { 'Idle' } else { 'BelowNormal' }
    $h = & $mk $got $false $false ([int]$sw.Elapsed.TotalSeconds) $mutexes $prio
    $h | Add-Member -NotePropertyName Owned -NotePropertyValue $got

    # GPU の枠は、性能計測の窓ロックが生きている間は待つ(枠は持ったまま)。
    # 取れたらマーカーを置き、置いたあとでもう一度窓を見る(計測側は「窓 -> マーカー」の順に見るので、どちらかが必ず相手に気づく)
    if ($cfg.Kind -eq 'gpu') {
        while ($true) {
            $owner = Get-DxGpuWindowOwnerPid
            if ($owner -eq 0) {
                $mf = Write-DxGpuMarker $got $Label
                if ((Get-DxGpuWindowOwnerPid) -eq 0) { $h | Add-Member -NotePropertyName Marker -NotePropertyValue $mf; break }
                if ($mf) { Remove-Item -LiteralPath $mf -Force -ErrorAction SilentlyContinue }
                continue
            }
            if (([DateTime]::UtcNow -ge $deadline) -or ($AbortIfPidDead -gt 0 -and -not (Get-Process -Id $AbortIfPidDead -ErrorAction SilentlyContinue))) {
                Exit-DxSlot $h
                if (-not $Quiet) { Write-Host "[slot:gpu] lock timeout (gpu measurement window held by pid $owner)" }
                return $null
            }
            if (-not $Quiet -and ([DateTime]::UtcNow - $lastReport).TotalSeconds -ge 30) {
                Write-Host ("[slot:gpu] waiting for the gpu measurement window (pid {0})" -f $owner)
                $lastReport = [DateTime]::UtcNow
            }
            Start-Sleep -Seconds 1
        }
    }
    $h.WaitedSec = [int]$sw.Elapsed.TotalSeconds
    if (-not $Quiet) { Write-Host ("[slot:{0}] slot {1}/{2} acquired after {3:n0}s priority={4} {5}" -f $cfg.Kind, $got, $cfg.Count, $sw.Elapsed.TotalSeconds, $prio, $Label) }
    # 子プロセスに「親が取った」と伝える(入れ子で二重に取らない)
    [Environment]::SetEnvironmentVariable("DX12_SLOT_HELD_$up", "$got")
    [Environment]::SetEnvironmentVariable('DX12_SLOT_PRIORITY', $prio)
    $h.EnvSet = $true
    return $h
}

function Exit-DxSlot($Handle) {
    if (-not $Handle) { return }
    if ($Handle.EnvSet) {
        [Environment]::SetEnvironmentVariable("DX12_SLOT_HELD_$($Handle.Kind.ToUpperInvariant())", $null)
        [Environment]::SetEnvironmentVariable('DX12_SLOT_PRIORITY', $null)
        $Handle.EnvSet = $false
    }
    if ($Handle.PSObject.Properties['Marker'] -and $Handle.Marker) { Remove-Item -LiteralPath $Handle.Marker -Force -ErrorAction SilentlyContinue; $Handle.Marker = $null }
    if ($Handle.Owned -and $Handle.Mutexes.Count -ge $Handle.Owned) {
        try { $Handle.Mutexes[$Handle.Owned - 1].ReleaseMutex() } catch {}
        $Handle.Owned = 0
    }
    foreach ($m in $Handle.Mutexes) { try { $m.Dispose() } catch {} }
    $Handle.Mutexes = @()
}

# このプロセスの優先度を、枠の優先度(スロット 2 以降は Idle)に合わせる。子プロセスに継承される
function Set-DxSlotPriority($Handle) {
    if (-not $Handle -or $Handle.Disabled) { return }
    try { (Get-Process -Id $PID).PriorityClass = $Handle.Priority } catch {}
}

# 枠を取ってからスクリプトブロックを実行する(必ず解放する)。取れなければ $null を返し $LASTEXITCODE=3
function Invoke-WithDxSlot {
    param([Parameter(Mandatory)][string]$Kind, [Parameter(Mandatory)][scriptblock]$Script, [string]$Label = '', [int]$WaitMinutes = -1)
    $h = Enter-DxSlot -Kind $Kind -Label $Label -WaitMinutes $WaitMinutes
    if (-not $h) { $global:LASTEXITCODE = 3; return $null }
    try { Set-DxSlotPriority $h; & $Script $h } finally { Exit-DxSlot $h }
}

Export-ModuleMember -Function Get-DxSlotDefaultWaitMin, Get-DxGpuMarkerDir, Get-DxSlotUserIdleSec, Get-DxSlotFreePhysicalGb, Get-DxIdleMinutesThreshold, Test-DxUserAway,
    Get-DxSlotConfig, Get-DxSlotName, Get-DxGpuWindowOwnerPid, Enter-DxSlot, Exit-DxSlot, Set-DxSlotPriority, Invoke-WithDxSlot
