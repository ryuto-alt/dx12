<#
  ドッグフード用のエンジンを背景起動・停止する（W05-38）。MCP 8856 / UI 8915 専用（レーンのポートとは別）。
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action start -Project <dir> [-Dpi 1.5] [-EnvVar DX12_D3D_DEBUG=1] [-BuildDir <dir>] [-IdleExitMin 1] [-FreshData]
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action restart ...   # 停止して起動し直す（DPI や環境変数を変えるとき）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action idle_exit [-IdleExitMin 1]  # アイドル終了を待つ（起動時の -IdleExitMin が短いこと）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action run -ExtraArgs --splash-selftest,<dir> [-EvidenceDir <dir>]  # 一回きり（窓なし）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action kill          # taskkill 相当（D2 の経路）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action stop
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action status
  - 起動は tools/engine_instance.ps1 -Name dogfood-<wave> -Port 8856 -Mode background 経由（--background。窓は出ない・実マウスもフォーカスも動かさない）。
  - -IdleExitMin: エンジンの --idle-exit（分。小数可。後ろに置くので engine_instance の既定 45 を上書きする）。
    idle_exit 経路（FleetGuard の自己終了）は、短い値で start / restart しておき、MCP を呼ばずに idle_exit で終了を待つ。
  - -FreshData: 起動前にこのインスタンスの data フォルダ（設定・テーマなど）を消す（「設定なしの初回起動」用。プロジェクトは消さない）。
  - -NameSuffix: インスタンス名の後ろに付ける（単体テストが実際の dogfood インスタンスと衝突しないため）。
  - -EvidenceDir: -ExtraArgs 中の {evidence} をこのフォルダに置き換える。
  - 終了コード: 0 正常 / 2 引数・環境の拒否 / 3 既に起動中 / 4 ビルド出力が無い / 5 idle_exit がタイムアウト / 6 run がタイムアウトまたは失敗
    （engine_instance.ps1 は Write-Error で止まり 1 を返すので、メッセージから 3 / 4 / 2 に直して返す）。
  - 環境に ANTHROPIC_API_KEY があれば（プロセス・ユーザー・マシンのどれでも）起動を拒む（課金の経路を持ち込まない）。
  - UI ポート 8915 は予約値。エンジンは MCP の dx12_imgui_* で仮想入力を受けるので、UI のために別のポートは開かない。
  - 結果の置き場は %USERPROFILE%\dx12-scorecard\dogfood\<wave>\（collect.mjs が作る）。
  - -DryRun: 検査と起動コマンドの表示だけ（エンジンは起動しない）。
#>
param(
    [Parameter(Mandatory)][ValidateSet('w05', 'w1')][string]$Wave,
    [ValidateSet('start', 'restart', 'stop', 'kill', 'idle_exit', 'run', 'status')][string]$Action = 'start',
    [string]$Project = '',
    [double]$Dpi = 0,
    [string[]]$EnvVar = @(),
    [string[]]$ExtraArgs = @(),
    [string]$BuildDir = '',
    [double]$IdleExitMin = 0,
    [switch]$FreshData,
    [string]$EvidenceDir = '',
    [string]$NameSuffix = '',   # テスト用: インスタンス名に付ける（既定は空。dogfood-<wave>）
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$McpPort = 8856
$UiPort = 8915
$Inv = [Globalization.CultureInfo]::InvariantCulture

# --- 課金の経路を持ち込まない
foreach ($scope in 'Process', 'User', 'Machine') {
    if ([Environment]::GetEnvironmentVariable('ANTHROPIC_API_KEY', $scope)) {
        Write-Host "[dogfood] ANTHROPIC_API_KEY が $scope に設定されている。API 課金を避けるため起動を拒否する。"
        exit 2
    }
}

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$inst = Join-Path $root 'tools\engine_instance.ps1'
$name = "dogfood-$Wave$NameSuffix"
$instBase = Join-Path $env:LOCALAPPDATA 'UnoEngine\agent-instances'
$instDir = Join-Path $instBase $name
$pidFile = Join-Path $instDir 'engine.pid'

function Get-EnginePid {
    if (Test-Path $pidFile) { $p = [int](Get-Content $pidFile -ErrorAction SilentlyContinue); if ($p -gt 0 -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { return $p } }
    return 0
}

if ($Action -eq 'status') { & pwsh -NoProfile -File $inst -List; exit $LASTEXITCODE }
if ($Action -in 'stop', 'kill') {
    # kill も stop も taskkill /T /F（エンジンに MCP の終了メソッドは無い）
    if ($DryRun) { Write-Host "[dogfood] DryRun: engine_instance -Name $name -Stop"; exit 0 }
    & pwsh -NoProfile -File $inst -Name $name -Stop
    exit $LASTEXITCODE
}
if ($Action -eq 'idle_exit') {
    # FleetGuard の --idle-exit で自分で終わるのを待つ。待ち中に MCP を呼ばないこと。
    $limit = [int](($IdleExitMin * 60) + 120)
    if ($DryRun) { Write-Host "[dogfood] DryRun: idle_exit を最大 $limit 秒待つ"; exit 0 }
    $p = Get-EnginePid
    if ($p -eq 0) { Write-Host '{"exited":true,"note":"already not running"}'; exit 0 }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process -Id $p -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt $limit) { Start-Sleep -Seconds 2 }
    $alive = [bool](Get-Process -Id $p -ErrorAction SilentlyContinue)
    Write-Host ("{0}" -f (@{ exited = (-not $alive); seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) } | ConvertTo-Json -Compress))
    if ($alive) { exit 5 }
    Remove-Item $pidFile -ErrorAction SilentlyContinue
    exit 0
}

