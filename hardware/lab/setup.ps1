<#
  ArduinoLab のセットアップ（友達の PC で 1 回だけ）
    1. エンジン（Uno Engine）を用意する … 自分でビルド済みならそれを使う。無ければ GitHub のリリースを engine\ に落とす
    2. arduino-cli を用意する（無ければ tools\ に落とす。管理者権限は要らない）
    3. ESP32 / Arduino のボード定義を入れる
    4. つながっているボードを表示する
  使い方: setup.bat をダブルクリック（または powershell -ExecutionPolicy Bypass -File setup.ps1）
    -EngineVersion v2.4.0 … 落とすエンジンの版（既定 v2.4.0 = Arduino 連携が入っている版）
    -SkipEngine           … エンジンは用意しない（自分でビルドする人向け）
    -NoUno                … Arduino Uno / Nano のボード定義を入れない（ESP32 だけ）
#>
param(
  [string]$EngineVersion = 'v2.4.0',
  [switch]$SkipEngine,
  [switch]$NoUno
)
. (Join-Path $PSScriptRoot 'common.ps1')
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest が極端に遅くなるのを防ぐ
New-Item -ItemType Directory -Force $ToolsDir | Out-Null

# ---- 1. エンジン ----
Write-Step '1/4 エンジン (Uno Engine)'
$engine = Find-Engine
if ($engine) {
  Write-Ok "見つかりました: $engine"
} elseif ($SkipEngine) {
  Write-Warn2 'エンジンが見つかりません（-SkipEngine なので用意しません）。README の「ソースからビルド」を参照'
} else {
  $url = "https://github.com/ryuto-alt/dx12/releases/download/$EngineVersion/dx12-engine-$EngineVersion.zip"
  $zip = Join-Path $ToolsDir 'engine.zip'
  Write-Host "  ダウンロード中（約 50MB）: $url"
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
  if (Test-Path $EngineDir) { Remove-Item -Recurse -Force $EngineDir }
  Expand-Archive -Path $zip -DestinationPath $EngineDir -Force
  Remove-Item $zip
  $engine = Join-Path $EngineDir 'DX12Engine.exe'
  if (-not (Test-Path $engine)) { throw "展開したのに DX12Engine.exe がありません: $EngineDir" }
  Write-Ok "用意しました: $engine"
}
# Visual C++ ランタイム（エンジンが使う。たいていの PC には入っている）
if (-not (Test-Path "$env:WINDIR\System32\vcruntime140_1.dll")) {
  Write-Warn2 'Visual C++ ランタイムが入っていません。入れます（winget）'
  try { winget install --id Microsoft.VCRedist.2015+.x64 -e --accept-source-agreements --accept-package-agreements }
  catch { Write-Warn2 '自動で入れられませんでした。https://aka.ms/vs/17/release/vc_redist.x64.exe を入れてください' }
}

# ---- 2. arduino-cli ----
Write-Step '2/4 arduino-cli'
$cli = Find-ArduinoCli
if ($cli) {
  Write-Ok "見つかりました: $cli"
} else {
  $url = 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip'
  $zip = Join-Path $ToolsDir 'arduino-cli.zip'
  Write-Host "  ダウンロード中: $url"
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
  Expand-Archive -Path $zip -DestinationPath (Join-Path $ToolsDir 'arduino-cli') -Force
  Remove-Item $zip
  $cli = Find-ArduinoCli
  if (-not $cli) { throw 'arduino-cli を展開できませんでした' }
  Write-Ok "用意しました: $cli"
}
& $cli version

# ---- 3. ボード定義 ----
Write-Step '3/4 ボード定義（初回は数分かかります）'
& $cli core update-index
if ($LASTEXITCODE -ne 0) { throw 'core update-index に失敗しました（ネットにつながっているか確認）' }
& $cli core install esp32:esp32
if ($LASTEXITCODE -ne 0) { throw 'esp32:esp32 を入れられませんでした' }
Write-Ok 'ESP32 (esp32:esp32)'
if (-not $NoUno) {
  & $cli core install arduino:avr
  if ($LASTEXITCODE -ne 0) { throw 'arduino:avr を入れられませんでした' }
  Write-Ok 'Arduino Uno / Nano (arduino:avr)'
}

# ---- 4. ボード ----
Write-Step '4/4 つながっている USB シリアル'
$ports = @(Get-BoardPorts)
if ($ports.Count -eq 0) {
  Write-Warn2 'ボードが見つかりません。USB ケーブルを挿してください（充電専用ケーブルだと通信できません）'
  Write-Warn2 'それでも出ないときは USB ドライバーを入れる:'
  Write-Warn2 '  CH340  → https://www.wch-ic.com/downloads/CH341SER_EXE.html'
  Write-Warn2 '  CP210x → https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers'
} else {
  $ports | Format-Table Port, Chip, Name -AutoSize | Out-String | Write-Host
}

Write-Host "`nセットアップ完了。次は flash.bat でボードに書き込み → start.bat でエンジンを起動" -ForegroundColor Green
