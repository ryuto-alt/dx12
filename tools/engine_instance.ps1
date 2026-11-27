<#
  エンジン(エディタ)を「exe のコピー」から背景起動/停止するヘルパ。開発エージェントと人が使う。
  build\release\DX12Engine.exe を直接起動すると、その exe が握られて次のビルドで LNK1104 になる。
  そのため必ずインスタンス専用フォルダへコピー(可能ならハードリンク)してから起動する。

    pwsh tools/engine_instance.ps1 -Name p2 -Port 8811 -Project C:\path\to\proj    # 起動(背景・窓は出ない)
    pwsh tools/engine_instance.ps1 -Name p2 -Stop                                   # 停止
    pwsh tools/engine_instance.ps1 -Name p2 -Port 8811 -Project ... -Refresh        # 最新ビルドへ入れ替えて再起動
    pwsh tools/engine_instance.ps1 -List                                            # 自分たちのインスタンス一覧

  - 既定は --background(オフスクリーン・仮想入力・前面化しない)。画面不要なら -Mode headless。
  - データ領域(最近のプロジェクト等)は DX12E_DATA_DIR をインスタンスごとに分離し、ユーザーの %APPDATA% を汚さない。
  - --idle-exit 45: 45 分操作が無ければエンジンが自分で終了する(閉じ忘れ対策)。
  - 出力は 1 行の JSON({name,pid,port,dir,slot})。
  - GPU の枠(Global\dx12-gpu。tools/lib/SlotLock.psm1 / docs/AGENT_KIT.md)を、エンジンが生きている間ずっと持つ。
    枠を持つ専用プロセス(tools/lib/slot_hold.ps1)を先に立て、エンジンの pid ファイルを見張らせる。
    エンジンが終了する(-Stop、--idle-exit、クラッシュ)と自動で解放される。昼は枠が 1 本なので、
    使い終わったら必ず -Stop すること(放置すると --idle-exit までの最大 45 分、他の GPU 作業が待たされる)。
    親(run_ui_tests.ps1 / golden.mjs)が既に枠を持っているとき(DX12_SLOT_HELD_GPU)は取らない。
    -NoSlot または DX12_SLOTS_DISABLE=1 で従来どおり(枠なし)。スロット 2(離席中)で取れたときはエンジンも Idle 優先度。
#>
param(
    [string]$Name,
    [int]$Port = 0,
    [string]$Project = '',
    [ValidateSet('background', 'headless')][string]$Mode = 'background',
    [string[]]$ExtraArgs = @(),
    # 既定は build\release。ビルド出力のスナップショット（exe + DLL + shaders）を指すと、他人のビルドに影響されずに撮れる（A/B 検証用）。
    [string]$BuildDir = '',
    [int]$IdleExitMin = 45,
    [switch]$Stop,
    [switch]$Refresh,
    [switch]$List,
    [switch]$NoSlot,           # GPU の枠を取らない(従来の挙動)
    [int]$SlotWaitMin = 40     # 枠が空くまで待つ分数。超えたら終了コード 3
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$base = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-instances'
New-Item -ItemType Directory -Force $base | Out-Null

function Stop-Instance([string]$n) {
    $pidFile = Join-Path $base "$n\engine.pid"
    if (Test-Path $pidFile) {
        $p = [int](Get-Content $pidFile -ErrorAction SilentlyContinue)
        if ($p -gt 0) { & taskkill /PID $p /T /F 2>$null | Out-Null }
        Remove-Item $pidFile -ErrorAction SilentlyContinue
    }
    # 枠を持っている専用プロセスも止める(落とせば Mutex は放棄扱いで次の待ち手へ渡る)
    $holdFile = Join-Path $base "$n\slot.holder.pid"
    if (Test-Path $holdFile) {
        $hp = [int](Get-Content $holdFile -ErrorAction SilentlyContinue)
        if ($hp -gt 0) { & taskkill /PID $hp /T /F 2>$null | Out-Null }
        Remove-Item $holdFile -ErrorAction SilentlyContinue
    }
}

if ($List) {
    Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $pidFile = Join-Path $_.FullName 'engine.pid'
        $alive = $false; $pp = 0
        if (Test-Path $pidFile) { $pp = [int](Get-Content $pidFile); $alive = [bool](Get-Process -Id $pp -ErrorAction SilentlyContinue) }
        [pscustomobject]@{ name = $_.Name; pid = $pp; alive = $alive; dir = $_.FullName }
    } | ConvertTo-Json -Compress
    exit 0
}
if (-not $Name) { Write-Error '-Name が必要です'; exit 2 }
$dir = Join-Path $base $Name

if ($Stop) { Stop-Instance $Name; Write-Output (@{ name = $Name; stopped = $true } | ConvertTo-Json -Compress); exit 0 }
if ($Refresh) { Stop-Instance $Name }

