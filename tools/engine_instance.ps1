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
  - 出力は 1 行の JSON({name,pid,port,dir})。
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
    [switch]$List
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
robocopy (Join-Path $root 'assets') (Join-Path $dir 'assets') /MIR /XF 'splash_d.*' /NFL /NDL /NJH /NJS /NP | Out-Null
# HLSL ソース（配布版の shaders-src/ と同じ置き場）。マテリアルグラフ（GraphMaterialSystem）が ForwardGraph.hlsl などを実行時に DXC でコンパイルする。
# exe 隣に assets/ があると「配布レイアウト」とみなされ、ソースの場所は exe 隣の shaders-src/ になる。
robocopy (Join-Path $root 'shaders') (Join-Path $dir 'shaders-src') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null

$env:DX12E_DATA_DIR = Join-Path $dir 'data'
$args2 = @("--$Mode", '--mcp-port', "$Port", '--idle-exit', "$IdleExitMin")
if ($Project) { $args2 += @('--project', $Project) }
$args2 += $ExtraArgs
$proc = Start-Process -FilePath (Join-Path $dir 'DX12Engine.exe') -ArgumentList $args2 -WorkingDirectory $dir -WindowStyle Hidden -PassThru
Set-Content -Path (Join-Path $dir 'engine.pid') -Value $proc.Id
try { $proc.PriorityClass = 'BelowNormal' } catch {}
Write-Output (@{ name = $Name; pid = $proc.Id; port = $Port; dir = $dir; mode = $Mode } | ConvertTo-Json -Compress)
