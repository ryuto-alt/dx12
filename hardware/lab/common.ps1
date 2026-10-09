# ArduinoLab のスクリプトが共通で使う関数（setup.ps1 / flash.ps1 / start.ps1 から読む）
$ErrorActionPreference = 'Stop'
$LabDir    = $PSScriptRoot
$RepoDir   = (Resolve-Path (Join-Path $LabDir '..\..')).Path
$ToolsDir  = Join-Path $LabDir 'tools'     # ダウンロードした道具の置き場（git には入れない）
$EngineDir = Join-Path $LabDir 'engine'    # ダウンロードしたエンジンの置き場（git には入れない）

function Write-Step($msg)  { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Write-Ok($msg)    { Write-Host "  OK  $msg" -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host "  !!  $msg" -ForegroundColor Yellow }

# arduino-cli.exe を探す: tools\ → PATH → 既定のインストール先
function Find-ArduinoCli {
  $c = Join-Path $ToolsDir 'arduino-cli\arduino-cli.exe'
  if (Test-Path $c) { return $c }
  $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $c = 'C:\Program Files\Arduino CLI\arduino-cli.exe'
  if (Test-Path $c) { return $c }
  return $null
}

# DX12Engine.exe を探す: 自分でビルドしたもの → setup.ps1 がダウンロードしたもの → インストール版
function Find-Engine {
  $cands = @(
    (Join-Path $RepoDir 'build\release\DX12Engine.exe'),
    (Join-Path $EngineDir 'DX12Engine.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Uno Engine\DX12Engine.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\UnoEngine\DX12Engine.exe')
  )
  foreach ($c in $cands) { if (Test-Path $c) { return $c } }
  return $null
}

# USB シリアルのポートを列挙する（ESP32 / Arduino でよく使う USB 変換チップの VID で見分ける）
function Get-BoardPorts {
  $known = @{
    '1A86' = 'CH340 (ESP32 / 互換 Arduino)'
    '10C4' = 'CP210x (ESP32)'
    '0403' = 'FTDI'
    '2341' = 'Arduino 純正'
    '2A03' = 'Arduino 純正'
    '303A' = 'ESP32-S2/S3/C3 (USB 直結)'
  }
  $list = @()
  Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '\((COM\d+)\)' } | ForEach-Object {
      $port = [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value
      $vid = ''
      $m = [regex]::Match([string]$_.PNPDeviceID, 'VID_([0-9A-Fa-f]{4})')
      if ($m.Success) { $vid = $m.Groups[1].Value.ToUpper() }
      $list += [pscustomobject]@{
        Port = $port; Name = $_.Name; Vid = $vid
        Chip = $(if ($known.ContainsKey($vid)) { $known[$vid] } else { '' })
      }
    }
  return $list | Sort-Object { [int]($_.Port -replace 'COM', '') }
}