if (Test-Path (Join-Path $dir 'engine.pid')) {
    $old = [int](Get-Content (Join-Path $dir 'engine.pid'))
    if (Get-Process -Id $old -ErrorAction SilentlyContinue) { Write-Error "既に起動中です(pid=$old)。-Refresh か -Stop を使ってください"; exit 3 }
}
if ($Port -le 0) { Write-Error '-Port が必要です'; exit 2 }

$src = if ($BuildDir) { $BuildDir } else { Join-Path $root 'build\release' }
if (-not (Test-Path (Join-Path $src 'DX12Engine.exe'))) { Write-Error "ビルド出力がありません: $src"; exit 4 }
New-Item -ItemType Directory -Force $dir, (Join-Path $dir 'data') | Out-Null

# exe コピー(DLL は同一ボリュームならハードリンクでディスク節約。失敗したらコピー)
robocopy $src $dir DX12Engine.exe GameRuntime.exe gh.exe *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $src 'shaders') (Join-Path $dir 'shaders') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
# Agility のランタイム（exe の隣の D3D12\）。無いと D3D12SDKPath の先が空になり、in-box のランタイムに縮退する（W05-03）。
if (Test-Path (Join-Path $src 'D3D12')) {
    robocopy (Join-Path $src 'D3D12') (Join-Path $dir 'D3D12') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
}
robocopy (Join-Path $root 'assets') (Join-Path $dir 'assets') /MIR /XF 'splash_d.*' /NFL /NDL /NJH /NJS /NP | Out-Null
# HLSL ソース（配布版の shaders-src/ と同じ置き場）。マテリアルグラフ（GraphMaterialSystem）が ForwardGraph.hlsl などを実行時に DXC でコンパイルする。
# exe 隣に assets/ があると「配布レイアウト」とみなされ、ソースの場所は exe 隣の shaders-src/ になる。
robocopy (Join-Path $root 'shaders') (Join-Path $dir 'shaders-src') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null

# --- GPU の枠(エンジンの寿命のあいだ持つ)
$slotInfo = 'none'; $slotPrio = 'BelowNormal'; $holdProc = $null
if (-not $NoSlot -and $env:DX12_SLOTS_DISABLE -ne '1') {
    if ($env:DX12_SLOT_HELD_GPU -match '^\d+$') { $slotInfo = "nested:$($env:DX12_SLOT_HELD_GPU)"; if ($env:DX12_SLOT_PRIORITY) { $slotPrio = $env:DX12_SLOT_PRIORITY } }
    else {
        $ready = Join-Path $dir 'slot.ready'
        Remove-Item $ready -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $dir 'engine.pid') -ErrorAction SilentlyContinue
        $holdProc = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'lib\slot_hold.ps1'),
            '-Kind', 'gpu', '-Label', "engine:$Name", '-WaitMinutes', $SlotWaitMin, '-WatchPidFile', (Join-Path $dir 'engine.pid'), '-ReadyFile', $ready)
        Set-Content -Path (Join-Path $dir 'slot.holder.pid') -Value $holdProc.Id
        $t0 = Get-Date
        while (-not (Test-Path $ready) -and -not $holdProc.HasExited -and ((Get-Date) - $t0).TotalMinutes -lt ($SlotWaitMin + 1)) { Start-Sleep -Milliseconds 300 }
        $r = if (Test-Path $ready) { (Get-Content $ready -Raw).Trim() } else { '' }
        if ($r -notmatch '^ACQUIRED slot=(\d+)/\d+ priority=(\w+)') {
            try { & taskkill /PID $holdProc.Id /T /F 2>$null | Out-Null } catch {}
            Remove-Item (Join-Path $dir 'slot.holder.pid') -ErrorAction SilentlyContinue
            Write-Error "GPU の枠が $SlotWaitMin 分以内に取れませんでした(他の GPU 作業が実行中)"; exit 3
        }
        $slotInfo = "gpu:$($Matches[1])"; $slotPrio = $Matches[2]
    }
}

$env:DX12E_DATA_DIR = Join-Path $dir 'data'
$env:DX12E_NO_SPLASH = '1'   # 起動画面(スプラッシュ)を前面へ出さない
$args2 = @("--$Mode", '--mcp-port', "$Port", '--idle-exit', "$IdleExitMin")
if ($Project) { $args2 += @('--project', $Project) }
$args2 += $ExtraArgs
$proc = Start-Process -FilePath (Join-Path $dir 'DX12Engine.exe') -ArgumentList $args2 -WorkingDirectory $dir -WindowStyle Hidden -PassThru
Set-Content -Path (Join-Path $dir 'engine.pid') -Value $proc.Id
try { $proc.PriorityClass = $slotPrio } catch {}
Write-Output (@{ name = $Name; pid = $proc.Id; port = $Port; dir = $dir; mode = $Mode; slot = $slotInfo } | ConvertTo-Json -Compress)
