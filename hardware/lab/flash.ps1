<#
  ボードに ArduinoLab のスケッチを書き込む（コンパイル + アップロード）
  使い方: flash.bat（ESP32）/ flash.bat uno（Arduino Uno）/ flash.bat nano
    -Board esp32|uno|nano|<FQBN>  … 既定 esp32
    -Port COM5                    … 省略すると自動で探す（候補が複数なら聞く）
    -Sketch <フォルダ>            … 既定 ArduinoLab\firmware\ArduinoLab。自作のスケッチも書ける（UnoLink ライブラリ付きでコンパイル）
  ★エンジン（start.bat）が起動中だとポートを掴んでいて書き込めない。先にエンジンを閉じる
#>
param(
  [Parameter(Position = 0)][string]$Board = 'esp32',
  [string]$Port,
  [string]$Sketch
)
. (Join-Path $PSScriptRoot 'common.ps1')

$fqbn = switch ($Board.ToLower()) {
  'esp32' { 'esp32:esp32:esp32' }
  'uno'   { 'arduino:avr:uno' }
  'nano'  { 'arduino:avr:nano:cpu=atmega328old' }   # 互換 Nano は旧ブートローダーが多い。だめなら -Board arduino:avr:nano
  default { $Board }
}
if (-not $Sketch) { $Sketch = Join-Path $LabDir 'ArduinoLab\firmware\ArduinoLab' }
$lib = Join-Path $RepoDir 'hardware\firmware\UnoLink'

$cli = Find-ArduinoCli
if (-not $cli) { throw 'arduino-cli がありません。先に setup.bat を実行してください' }

if (Get-Process DX12Engine -ErrorAction SilentlyContinue) {
  Write-Warn2 'エンジン (DX12Engine.exe) が起動中です。ポートを掴んでいると書き込みに失敗するので、閉じてから続けてください'
  Read-Host '閉じたら Enter'
}

if (-not $Port) {
  $ports = @(Get-BoardPorts)
  $likely = @($ports | Where-Object { $_.Chip })
  if ($likely.Count -eq 1) { $Port = $likely[0].Port }
  elseif ($ports.Count -eq 1) { $Port = $ports[0].Port }
  elseif ($ports.Count -eq 0) { throw 'ボードが見つかりません。USB ケーブル（データ通信対応のもの）とドライバーを確認してください' }
  else {
    $ports | Format-Table Port, Chip, Name -AutoSize | Out-String | Write-Host
    $Port = Read-Host 'どのポートに書き込みますか（例 COM5）'
  }
}

Write-Step "書き込み: $Sketch → $Port ($fqbn)"
& $cli compile --upload -p $Port --fqbn $fqbn --library $lib --jobs 2 $Sketch
if ($LASTEXITCODE -ne 0) {
  Write-Warn2 '失敗しました。ESP32 で "Failed to connect" / "Wrong boot mode" のときは、'
  Write-Warn2 '  "Connecting..." が出ている間、基板の BOOT ボタンを押しっぱなしにするとたいてい通ります'
  exit 1
}
Write-Host "`n書き込み完了。次は start.bat でエンジンを起動 → ▶ Play" -ForegroundColor Green
