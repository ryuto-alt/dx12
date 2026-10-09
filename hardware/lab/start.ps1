<#
  エンジンを起動して ArduinoLab プロジェクトを開く
    -Engine <DX12Engine.exe のパス> … 省略すると自動で探す（ビルド版 → setup がダウンロードした版 → インストール版）
#>
param([string]$Engine)
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $Engine) { $Engine = Find-Engine }
if (-not $Engine) { throw 'DX12Engine.exe が見つかりません。先に setup.bat を実行してください' }
$project = Join-Path $LabDir 'ArduinoLab'
Write-Host "起動: $Engine --project $project"
Start-Process -FilePath $Engine -ArgumentList @('--project', "`"$project`"") -WorkingDirectory (Split-Path $Engine)
