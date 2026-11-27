<#
  アイテムキット: エージェントが「ビルド・テスト・ゴールデン・エンジン・撮影」を少ないターンで回すための 1 本のコマンド。
  出力は常に 40 行以内の要約。全文は build\kit-logs\*.log に書き、そのパスを最後に返す。終了コードは中身のコマンドのもの
  (0 = 成功、3 = 枠待ちのタイムアウト、それ以外 = 失敗)。前景で実行し、終わるまで待つこと(Monitor / sleep のポーリングは不要)。
  詳細は docs/AGENT_KIT.md。

    pwsh tools/kit.ps1 status                          # ブランチ・変更数・離席か・枠の空き・エンジン・ビルドの古さ
    pwsh tools/kit.ps1 build [-Tests] [-Target a,b] [-Dir build\release] [-Dlss off]   # tools/build.ps1(枠・BelowNormal)
    pwsh tools/kit.ps1 test-changed [-Base ref] [-Gpu] [-Dir ...]   # 変更に関係する ctest だけ(ctest.ps1 -Changed)
    pwsh tools/kit.ps1 test [-R regex] [-Dir ...]      # CPU の ctest 全件
    pwsh tools/kit.ps1 ui-auto [-Base ref]             # 変更に関係する UI テストだけ(run_ui_tests.ps1 -Auto。GPU の枠)
    pwsh tools/kit.ps1 golden-subset a,b,c [-UiOnly]   # node tools/bench/golden.mjs --only a,b,c(GPU の枠)
    pwsh tools/kit.ps1 engine-start [-Name n] [-Project dir] [-Port N] [-Mode background|headless]   # 空きポート 8881-8889 を選ぶ
    pwsh tools/kit.ps1 engine-stop [-Name n]           # 止める(GPU の枠も戻る。使い終わったら必ず)
    pwsh tools/kit.ps1 screenshot -Port N [-Out file] [-Scene]    # 起動中のエンジンから 1 枚(窓もマウスも使わない)
    pwsh tools/kit.ps1 verify <項目ID> [-Base ref]      # build + test-changed を順に回し、build\kit\evidence\<ID>.txt に結果を書く

  ポート: 既定 8881-8889 だけを使う(KIT_PORT_MIN / KIT_PORT_MAX で変更。8851-8860 は別のワークフロー用で避ける)。
  DX12_SLOTS_DISABLE=1 で枠なし(従来の挙動)。
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)][string]$Cmd = 'status',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib\ChangedMap.psm1') -Force

# --- 引数(-Name value / --name value / -Flag / 位置引数)
$opts = @{}; $pos = @()
for ($i = 0; $i -lt $Rest.Count; $i++) {
    $t = $Rest[$i]
    if ($t -match '^--?([A-Za-z][\w-]*)$') {
        $k = $Matches[1].ToLowerInvariant()
        if (($i + 1) -lt $Rest.Count -and $Rest[$i + 1] -notmatch '^--?[A-Za-z]') { $opts[$k] = $Rest[$i + 1]; $i++ } else { $opts[$k] = $true }
    } else { $pos += $t }
}
function Opt([string]$n, $d = $null) { $k = $n.ToLowerInvariant(); if ($opts.ContainsKey($k)) { return $opts[$k] } else { return $d } }

$logDir = if ($env:KIT_LOG_DIR) { $env:KIT_LOG_DIR } else { Join-Path $root 'build\kit-logs' }
New-Item -ItemType Directory -Force $logDir | Out-Null
$portMin = if ($env:KIT_PORT_MIN -match '^\d+$') { [int]$env:KIT_PORT_MIN } else { 8881 }
$portMax = if ($env:KIT_PORT_MAX -match '^\d+$') { [int]$env:KIT_PORT_MAX } else { 8889 }
$leaf = Split-Path -Leaf $root

