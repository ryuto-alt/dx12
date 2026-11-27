<#
  dx12 エンジンの標準ビルドラッパー。CPU を使い切らず、複数のエージェント/セッションが
  同時にビルドしても潰し合わないようにする。ビルドは必ずこれ経由で行う。

    pwsh tools/build.ps1                     # DX12Engine だけ Release 増分ビルド(テストは建てない)
    pwsh tools/build.ps1 -Target GameRuntime # ターゲット指定(複数可: -Target DX12Engine,GameRuntime)
    pwsh tools/build.ps1 -Tests              # 全ターゲット(テスト exe 含む)
    pwsh tools/build.ps1 -Jobs 8             # 並列数を明示(既定 = 論理コア/2, 最大 12)
    pwsh tools/build.ps1 -Dir build\tracy    # 別ビルドディレクトリ
    pwsh tools/build.ps1 -Dlss require       # DLSS SDK が無ければ失敗(終了コード 3)。夜間ビルドと公式リリース用
    pwsh tools/build.ps1 -Dlss off           # DLSS を入れない(-DUNO_ENABLE_DLSS=OFF)
    pwsh tools/build.ps1 -SlotProbe 15       # テスト用: 本番と同じ手順でロックを取り、スロット番号を出して 15 秒待つだけ(cmake なし)

  -Dlss auto|require|off (既定 auto):
    auto    … SDK(third_party/dlss。pwsh tools/fetch_dlss.ps1 で取得)があれば DLSS 付き。無ければビルドの最後に
              「DLSS: OFF」を枠付きで表示し、DLSS 無しで成功する。
    require … SDK が無ければビルドせず終了コード 3。ビルド後に exe の隣へ nvngx_dlss.dll が無くても 3。
    off     … DLSS を入れない。古い nvngx_dlss.dll が出力に残っていれば消す。
    ゴールデンとショーケースは決定論モードで常に DLSS OFF なので、どのモードでも結果は同じ。

  やっていること:
    1. 名前付き Mutex で同時ビルド数を制限する(既定 2 本。環境変数 DX12_BUILD_SLOTS=1..3)。
       - スロット: Global\dx12-build(スロット 1。旧版 build.ps1 はこれだけを取る)と Global\dx12-build-<k>(k>=2)。
         利用者が操作中はビルド 1 本、10 分以上操作が無いときだけ 2 本目を Idle 優先度で走らせる。
         スロット 2 以降は「最後の入力から DX12_BUILD_IDLE_MIN 分(既定 10)以上」かつ
         「空き物理メモリが DX12_BUILD_MIN_FREE_GB(既定 8)以上」のときだけ取る。
         スロット 2 以降のビルドは Idle 優先度・-j 8(-Jobs で上書き可)。スロット 1 は BelowNormal・従来の -j。
         (テスト用: DX12_BUILD_FAKE_IDLE_SEC で「最後の入力からの秒数」を差し替え)
       - 先にビルドディレクトリ単位の Mutex(Global\dx12-build-dir-<パスの SHA1 先頭16桁>)と
         <Dir>\.build-in-progress(PID。旧版は書かない)で、同じディレクトリを同時に建てないようにする。
       - 待っている間は待機中と表示する(30 秒ごとに各スロットの状態)。
       - 接頭辞は DX12_BUILD_SLOT_PREFIX で差し替え可(テスト用。既定 Global\dx12-build)。
    2. プロセス優先度を BelowNormal にする(cl.exe / link.exe に継承される)。PC 操作が重くならない。
    3. 並列数を制限(-j)し、Ninja の負荷平均制限(-l)も付ける。
    4. vcvars64 を自動で読み込む(通常の PowerShell / bash から直接呼べる)。
    5. ビルドは chcp 65001 下で走らせ、前後で #deps 0 を検査・報告する。
    6. 所要時間を表示。終了コードは cmake の終了コードをそのまま返す。
