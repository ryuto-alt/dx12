# GroveLab: スマホをカートレースのコントローラーにする中継（PC 側）
#   部屋番号（4 桁）を決めて中継サーバーにつなぎ、
#     スマホの入力  → UDP 127.0.0.1:47811（GroveKart.lua が毎フレーム受ける）
#     レースの状況 ← UDP 127.0.0.1:47812（GroveKart.lua が送る）をスマホへ送る
#     参加用の QR  → assets\kart\room.txt（ゲーム画面に描く。起動時に 1 回だけ書く）
#   ★以前はスマホの入力を phones.txt に 1 秒 30 回書いていた。OneDrive の中だと同期と Defender の検査が
#     休まず走って PC が重くなったので、PC の中だけの UDP に替えた（ファイルは書き換えない）。
#     古いエンジン（net.udpOpen が無い）のゲームが state.txt を書いているのに気づいたときだけ、ファイルでも渡す。
#   Windows PowerShell 5.1 で動く（追加のインストールはいらない）。止めるときは窓を閉じる
param(
  [string]$Server = "grove-kart.cafiyagi.workers.dev",
  [string]$Room = ""
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$utf8 = New-Object Text.UTF8Encoding $false

$GamePort = 47811    # ゲームが受ける口
$RelayPort = 47812   # この中継が受ける口

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

# PC の中だけの UDP（127.0.0.1 だけ。外からは届かない）
$udp = New-Object System.Net.Sockets.UdpClient([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, $RelayPort))
# ゲームがまだ口を開けていないときに送ると、Windows は次の Receive を「接続がリセットされた」で失敗させる。
#   その通知（SIO_UDP_CONNRESET）を切っておく
try { [void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null) } catch { }
$gameEp = [Net.IPEndPoint]::new([Net.IPAddress]::Loopback, $GamePort)
$anyEp = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
function Send-Game($text) {
  $b = $utf8.GetBytes($text)
  try { [void]$udp.Send($b, $b.Length, $gameEp) } catch { }   # ゲームがまだ口を開けていなくても気にしない
}

$seq = 0
$lastRun = ""
$lastUdpState = [DateTime]::MinValue   # ゲームから UDP で状況が来た最後の時刻
$fileMode = $false                      # 古いゲーム（ファイルでやり取り）向けに phones.txt も書くか
$ct = [Threading.CancellationToken]::None
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
    $nextHeartbeat = Get-Date
    $nextFileCheck = Get-Date
    while ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
      $now = Get-Date
      # ---- 中継サーバー → スマホの入力（#phones）をゲームへ ----
      if ($recv.IsCompleted) {
        if ($recv.IsFaulted) { throw $recv.Exception }
        $r = $recv.Result
        if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { break }
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
        if ($r.EndOfMessage) {
          $m = $sb.ToString(); [void]$sb.Clear()
          if ($m.StartsWith("#phones")) {
            $seq++
            $msg = "#seq $seq`n" + $m.Substring(8)
            Send-Game $msg
            if ($fileMode) { Write-Text $phonesFile $msg }
            $nextHeartbeat = $now.AddSeconds(1)
          }
        }
        $recv = $ws.ReceiveAsync($seg, $ct)
        continue
      }
      # ---- ゲーム → レースの状況をスマホへ ----
      $s = $null
      while ($udp.Available -gt 0) {
        try { $b = $udp.Receive([ref]$anyEp) } catch { break }
        $s = $utf8.GetString($b)
        $lastUdpState = $now
      }
      if (-not $s -and $now -ge $nextFileCheck) {
        # 古いゲームは state.txt を書く。UDP が 3 秒来ていなくて state.txt が新しければファイルでやり取りする
        $nextFileCheck = $now.AddMilliseconds(200)
        $fi = Get-Item -LiteralPath $stateFile -ErrorAction SilentlyContinue
        $wasFile = $fileMode
        $fileMode = ($now - $lastUdpState).TotalSeconds -gt 3 -and $fi -and ($now - $fi.LastWriteTime).TotalSeconds -lt 3
        if ($fileMode -and -not $wasFile) { Write-Host "  古いゲーム（ファイルでやり取り）に合わせます" -ForegroundColor Yellow }
        if ($fileMode) { try { $s = [IO.File]::ReadAllText($stateFile, $utf8) } catch { } }
      }
      if ($s -and $s.StartsWith("{") -and $s.TrimEnd().EndsWith("}")) {
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
      if ($now -ge $nextHeartbeat) {   # スマホが 1 台もいなくても、ゲームに「中継は生きている」と伝える
        $seq++
        Send-Game "#seq $seq`n"
        if ($fileMode) { Write-Text $phonesFile "#seq $seq`n" }
        $nextHeartbeat = $now.AddSeconds(1)
      }
      Start-Sleep -Milliseconds 5
    }
  } catch {
    Write-Host ("[{0:HH:mm:ss}] 切れました: {1}" -f (Get-Date), $_.Exception.Message) -ForegroundColor Red
  }
  if ($ws) { try { $ws.Dispose() } catch { } }
  Send-Game "#seq 0`n"
  if ($fileMode) { Write-Text $phonesFile "#seq 0`n" }
  Start-Sleep -Seconds 3
}