# 最大 $Max 行に収めて出す: 見出し(先頭の n 行)、重要行、末尾
function Show-Lines([string[]]$Lines, [string[]]$Keep, [int]$Max = 38) {
    $sel = New-Object System.Collections.Generic.List[string]
    foreach ($l in $Lines) { foreach ($rx in $Keep) { if ($l -match $rx) { $sel.Add($l); break } } }
    $out = @($sel | Select-Object -Unique | Select-Object -First ($Max - 6))
    if ($out.Count -lt 6) { $out += @($Lines | Select-Object -Last (6 - $out.Count)) }
    $out = @($out | ForEach-Object { if ($_.Length -gt 220) { $_.Substring(0, 220) + '…' } else { $_ } })
    $out | Select-Object -First $Max | ForEach-Object { Write-Host $_ }
    $skipped = $Lines.Count - $out.Count
    if ($skipped -gt 0) { Write-Host "(… 他 $skipped 行は省略。全文は下のログ)" }
}

# 子スクリプトを実行して全文をログへ。戻り値 { Code; Lines; Log }
function Invoke-Logged([string]$Label, [string]$Script, [hashtable]$P = @{}) {
    $log = Join-Path $logDir ("{0}-{1}.log" -f $Label, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $global:LASTEXITCODE = 0
    $lines = @(& $Script @P *>&1 | ForEach-Object { "$_" })   # 同じプロセスで呼ぶ(配列引数を壊さない。exit は呼んだスクリプトだけを終える)
    $code = $LASTEXITCODE
    $lines | Set-Content -LiteralPath $log -Encoding utf8
    return [pscustomobject]@{ Code = $code; Lines = $lines; Log = $log; Sec = $sw.Elapsed.TotalSeconds }
}
function Finish($r, [string]$what) {
    Write-Host ("[kit] {0}: {1} exit={2} {3:n0}s log={4}" -f $what, $(if ($r.Code -eq 0) { 'OK' } else { 'FAILED' }), $r.Code, $r.Sec, $r.Log)
    if ($r.Code -eq 3) { Write-Host '[kit] retry: 枠(GPU / テスト / ビルド)が空かず時間切れ。他の作業が終わるのを待ってから、同じコマンドをもう一度実行する(失敗ではない)' }
    exit $r.Code
}

function Test-PortOpen([int]$p) {
    try { $c = [Net.Sockets.TcpClient]::new(); $a = $c.BeginConnect('127.0.0.1', $p, $null, $null); $ok = $a.AsyncWaitHandle.WaitOne(200); if ($ok -and $c.Connected) { $c.Close(); return $true }; $c.Close(); return $false } catch { return $false }
}
# --- ポートの予約: ポートが空いて見えても、別のエージェントの engine-start が GPU の枠を待っている最中(エンジンはまだ立っていない)かもしれない。
# 予約ファイルを CreateNew で排他に作る。持ち主が生きている間(kit のプロセス、またはその名前のエンジン)は有効。
$portDir = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-ports'
$instBase = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-instances'
function Test-ReservationLive([string]$f) {
    try {
        $j = Get-Content -LiteralPath $f -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($j.kitPid -and (Get-Process -Id ([int]$j.kitPid) -ErrorAction SilentlyContinue)) { return $true }
        $pf = Join-Path $instBase "$($j.name)\engine.pid"
        if (Test-Path $pf) { $ep = [int](Get-Content $pf -ErrorAction SilentlyContinue | Select-Object -First 1); if ($ep -gt 0 -and (Get-Process -Id $ep -ErrorAction SilentlyContinue)) { return $true } }
        return $false
    } catch { return ((Get-Item -LiteralPath $f -ErrorAction SilentlyContinue).LastWriteTime -gt (Get-Date).AddSeconds(-5)) }   # 書き込み途中かもしれない
}
function Reserve-Port([int]$p, [string]$name) {
    New-Item -ItemType Directory -Force $portDir | Out-Null
    $f = Join-Path $portDir "$p.json"
    for ($try = 0; $try -lt 2; $try++) {
        try {
            $fs = [IO.File]::Open($f, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
            try { $b = [Text.Encoding]::UTF8.GetBytes((@{ name = $name; kitPid = $PID; port = $p; at = (Get-Date).ToString('o') } | ConvertTo-Json -Compress)); $fs.Write($b, 0, $b.Length) } finally { $fs.Dispose() }
            return $true
        } catch [IO.IOException] {
            if (Test-ReservationLive $f) { return $false }
            Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
        }
    }
    return $false
}
function Release-Ports([string]$name) {
    if (-not (Test-Path $portDir)) { return }
    foreach ($f in Get-ChildItem $portDir -Filter *.json -ErrorAction SilentlyContinue) {
        try { $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json; if ($j.name -eq $name) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue } } catch {}
    }
}
function Get-FreePort([string]$name) {
    for ($p = $portMin; $p -le $portMax; $p++) { if (-not (Test-PortOpen $p) -and (Reserve-Port $p $name)) { return $p } }
    throw "空きポートが $portMin..$portMax にありません(他のエンジンを engine-stop するか KIT_PORT_MIN/MAX を調整)"
}
function Get-ListenerPid([int]$p) {
    try { $c = @(Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction Stop | Select-Object -First 1); if ($c.Count -gt 0) { return [int]$c[0].OwningProcess } } catch {}
    return 0
}
function Get-SlotStatus([string]$Kind) {
    $cfg = Get-DxSlotConfig $Kind
    $busy = @()
    for ($k = 1; $k -le $cfg.Count; $k++) {
        $m = [System.Threading.Mutex]::new($false, (Get-DxSlotName $cfg.Prefix $k))
        try { if ($m.WaitOne(0)) { $m.ReleaseMutex() } else { $busy += $k } }
        catch [System.Threading.AbandonedMutexException] { $m.ReleaseMutex() }
        finally { $m.Dispose() }
    }
    return ("{0}: {1}/{2} busy{3}" -f $Kind, $busy.Count, $cfg.Count, $(if ($busy.Count) { " (slot " + ($busy -join ',') + ")" } else { '' }))
}

switch ($Cmd.ToLowerInvariant()) {
    'status' {
        $br = (& git branch --show-current 2>$null); $head = (& git rev-parse --short HEAD 2>$null)
        $dirty = @(& git status --porcelain 2>$null).Count
        $baseRef = Resolve-DxBaseRef '' $root
        $ab = if ($baseRef) { (& git rev-list --left-right --count "$baseRef...HEAD" 2>$null) -replace '\s+', '/' } else { '?' }
        Write-Host ("[kit] {0}: branch={1} @{2} dirty={3} base={4} (behind/ahead {5})" -f $leaf, $br, $head, $dirty, $baseRef, $ab)
        $idle = Get-DxSlotUserIdleSec
        Write-Host ("[kit] user idle {0:n0} min -> {1} (threshold {2:n0} min)" -f ($idle / 60), $(if (Test-DxUserAway) { 'AWAY: extra slots open' } else { 'ACTIVE: 1 slot each' }), (Get-DxIdleMinutesThreshold))
        Write-Host ("[kit] slots: {0} | {1} | build: {2}" -f (Get-SlotStatus 'gpu'), (Get-SlotStatus 'test'), 'see tools/build.ps1 (Global\dx12-build)')
        $exe = Join-Path $root 'build\release\DX12Engine.exe'
        if (Test-Path $exe) {
            $age = (Get-Date) - (Get-Item $exe).LastWriteTime
            Write-Host ("[kit] build\release\DX12Engine.exe: {0:n0} min old" -f $age.TotalMinutes)
        } else { Write-Host '[kit] build\release\DX12Engine.exe: not built (pwsh tools/kit.ps1 build)' }
        $inst = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'engine_instance.ps1') -List 2>$null | Select-Object -Last 1
        $alive = @(); try { $alive = @($inst | ConvertFrom-Json | Where-Object { $_.alive }) } catch {}
        Write-Host ("[kit] engines alive: {0}{1}" -f $alive.Count, $(if ($alive.Count) { ' (' + (($alive | ForEach-Object { "$($_.name):$($_.pid)" }) -join ', ') + ')' } else { '' }))
        $busyPorts = @($portMin..$portMax | Where-Object { Test-PortOpen $_ })
        Write-Host ("[kit] ports {0}-{1} in use: {2}" -f $portMin, $portMax, $(if ($busyPorts.Count) { $busyPorts -join ',' } else { 'none' }))
        exit 0
    }
    'build' {
        $a = @{}
        if (Opt 'tests') { $a.Tests = $true }
        if (Opt 'target') { $a.Target = @(([string](Opt 'target')) -split ',') }
        if (Opt 'dir') { $a.Dir = [string](Opt 'dir') }
        if (Opt 'dlss') { $a.Dlss = [string](Opt 'dlss') }
        if (Opt 'jobs') { $a.Jobs = [int](Opt 'jobs') }
        $a.WaitMinutes = Get-DxSlotDefaultWaitMin   # build.ps1 の既定は 40 分。前景ツールの 10 分に収める(取れなければ終了コード 3)
        $r = Invoke-Logged 'build' (Join-Path $PSScriptRoot 'build.ps1') $a
        Show-Lines $r.Lines @('^\[build\]', 'error\s+C\d+', 'error\s+LNK\d+', 'fatal error', ': error', 'FAILED:') 36
        Finish $r 'build'
    }
    'test-changed' {
        $a = @{ Changed = $true }
        if (Opt 'base') { $a.Base = [string](Opt 'base') }
        if (Opt 'gpu') { $a.Gpu = $true }
        if (Opt 'dir') { $a.Dir = [string](Opt 'dir') }
        $r = Invoke-Logged 'ctest-changed' (Join-Path $PSScriptRoot 'ctest.ps1') $a
        Show-Lines $r.Lines @('^\[ctest\]', 'NEW RED', 'known red', 'EXPIRED', 'now passing', '\*\*\*Failed', '\*\*\*Exception', 'Test\s+#\d+:.*Failed', '^\s+- ') 36
        Finish $r 'test-changed'
    }
    'test' {
        $a = @{}
        if (Opt 'r') { $a.R = [string](Opt 'r') }
        if (Opt 'dir') { $a.Dir = [string](Opt 'dir') }
        $r = Invoke-Logged 'ctest' (Join-Path $PSScriptRoot 'ctest.ps1') $a
        Show-Lines $r.Lines @('^\[ctest\]', 'NEW RED', 'known red', 'EXPIRED', 'now passing', '\*\*\*Failed', '\*\*\*Exception') 36
        Finish $r 'test'
    }
    'ui-auto' {
        $a = @{ Auto = $true }
        if (Opt 'base') { $a.Base = [string](Opt 'base') }
        $r = Invoke-Logged 'ui-auto' (Join-Path $PSScriptRoot 'run_ui_tests.ps1') $a
        Show-Lines $r.Lines @('^\[uitests\]', '^\s+FAIL', 'known red', 'STALLED', '^\s+- ', '^\[slot') 36
        Finish $r 'ui-auto'
    }
    'golden-subset' {
        $ids = if ($pos.Count -gt 0) { $pos[0] } else { [string](Opt 'only') }
        if (-not $ids) { Write-Host '[kit] golden-subset <id,id,...> が必要'; exit 2 }
        $log = Join-Path $logDir ("golden-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $ga = @('tools/bench/golden.mjs', '--only', $ids)
        if (Opt 'uionly') { $ga += '--ui-only' }
        # golden.mjs の launchInstance は既定で 8850..8899 から空きを探す(8851-8860 は別のワークフローのもの)。範囲を kit のポート範囲に絞って子に渡す
        $env:DX12_BENCH_PORT_START = "$portMin"; $env:DX12_BENCH_PORT_END = "$portMax"
        $lines = @(& node @ga 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
        $lines | Set-Content -LiteralPath $log -Encoding utf8
        Show-Lines $lines @('^(ok|FAIL|SKIP|UPD)\s', 'シーン中', '不合格', '^\s{5,}\S', '\[slot', '\[golden\]') 36
        Finish ([pscustomobject]@{ Code = $code; Log = $log; Sec = $sw.Elapsed.TotalSeconds }) 'golden-subset'
    }
    'engine-start' {
        $name = [string](Opt 'name' "kit-$leaf")
        if (Opt 'port') {
            $port = [int](Opt 'port')
            if ($port -lt $portMin -or $port -gt $portMax) { Write-Host "[kit] ポート $port は範囲外($portMin-$portMax)"; exit 2 }
            if (Test-PortOpen $port) { Write-Host "[kit] ポート $port は使用中"; exit 2 }
            if (-not (Reserve-Port $port $name)) { Write-Host "[kit] ポート $port は別のエンジンが予約中"; exit 2 }
        } else { $port = Get-FreePort $name }
        $proj = [string](Opt 'project' '')
        if (-not $proj) {   # 使い捨てプロジェクト(実プロジェクトには触れない)
            $proj = Join-Path $env:TEMP "dx12_kit_proj_$name"
            if (Test-Path $proj) { Remove-Item $proj -Recurse -Force }
            New-Item -ItemType Directory -Force (Join-Path $proj 'assets\scenes'), (Join-Path $proj 'scripts') | Out-Null
            '{ "assetsDir": "assets", "defaultScene": "scenes/test.json", "lastOpenedScene": "scenes/test.json", "name": "KitProj", "scriptsDir": "scripts", "version": "0.1.0" }' | Set-Content (Join-Path $proj 'KitProj.dx12proj') -Encoding UTF8
        }
        # 枠待ち + 起動待ちを合わせて前景ツールの 10 分に収める(枠待ち 5 分 + 起動待ち 2.5 分)。取れなければ終了コード 3(あとでもう一度)
        $slotWait = if ($env:KIT_ENGINE_SLOT_WAIT_MIN -match '^\d+$' -and [int]$env:KIT_ENGINE_SLOT_WAIT_MIN -gt 0) { [int]$env:KIT_ENGINE_SLOT_WAIT_MIN } else { 5 }
        $a = @('-Name', $name, '-Port', "$port", '-Project', $proj, '-Mode', [string](Opt 'mode' 'background'), '-SlotWaitMin', "$slotWait")
        if (Opt 'builddir') { $a += @('-BuildDir', [string](Opt 'builddir')) }
        $out = @(& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'engine_instance.ps1') @a 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Release-Ports $name
            $out | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }; Write-Host "[kit] engine-start FAILED exit=$code"
            if ($code -eq 3) { Write-Host '[kit] retry: GPU の枠が空かなかった。他の GPU 作業が終わってから同じコマンドをもう一度(失敗ではない)' }
            exit $code
        }
        $enginePid = 0
        try { $enginePid = [int](($out | Select-Object -Last 1 | ConvertFrom-Json).pid) } catch {}
        $stopOurs = { & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'engine_instance.ps1') -Name $name -Stop | Out-Null; Release-Ports $name }
        $t0 = Get-Date
        while (-not (Test-PortOpen $port) -and ((Get-Date) - $t0).TotalSeconds -lt 150) {
            if ($enginePid -gt 0 -and -not (Get-Process -Id $enginePid -ErrorAction SilentlyContinue)) { break }   # 自分のエンジンが落ちた(ポートの取り合いに負けた等)
            Start-Sleep -Milliseconds 700
        }
        if (-not (Test-PortOpen $port)) { & $stopOurs; Write-Host "[kit] engine-start: ポート $port が開かなかった(自分のエンジンが落ちたか 150 秒超過。停止した)"; exit 1 }
        # 開いたポートの持ち主が自分のエンジンか確かめる(別のエージェントのエンジンを自分のものと取り違えない)
        $lp = Get-ListenerPid $port
        if ($enginePid -gt 0 -and $lp -gt 0 -and $lp -ne $enginePid) { & $stopOurs; Write-Host "[kit] engine-start: ポート $port の持ち主は pid=$lp で自分のエンジン(pid=$enginePid)ではない。停止した。もう一度実行"; exit 1 }
        Write-Host ("[kit] engine ready: name={0} port={1} {2}" -f $name, $port, ($out | Select-Object -Last 1))
        Write-Host "[kit] 使い終わったら: pwsh tools/kit.ps1 engine-stop -Name $name  (GPU の枠もそのとき戻る)"
        exit 0
    }
    'engine-stop' {
        $name = [string](Opt 'name' "kit-$leaf")
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'engine_instance.ps1') -Name $name -Stop | Out-Null
        Release-Ports $name
        Write-Host "[kit] engine stopped: $name"
        exit 0
    }
    'screenshot' {
        if (-not (Opt 'port')) { Write-Host '[kit] screenshot -Port N が必要(engine-start の出力のポート)'; exit 2 }
        $a = @('tools/kit_screenshot.mjs', '--port', [string](Opt 'port'))
        if (Opt 'out') { $a += @('--out', [string](Opt 'out')) }
        if (Opt 'scene') { $a += '--scene' }
        $o = @(& node @a 2>&1 | ForEach-Object { "$_" })
        $o | Select-Object -Last 3 | ForEach-Object { Write-Host $_ }
        exit $LASTEXITCODE
    }
    'verify' {
        $id = if ($pos.Count -gt 0) { $pos[0] } else { '' }
        if (-not $id) { Write-Host '[kit] verify <項目ID> が必要'; exit 2 }
        $evDir = Join-Path $root 'build\kit\evidence'; New-Item -ItemType Directory -Force $evDir | Out-Null
        $ev = Join-Path $evDir "$($id -replace '[^\w.-]', '_').txt"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $buildArgs = @{ Tests = $true; WaitMinutes = (Get-DxSlotDefaultWaitMin) }; if (Opt 'dir') { $buildArgs.Dir = [string](Opt 'dir') }
        $b = Invoke-Logged 'build' (Join-Path $PSScriptRoot 'build.ps1') $buildArgs
        $t = $null
        if ($b.Code -eq 0) {
            $ta = @{ Changed = $true }; if (Opt 'base') { $ta.Base = [string](Opt 'base') }; if (Opt 'dir') { $ta.Dir = [string](Opt 'dir') }
            $t = Invoke-Logged 'ctest-changed' (Join-Path $PSScriptRoot 'ctest.ps1') $ta
        }
        $head = & git log -1 --format='%h %s' 2>$null
        $rows = @("verify $id", "head: $head", ("when: {0}" -f (Get-Date -Format 's')), '')
        $rows += "build: exit=$($b.Code) $([int]$b.Sec)s log=$($b.Log)"
        $rows += @($b.Lines | Where-Object { $_ -match '^\[build\]' } | Select-Object -Last 8)
        if ($t) { $rows += ''; $rows += "ctest -Changed: exit=$($t.Code) $([int]$t.Sec)s log=$($t.Log)"; $rows += @($t.Lines | Where-Object { $_ -match '^\[ctest\]|NEW RED|known red|^\s+- ' } | Select-Object -First 16) }
        else { $rows += ''; $rows += 'ctest -Changed: not run (build failed)' }
        $rows | Set-Content -LiteralPath $ev -Encoding utf8
        Write-Host ("[kit] verify {0}: build={1} ctest={2} evidence={3}" -f $id, $b.Code, $(if ($t) { $t.Code } else { 'skipped' }), $ev)
        if ($b.Code -ne 0) { Show-Lines $b.Lines @('error\s+C\d+', 'error\s+LNK\d+', 'fatal error', ': error') 20 }
        elseif ($t -and $t.Code -ne 0) { Show-Lines $t.Lines @('NEW RED', 'EXPIRED', '\*\*\*Failed') 20 }
        exit $(if ($b.Code -ne 0) { $b.Code } elseif ($t) { $t.Code } else { 0 })
    }
    default {
        Write-Host '[kit] サブコマンド: status | build | test-changed | test | ui-auto | golden-subset <ids> | engine-start | engine-stop | screenshot | verify <id>'
        Write-Host '[kit] 詳細: docs/AGENT_KIT.md'
        exit 2
    }
}