#>
param(
    [string[]]$Target = @('DX12Engine'),
    [string]$Dir = 'build\release',
    [int]$Jobs = 0,
    [switch]$Tests,
    [int]$WaitMinutes = 40,
    [int]$SlotProbe = 0,
    [ValidateSet('auto', 'require', 'off')][string]$Dlss = 'auto'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# --- DLSS SDK の有無（CMakeLists.txt の UNO_DLSS_ACTIVE と同じ 3 ファイルで判定する）
$dlssSdk = Join-Path $root 'third_party\dlss'
# 構成済みなら CMake キャッシュの UNO_DLSS_SDK_DIR（ユーザーが差し替え可能）を優先する
$dirFull = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $Dir))
$dlssCache = Join-Path $dirFull 'CMakeCache.txt'
if (Test-Path $dlssCache) {
    $sd = Select-String -Path $dlssCache -Pattern '^UNO_DLSS_SDK_DIR:[A-Z]+=(.+)$' | Select-Object -First 1
    if ($sd) { $dlssSdk = $sd.Matches[0].Groups[1].Value.Trim() -replace '/', '\' }
}
$sdkPresent = (Test-Path (Join-Path $dlssSdk 'include\nvsdk_ngx_helpers.h')) -and
              (Test-Path (Join-Path $dlssSdk 'lib\Windows_x86_64\x64\nvsdk_ngx_d.lib')) -and
              (Test-Path (Join-Path $dlssSdk 'lib\Windows_x86_64\rel\nvngx_dlss.dll'))
if ($Dlss -eq 'require' -and -not $sdkPresent) {
    Write-Host '[build] -Dlss require: DLSS SDK (third_party/dlss) が無い。pwsh tools/fetch_dlss.ps1 で取得してから実行して'
    exit 3
}
$dlssWanted = ($Dlss -ne 'off') -and $sdkPresent   # このビルドで DLSS が有効になるはずか

# --- 並列数: 論理コアの半分、最大 12（16GB 機で cl 1本が最大 ~1GB 使うため）
$jobsExplicit = $Jobs -gt 0
if ($Jobs -le 0) {
    $logical = [Environment]::ProcessorCount
    $Jobs = [Math]::Max(2, [Math]::Min(12, [int]($logical / 2)))
}

# --- 1. 排他ロック: ビルドディレクトリ単位のロック + 最大 N 個の「スロット」(同時ビルド数)
#   スロット 1 は旧名 Global\dx12-build のまま(旧版 build.ps1 / nightly.ps1 はスロット 1 だけを取る)。
#   スロット k>=2 は Global\dx12-build-<k>。接頭辞は DX12_BUILD_SLOT_PREFIX で差し替え可(テスト用)。
$slotPrefix = if ($env:DX12_BUILD_SLOT_PREFIX) { $env:DX12_BUILD_SLOT_PREFIX } else { 'Global\dx12-build' }
$slotCount = 2
if ($env:DX12_BUILD_SLOTS -match '^\d+$') { $slotCount = [Math]::Max(1, [Math]::Min(3, [int]$env:DX12_BUILD_SLOTS)) }
$idleMinMin = 10.0
if ($env:DX12_BUILD_IDLE_MIN -match '^\d+(\.\d+)?$') { $idleMinMin = [double]$env:DX12_BUILD_IDLE_MIN }
$minFreeGb = 8.0
if ($env:DX12_BUILD_MIN_FREE_GB -match '^\d+(\.\d+)?$') { $minFreeGb = [double]$env:DX12_BUILD_MIN_FREE_GB }

$dirMutex = $null; $dirGot = $false
$slotMutexes = @(); $slotGot = 0       # $slotGot = 取れたスロット番号(0 = 未取得)
$markerPath = Join-Path $dirFull '.build-in-progress'; $markerWritten = $false
$sw = [Diagnostics.Stopwatch]::StartNew()

# Mutex を取る。放棄された Mutex(前のビルドが異常終了)も取得扱いにする。
function Enter-BuildMutex([System.Threading.Mutex]$m, [int]$ms, [string]$what) {
    try { return $m.WaitOne($ms) }
    catch [System.Threading.AbandonedMutexException] {
        Write-Host "[build] note: $what was abandoned by a crashed build. taking it over"
        return $true
    }
}
# 最後のキー/マウス入力からの経過秒。取れなければ 0(= 操作中扱いで安全側)。DX12_BUILD_FAKE_IDLE_SEC で差し替え可
function Get-UserIdleSec {
    if ($env:DX12_BUILD_FAKE_IDLE_SEC -match '^\d+(\.\d+)?$') { return [double]$env:DX12_BUILD_FAKE_IDLE_SEC }
    try {
        if (-not ('Dx12BuildIdle' -as [type])) {
            Add-Type -Name 'Dx12BuildIdle' -Namespace 'Dx12' -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
public static double IdleSec() {
    LASTINPUTINFO li = new LASTINPUTINFO(); li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
    if (!GetLastInputInfo(ref li)) return 0;
    uint now = unchecked((uint)Environment.TickCount64);
    return unchecked(now - li.dwTime) / 1000.0;
}
'@ -UsingNamespace 'System.Runtime.InteropServices'
        }
        return [Dx12.Dx12BuildIdle]::IdleSec()
    } catch { return 0 }
}
function Get-FreePhysicalGb {
    try { return [double](Get-CimInstance Win32_OperatingSystem -Property FreePhysicalMemory).FreePhysicalMemory / 1MB } catch { return 1e9 }
}
function Test-MarkerBusy([string]$path) {
    # 別プロセスが同じディレクトリをビルド中なら true(マーカーの PID が生きている)。旧版はマーカーを書かない
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try { $raw = (Get-Content -LiteralPath $path -Raw -ErrorAction Stop).Trim() } catch { return $false }
    if ($raw -notmatch '^\d+$' -or [int]$raw -eq $PID) { return $false }
    return [bool](Get-Process -Id ([int]$raw) -ErrorAction SilentlyContinue)
}

try {
    $deadline = [DateTime]::UtcNow.AddMinutes($WaitMinutes)
    $announced = $false
    function Wait-Announce { if (-not $script:announced) { $script:announced = $true; Write-Host "[build] another build is running. waiting for the lock (up to $WaitMinutes min)..." } }

    # 1a. ディレクトリ単位のロック(同じディレクトリを同時に建てない)
    $dirHash = [BitConverter]::ToString([Security.Cryptography.SHA1]::HashData([Text.Encoding]::UTF8.GetBytes($dirFull.ToLowerInvariant()))).Replace('-', '').ToLowerInvariant().Substring(0, 16)
    $dirMutex = [System.Threading.Mutex]::new($false, "$slotPrefix-dir-$dirHash")
    if (-not (Enter-BuildMutex $dirMutex 0 'dir lock')) {
        Wait-Announce
        Write-Host "[build] same build dir is being built by another process. waiting..."
        $left = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if (-not (Enter-BuildMutex $dirMutex $left 'dir lock')) { Write-Host "[build] lock timeout"; exit 3 }
    }
    $dirGot = $true
    # 旧版(マーカーを書かない)との移行期の競合は残る。各レーンは自分の worktree のディレクトリを建てるので許容。
    while (Test-MarkerBusy $markerPath) {
        Wait-Announce
        if ([DateTime]::UtcNow -ge $deadline) { Write-Host "[build] lock timeout"; exit 3 }
        Start-Sleep -Seconds 3
    }

    # 1b. スロット取得: 1..N を WaitOne(0) で順に試す。k>=2 は空きメモリが足りるときだけ。3 秒ごとにポーリング
    for ($k = 1; $k -le $slotCount; $k++) {
        $name = if ($k -eq 1) { $slotPrefix } else { "$slotPrefix-$k" }
        $slotMutexes += , [System.Threading.Mutex]::new($false, $name)
    }
    $lastReport = [DateTime]::UtcNow
    while ($slotGot -eq 0) {
        $state = @()
        for ($k = 1; $k -le $slotCount; $k++) {
            if ($k -ge 2) {
                $idle = Get-UserIdleSec
                if ($idle -lt $idleMinMin * 60) { $state += ("slot {0} not used: user active (idle {1:n0}s < {2:n0}s)" -f $k, $idle, ($idleMinMin * 60)); continue }
                $free = Get-FreePhysicalGb
                if ($free -lt $minFreeGb) { $state += ("slot {0} not used: low memory (free {1:n1} GB < {2:n1} GB)" -f $k, $free, $minFreeGb); continue }
            }
            if (Enter-BuildMutex $slotMutexes[$k - 1] 0 "slot $k") { $slotGot = $k; break }
            $state += "slot $k busy"
        }
        if ($slotGot -ne 0) { break }
        Wait-Announce
        if ([DateTime]::UtcNow -ge $deadline) { Write-Host "[build] lock timeout"; exit 3 }
        if (([DateTime]::UtcNow - $lastReport).TotalSeconds -ge 30) {
            Write-Host ("[build] waiting {0:n0}s: {1}" -f $sw.Elapsed.TotalSeconds, ($state -join ', '))
            $lastReport = [DateTime]::UtcNow
        }
        Start-Sleep -Seconds 3
    }
    if ($announced) { Write-Host ("[build] lock acquired after {0:n0}s" -f $sw.Elapsed.TotalSeconds) }
    Write-Host "[build] slot $slotGot/$slotCount"
    # スロット 2 以降は -Jobs 未指定なら -j 8(利用者が戻ってきても影響を小さくする)
    if (-not $jobsExplicit -and $slotGot -ge 2) { $Jobs = 8 }
    $prio = if ($slotGot -ge 2) { 'Idle' } else { 'BelowNormal' }

    # マーカー(ディレクトリが既にある場合だけ。無ければ cmake が作るので省略)
    if (Test-Path -LiteralPath $dirFull) { try { Set-Content -LiteralPath $markerPath -Value $PID -NoNewline; $markerWritten = $true } catch {} }

    # -SlotProbe: ロックの取り方を本番と同じにして、スロット番号を出して待つだけ(cmake は動かさない)
    if ($SlotProbe -gt 0) {
        $t0 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        Write-Host "PROBE slot=$slotGot/$slotCount acquired=$t0 waited=$([int]$sw.Elapsed.TotalSeconds)"
        Start-Sleep -Seconds $SlotProbe
        Write-Host "PROBE released=$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())"
        exit 0
    }

    # --- 2. 優先度を下げる（子プロセスに継承される。スロット 2 以降は Idle）
    try { (Get-Process -Id $PID).PriorityClass = $prio } catch {}

    # --- 4. vcvars64 を取り込む
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path $vc)) { Write-Host "[build] vcvars64.bat not found: $vc"; exit 2 }
        # vcvars64.bat が呼ぶ vswhere.exe は Installer ディレクトリが PATH に無いと「認識されていません」を出すので足しておく
        $vsInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
        if ((Test-Path $vsInstaller) -and ($env:PATH -notlike "*$vsInstaller*")) { $env:PATH = "$env:PATH;$vsInstaller" }
        cmd /c "`"$vc`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
    }
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $env:USERPROFILE 'vcpkg' }
    # vcpkg のバイナリキャッシュは全 worktree で共通の固定パス(ninja の自動 re-configure にも効く)
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:USERPROFILE 'dx12-vcpkg-cache' }
    New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null

    # cl.exe の /showIncludes はコンソールのコードページで出る。CMake は configure 時の出力を UTF-8 で
    # rules.ninja の msvc_deps_prefix に書くので、ビルドも chcp 65001 で走らせて一致させる(下の cmd /c 参照)。
    # 一致しないと ninja が依存を 1 つも記録せず(.ninja_deps に `#deps 0`)、ヘッダを変えても再コンパイルされない。
    # VSLANG=1033 は使わない(configure 側の prefix とずれる原因になるため)。
    Remove-Item Env:\VSLANG -ErrorAction SilentlyContinue

    # rules.ninja の msvc_deps_prefix が UTF-8 で書かれているか確認する(CP932 だと 83 81 83 82 ... で不正な UTF-8)。
    $rules = Join-Path (Join-Path (Join-Path $root $Dir) 'CMakeFiles') 'rules.ninja'
    if (Test-Path $rules) {
        $line = [IO.File]::ReadAllLines($rules, [Text.Encoding]::Latin1) | Where-Object { $_ -match '^\s*msvc_deps_prefix\s*=' } | Select-Object -First 1
        if ($line) {
            $bytes = [Text.Encoding]::Latin1.GetBytes($line)
            $prefixOk = $false
            try {
                $strict = [Text.UTF8Encoding]::new($false, $true)
                $txt = $strict.GetString($bytes)
                # 日本語 MSVC の既定「メモ: インクルード ファイル:」(UTF-8)、または少なくとも非 ASCII の UTF-8 であること。
                # ASCII のみ(英語ロケール)の prefix は chcp 65001 でも同一なので問題ない。
                $jp = [string]::new([char[]](0x30E1,0x30E2,0x3A))   # "メモ:"
                $prefixOk = $txt.Contains($jp) -or ($txt -notmatch '[^\x00-\x7F]' -and $txt -match 'Note: including file:')
            } catch { $prefixOk = $false }
            if (-not $prefixOk) {
                Write-Host "[build] WARNING: msvc_deps_prefix in rules.ninja is not the expected UTF-8 'メモ: インクルード ファイル:' (configured under another codepage?). Header deps would show '#deps 0'. Re-run configure under 'chcp 65001'."
            }
        }
    }

    # `#deps 0` の .obj のうち、本当に依存が欠落しているものだけを返す。
    # システムヘッダだけを include する TU は ninja が(INCLUDE 配下を除外するため)正当に #deps 0 になる。
    # ソースに `#include "..."`(プロジェクトヘッダ)がある obj だけを異常とみなす。
    function Get-BrokenDepObjs($ninjaExe, $dir) {
        $zero = @(& $ninjaExe -C $dir -t deps 2>$null | Where-Object { $_ -match '^(.+\.obj): #deps 0,.*\(VALID\)' -and $_ -notmatch '(^|[\/])_deps[\/]' } |   # .ddi(C++20 モジュール走査)の #deps 0 は正常なので .obj だけ。_deps/(外部ライブラリ)は対象外
                  ForEach-Object { $Matches[1] })
        foreach ($o in $zero) {
            $src = & $ninjaExe -C $dir -t query $o 2>$null | Where-Object { $_ -match '^\s{4}(\S.*\.(cpp|cc|cxx|c))\s*$' } | Select-Object -First 1
            if ($src -match '^\s{4}(\S.*)$') {
                $sp = $Matches[1]
                if (-not [IO.Path]::IsPathRooted($sp)) { $sp = Join-Path (Join-Path $root $dir) $sp }
                if ((Test-Path -LiteralPath $sp) -and -not (Select-String -LiteralPath $sp -Pattern '^\s*#\s*include\s*"' -Quiet)) { continue }
            }
            $o
        }
    }

    # `#deps 0` の obj を検出し、あれば一度だけ clean して再コンパイルさせる(次回以降は依存が正しく記録される)。
    $cache = Join-Path (Join-Path $root $Dir) 'CMakeCache.txt'
    $ninja = $null
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^CMAKE_MAKE_PROGRAM:[A-Z]+=(.+)$' | Select-Object -First 1
        if ($m) { $ninja = $m.Matches[0].Groups[1].Value.Trim() }
    }
    if ($ninja -and (Test-Path $ninja)) {
        $bad = @(Get-BrokenDepObjs $ninja $Dir)
        if ($bad.Count -gt 0) {
            Write-Host "[build] WARNING: $($bad.Count) objects have no header deps (#deps 0). cleaning them once so they rebuild with correct deps."
            $bad | ForEach-Object -Begin { $batch = @() } -Process {
                $batch += $_
                if ($batch.Count -ge 50) { & $ninja -C $Dir -t clean @batch 2>$null | Out-Null; $batch = @() }
            } -End { if ($batch.Count -gt 0) { & $ninja -C $Dir -t clean @batch 2>$null | Out-Null } }
        }
    }

    # --- 3a. DLSS の状態が前回の configure と違うときだけ構成し直す（UNO_ENABLE_DLSS と SDK の有無は configure で決まる）
    $dlssMarker = Join-Path (Join-Path $root $Dir) '.uno_dlss_configured'
    # 要求(-Dlss)も含めて記録する: ON / OFF:nosdk（SDK 無しで OFF）/ OFF:off（-Dlss off）
    $dlssWant = if ($dlssWanted) { 'ON' } elseif ($Dlss -eq 'off') { 'OFF:off' } else { 'OFF:nosdk' }
    # 注: マーカーが無い既存ビルドは最初の 1 回だけ（状態が同じでも）構成し直す。未構成のディレクトリは build.ps1 が configure しないので -Dlss off は効かない。
    # -Dlss require で SDK 無しの終了コード 3 はロック待ちタイムアウトと同じ。ログの "[build] -Dlss require" 行で見分ける。
    $dlssHave = ''
    if (Test-Path $dlssMarker) { $raw = Get-Content $dlssMarker -Raw; if ($raw) { $dlssHave = $raw.Trim() } }
    if ((Test-Path $cache) -and $dlssHave -ne $dlssWant) {
        $enableFlag = if ($Dlss -eq 'off') { 'OFF' } else { 'ON' }
        Write-Host "[build] DLSS configure state: '$dlssHave' -> '$dlssWant' (UNO_ENABLE_DLSS=$enableFlag). reconfiguring..."
        & cmd /c "chcp 65001>nul && cmake -S . -B `"$Dir`" -DUNO_ENABLE_DLSS=$enableFlag"
        if ($LASTEXITCODE -ne 0) { Write-Host "[build] reconfigure FAILED (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
        Set-Content -Path $dlssMarker -Value $dlssWant -NoNewline
    }

    # --- 3. ビルド
    $args2 = @('--build', $Dir, '-j', $Jobs)
    if (-not $Tests) { foreach ($t in $Target) { $args2 += @('--target', $t) } }
    $args2 += @('--', '-l', ([Environment]::ProcessorCount - 2))
    Write-Host "[build] cmake $($args2 -join ' ')  (jobs=$Jobs, priority=$prio)"
    $bsw = [Diagnostics.Stopwatch]::StartNew()
    $cmdArgs = ($args2 | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '
    $oldCp = $null
    try { $oldCp = ((& cmd /c chcp 2>$null) -replace '\D', '') } catch {}
    & cmd /c "chcp 65001>nul && cmake $cmdArgs"
    $code = $LASTEXITCODE
    if ($oldCp -match '^\d+$' -and $oldCp -ne '65001') { & cmd /c "chcp $oldCp>nul" }   # 呼び出し元のコンソール コードページを戻す
    Write-Host ("[build] {0} in {1:n1}s (exit {2})" -f ($(if ($code -eq 0) { 'OK' } else { 'FAILED' })), $bsw.Elapsed.TotalSeconds, $code)
    # ビルド後に `#deps 0` の .obj の件数を表示する(0 が正常)
    if ($ninja -and (Test-Path $ninja)) {
        $after = @(Get-BrokenDepObjs $ninja $Dir)
        if ($after.Count -gt 0) { Write-Host "[build] WARNING: #deps 0 objects after build: $($after.Count) (configure/build codepage mismatch?)" }
        else { Write-Host "[build] #deps 0 objects: 0" }
    }
    # --- 5. DLSS の結果表示（成功したときだけ。出力に実際に nvngx_dlss.dll があるかを見る）
    if ($code -eq 0) {
        $outDir = Join-Path $root $Dir
        $dllOut = Join-Path $outDir 'nvngx_dlss.dll'
        if (-not $dlssWanted) {
            # 古いビルドの残りを消す（DlssWiringCheck の「無いときは無い」を守る）。exe が掴んでいれば諦める
            foreach ($stale in @($dllOut, (Join-Path $outDir 'licenses\NVIDIA_DLSS_LICENSE.txt'))) {
                if (Test-Path $stale) { try { Remove-Item -Force $stale -ErrorAction Stop } catch {} }
            }
        }
        $dllNow = Test-Path $dllOut
        if ($dlssWanted -and -not $dllNow) {
            Write-Host '[build] DLSS: SDK はあるが出力に nvngx_dlss.dll が無い（-Target に DX12Engine / GameRuntime を含めたか確認）'
            if ($Dlss -eq 'require') { exit 3 }
        } elseif ($dllNow) {
            Write-Host '[build] DLSS: ON'
        } elseif ($Dlss -eq 'off') {
            Write-Host '[build] DLSS: OFF (-Dlss off)'
        } else {
            $msg = 'DLSS: OFF (pwsh tools/fetch_dlss.ps1)'
            $bar = '+' + ('-' * ($msg.Length + 2)) + '+'
            Write-Host $bar; Write-Host "| $msg |"; Write-Host $bar
        }
    }
    exit $code
}
finally {
    if ($markerWritten) { try { Remove-Item -LiteralPath $markerPath -Force -ErrorAction SilentlyContinue } catch {} }
    if ($slotGot -ne 0) { try { $slotMutexes[$slotGot - 1].ReleaseMutex() } catch {} }
    foreach ($m in $slotMutexes) { try { $m.Dispose() } catch {} }
    if ($dirGot) { try { $dirMutex.ReleaseMutex() } catch {} }
    if ($dirMutex) { try { $dirMutex.Dispose() } catch {} }
}
