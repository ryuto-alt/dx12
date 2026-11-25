<#
  ドッグフード用のエンジンを背景起動・停止する（W05-38）。MCP 8856 / UI 8915 専用（レーンのポートとは別）。
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action start -Project <dir> [-Dpi 1.5] [-EnvVar DX12_D3D_DEBUG=1] [-BuildDir <dir>]
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action restart ...   # 停止して起動し直す（DPI や環境変数を変えるとき）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action kill          # taskkill 相当（D2 の経路）
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action stop
    pwsh tools/dogfood/run.ps1 -Wave w05 -Action status
  - 起動は tools/engine_instance.ps1 -Name dogfood-<wave> -Port 8856 -Mode background 経由（--background。窓は出ない・実マウスもフォーカスも動かさない）。
  - 環境に ANTHROPIC_API_KEY があれば（プロセス・ユーザー・マシンのどれでも）起動を拒む（課金の経路を持ち込まない）。
  - UI ポート 8915 は予約値。エンジンは MCP の dx12_imgui_* で仮想入力を受けるので、UI のために別のポートは開かない。
  - 結果の置き場は %USERPROFILE%\dx12-scorecard\dogfood\<wave>\（collect.mjs が作る）。
  - -DryRun: 検査と起動コマンドの表示だけ（エンジンは起動しない）。
#>
param(
    [Parameter(Mandatory)][ValidateSet('w05', 'w1')][string]$Wave,
    [ValidateSet('start', 'restart', 'stop', 'kill', 'status')][string]$Action = 'start',
    [string]$Project = '',
    [double]$Dpi = 0,
    [string[]]$EnvVar = @(),
    [string[]]$ExtraArgs = @(),
    [string]$BuildDir = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$McpPort = 8856
$UiPort = 8915

# --- 課金の経路を持ち込まない
foreach ($scope in 'Process', 'User', 'Machine') {
    if ([Environment]::GetEnvironmentVariable('ANTHROPIC_API_KEY', $scope)) {
        Write-Host "[dogfood] ANTHROPIC_API_KEY が $scope に設定されている。API 課金を避けるため起動を拒否する。"
        exit 2
    }
}

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$inst = Join-Path $root 'tools\engine_instance.ps1'
$name = "dogfood-$Wave"

if ($Action -eq 'status') { & pwsh -NoProfile -File $inst -List; exit $LASTEXITCODE }
if ($Action -in 'stop', 'kill') {
    # kill も stop も taskkill /T /F（エンジンに MCP の終了メソッドは無い）
    if ($DryRun) { Write-Host "[dogfood] DryRun: engine_instance -Name $name -Stop"; exit 0 }
    & pwsh -NoProfile -File $inst -Name $name -Stop
    exit $LASTEXITCODE
}

$extra = @($ExtraArgs)
if ($Dpi -gt 0) { $extra += @('--dpi-scale', ([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $Dpi))) }
$envSet = @{}
foreach ($e in $EnvVar) {
    if ($e -notmatch '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Write-Host "[dogfood] -EnvVar は NAME=VALUE の形: $e"; exit 2 }
    if ($Matches[1] -like 'ANTHROPIC*') { Write-Host "[dogfood] ANTHROPIC で始まる環境変数は渡さない"; exit 2 }
    $envSet[$Matches[1]] = $Matches[2]
}

$p = @{ Name = $name; Port = $McpPort; Mode = 'background' }
if ($Project) { $p.Project = $Project }
if ($BuildDir) { $p.BuildDir = $BuildDir }
if ($Action -eq 'restart') { $p.Refresh = $true }
if ($extra.Count -gt 0) { $p.ExtraArgs = $extra }

if ($DryRun) {
    Write-Host "[dogfood] DryRun: engine_instance $(($p.GetEnumerator() | Sort-Object Name | ForEach-Object { '-' + $_.Name + '=' + ($_.Value -join ' ') }) -join ' ')  (MCP $McpPort / UI $UiPort, env: $($envSet.Keys -join ','))"
    exit 0
}
foreach ($k in $envSet.Keys) { Set-Item -Path "Env:$k" -Value $envSet[$k] }
& $inst @p
exit $LASTEXITCODE
