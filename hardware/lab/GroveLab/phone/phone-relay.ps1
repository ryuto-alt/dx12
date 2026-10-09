# GroveLab: スマホをカートレースのコントローラーにする中継（PC 側）
#   部屋番号（4 桁）を決めて中継サーバーにつなぎ、
#     スマホの入力  → assets\kart\phones.txt（GroveKart.lua が毎フレーム読む）
#     レースの状況 ← assets\kart\state.txt（GroveKart.lua が書く）をスマホへ送る
#     参加用の QR  → assets\kart\room.txt（ゲーム画面に描く）
#   Windows PowerShell 5.1 で動く（追加のインストールはいらない）。止めるときは窓を閉じる
param(
  [string]$Server = "grove-kart.cafiyagi.workers.dev",
  [string]$Room = ""
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$utf8 = New-Object Text.UTF8Encoding $false

$proj = Split-Path -Parent $PSScriptRoot
$kart = Join-Path $proj "assets\kart"
New-Item -ItemType Directory -Force -Path $kart | Out-Null
$phonesFile = Join-Path $kart "phones.txt"
$stateFile = Join-Path $kart "state.txt"
$roomFile = Join-Path $kart "room.txt"
if (-not $Room) { $Room = "{0:D4}" -f (Get-Random -Minimum 1000 -Maximum 10000) }
$url = "https://$Server/?r=$Room"

function Write-Text($path, $text) {
  try { [IO.File]::WriteAllText($path, $text, $utf8) } catch { }   # ゲームが読んでいる最中なら次の回に書く
}

Write-Host "グローブ・グランプリ スマホ中継" -ForegroundColor Yellow
Write-Host "  部屋番号: $Room"
Write-Host "  スマホで開く: $url"
$qr = ""
try { $qr = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "https://$Server/qr?r=$Room").Content } catch { Write-Host "  QR を取れませんでした: $_" }
Write-Text $roomFile ("$Room`n$url`n" + $qr)

$seq = 0
$lastRun = ""
$ct =[Threading.CancellationToken]::None
while ($true) {
  $ws = $null
  try {
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ws.Options.KeepAliveInterval = [TimeSpan]::FromSeconds(15)
    $ws.ConnectAsync([Uri]"wss://$Server/ws?room=$Room&role=host", $ct).Wait()
    Write-Host ("[{0:HH:mm:ss}] つながりました" -f (Get-Date)) -ForegroundColor Green
    $buf = New-Object byte[] 65536
    $seg = [ArraySegment[byte]]::new($buf)
    $recv = $ws.ReceiveAsync($seg, $ct)
    $sb = New-Object Text.StringBuilder
    $lastState = ""
    $nextState = Get-Date
    $nextHeartbeat = Get-Date
    while ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
      if ($recv.IsCompleted) {
        if ($recv.IsFaulted) { throw $recv.Exception }
        $r = $recv.Result
        if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { break }
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
        if ($r.EndOfMessage) {
          $m = $sb.ToString(); [void]$sb.Clear()
          if ($m.StartsWith("#phones")) {
            $seq++
            Write-Text $phonesFile ("#seq $seq`n" + $m.Substring(8))
            $nextHeartbeat = (Get-Date).AddSeconds(1)
          }
        }
        $recv = $ws.ReceiveAsync($seg, $ct)
        continue
      }
      $now = Get-Date
      if ($now -ge $nextState) {
        $nextState = $now.AddMilliseconds(100)
        $s = ""
        try { $s = [IO.File]::ReadAllText($stateFile, $utf8) } catch { }
        if ($s -and $s -ne $lastState -and $s.StartsWith("{") -and $s.TrimEnd().EndsWith("}")) {
          $lastState = $s
          # ゲームが Play を始め直したら（run が変わったら）参加者をいったん全員切ってもらう。
          #   生きているスマホはすぐ入り直し、閉じたのに残っていた分（同じ人が何人も見える原因）は消える
          if ($s -match '"run":"([^"]*)"') {
            $run = $Matches[1]
            if ($lastRun -and $run -ne $lastRun) {
              $rb = [Text.Encoding]::UTF8.GetBytes("#reset")
              $ws.SendAsync([ArraySegment[byte]]::new($rb), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).Wait()
              Write-Host ("[{0:HH:mm:ss}] ゲームが始まり直したので、スマホの参加をリセットしました" -f (Get-Date)) -ForegroundColor Cyan
            }
            $lastRun = $run
          }
          $bytes = [Text.Encoding]::UTF8.GetBytes($s.Trim())
          $ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).Wait()
        }
      }
      if ($now -ge $nextHeartbeat) {   # スマホが 1 台もいなくても、ゲームに「中継は生きている」と伝える
        $seq++; Write-Text $phonesFile "#seq $seq`n"; $nextHeartbeat = $now.AddSeconds(1)
      }
      Start-Sleep -Milliseconds 5
    }
  } catch {
    Write-Host ("[{0:HH:mm:ss}] 切れました: {1}" -f (Get-Date), $_.Exception.Message) -ForegroundColor Red
  }
  if ($ws) { try { $ws.Dispose() } catch { } }
  Write-Text $phonesFile "#seq 0`n"
  Start-Sleep -Seconds 3
}