# --- 引数の組み立て（start / restart / run 共通）
# pwsh -File では配列がカンマ連結の 1 本の文字列で来るので、カンマで分ける（引数にカンマを含めない）
$extra = @($ExtraArgs | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' } | ForEach-Object { if ($EvidenceDir) { $_.Replace('{evidence}', $EvidenceDir) } else { $_ } })
if ($Dpi -gt 0) { $extra += @('--dpi-scale', ([string]::Format($Inv, '{0}', $Dpi))) }
# --idle-exit は最後に置く（main.cpp は後勝ち）
if ($IdleExitMin -gt 0 -and $Action -ne 'run') { $extra += @('--idle-exit', ([string]::Format($Inv, '{0}', $IdleExitMin))) }
$envSet = @{}
foreach ($e in $EnvVar) {
    if ($e -notmatch '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Write-Host "[dogfood] -EnvVar は NAME=VALUE の形: $e"; exit 2 }
    if ($Matches[1] -like 'ANTHROPIC*') { Write-Host "[dogfood] ANTHROPIC で始まる環境変数は渡さない"; exit 2 }
    $envSet[$Matches[1]] = $Matches[2]
}

if ($Action -eq 'run') {
    # 一回きりの窓なし実行（--splash-selftest など）。終了コードをそのまま返す。
    # build\release の exe を直接起動すると exe が掴まれて次のビルドが LNK1104 になるので、インスタンスフォルダのコピーで動かす
    $src = if ($BuildDir) { $BuildDir } else { Join-Path $root 'build\release' }
    $srcExe = Join-Path $src 'DX12Engine.exe'
    $exe = Join-Path $instDir 'DX12Engine.exe'
    if ($DryRun) { Write-Host "[dogfood] DryRun: run $exe（$srcExe のコピー） $($extra -join ' ')"; exit 0 }
    if (-not (Test-Path $srcExe)) { Write-Host "[dogfood] ビルド出力がありません: $src"; exit 4 }
    # コピーは、インスタンスのコピーが無いか、ビルド出力より古く、かつエンジンが動いていないときだけ（動いている exe は上書きできない）
    $stale = (-not (Test-Path $exe)) -or ((Get-Item $exe).LastWriteTimeUtc -lt (Get-Item $srcExe).LastWriteTimeUtc)
    if ($stale -and (Get-EnginePid) -eq 0) {
        New-Item -ItemType Directory -Force $instDir | Out-Null
        robocopy $src $instDir DX12Engine.exe GameRuntime.exe gh.exe *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
        robocopy (Join-Path $src 'shaders') (Join-Path $instDir 'shaders') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
        robocopy (Join-Path $root 'assets') (Join-Path $instDir 'assets') /MIR /XF 'splash_d.*' /NFL /NDL /NJH /NJS /NP | Out-Null
        robocopy (Join-Path $root 'shaders') (Join-Path $instDir 'shaders-src') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    }
    if (-not (Test-Path $exe)) { Write-Host "[dogfood] インスタンスのコピーを作れません: $instDir"; exit 4 }
    $dataDir = Join-Path $instDir 'run-data'
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    $env:DX12E_DATA_DIR = $dataDir
    $env:DX12E_NO_SPLASH = '1'
    foreach ($k in $envSet.Keys) { Set-Item -Path "Env:$k" -Value $envSet[$k] }
    # Start-Process は配列の要素を引用しないので、空白を含む引数（{evidence} のパスなど）は自分で引用する
    $argLine = ($extra | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $proc = if ($argLine) { Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $instDir -WindowStyle Hidden -PassThru } else { Start-Process -FilePath $exe -WorkingDirectory $instDir -WindowStyle Hidden -PassThru }
    try { $proc.PriorityClass = 'BelowNormal' } catch {}
    if (-not $proc.WaitForExit(120000)) { & taskkill /PID $proc.Id /T /F 2>$null | Out-Null; Write-Host '{"exited":false,"note":"run timeout 120s"}'; exit 6 }
    Write-Host ("{0}" -f (@{ exited = $true; exitCode = $proc.ExitCode } | ConvertTo-Json -Compress))
    exit $(if ($proc.ExitCode -eq 0) { 0 } else { 6 })
}

# --- 設定なしの初回起動（data フォルダだけ消す。このインスタンス名の data の下に限る）
if ($FreshData) {
    $dataDir = Join-Path $instDir 'data'
    $full = [IO.Path]::GetFullPath($dataDir)
    $okPath = $full.StartsWith([IO.Path]::GetFullPath($instBase) + [IO.Path]::DirectorySeparatorChar) -and (Split-Path -Leaf $full) -eq 'data' -and (Split-Path -Leaf (Split-Path -Parent $full)) -eq $name
    if (-not $okPath) { Write-Host "[dogfood] FreshData の対象が想定外: $full"; exit 2 }
    if ($DryRun) { Write-Host "[dogfood] DryRun: FreshData で $full を空にする" }
    elseif ((Get-EnginePid) -ne 0 -and $Action -ne 'restart') { Write-Host '[dogfood] 起動中は -FreshData できない（restart を使う）'; exit 3 }
    elseif (Test-Path $full) {
        if ($Action -eq 'restart') { & pwsh -NoProfile -File $inst -Name $name -Stop | Out-Null }
        # taskkill 直後はファイルがまだ掴まれていることがあるので、少し待って数回やり直す
        $removed = $false
        for ($try = 0; $try -lt 10 -and -not $removed; $try++) {
            try { Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop; $removed = $true }
            catch { Start-Sleep -Milliseconds 500 }
        }
        if (-not $removed -and (Test-Path $full)) { Write-Host "[dogfood] data を消せません（使用中）: $full"; exit 2 }
    }
}

# engine_instance.ps1 は別プロセスで呼び、終了コードとメッセージを見て 2/3/4 に直す
$cmd = @('-Name', $name, '-Port', $McpPort, '-Mode', 'background')
if ($Project) { $cmd += @('-Project', $Project) }
if ($BuildDir) { $cmd += @('-BuildDir', $BuildDir) }
if ($Action -eq 'restart') { $cmd += '-Refresh' }

if ($DryRun) {
    Write-Host "[dogfood] DryRun: engine_instance $($cmd -join ' ') -ExtraArgs $($extra -join ' ')  (MCP $McpPort / UI $UiPort, env: $($envSet.Keys -join ','))"
    exit 0
}
foreach ($k in $envSet.Keys) { Set-Item -Path "Env:$k" -Value $envSet[$k] }
# ExtraArgs は -Command で配列として渡す（-File では配列が 1 本の文字列になる）
$q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
$cmdText = "& " + (& $q $inst) + " " + (($cmd | ForEach-Object { if ($_ -like '-*') { $_ } else { & $q $_ } }) -join ' ')
if ($extra.Count -gt 0) { $cmdText += " -ExtraArgs @(" + (($extra | ForEach-Object { & $q $_ }) -join ',') + ")" }
$cmdText += '; exit $LASTEXITCODE'
$out = & pwsh -NoProfile -Command $cmdText 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
# engine_instance.ps1 は $ErrorActionPreference='Stop' 下の Write-Error で終わるので、exit 3|4 に届かず 1 になる。メッセージから終了コードを復元する
if ($code -ne 0) {
    $text = $out -join "`n"
    if ($text -match '既に起動中') { $code = 3 }
    elseif ($text -match 'ビルド出力がありません') { $code = 4 }
    elseif ($text -match '-Port が必要|-Name が必要') { $code = 2 }
}
exit $code
