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

  -Dlss auto|require|off (既定 auto):
    auto    … SDK(third_party/dlss。pwsh tools/fetch_dlss.ps1 で取得)があれば DLSS 付き。無ければビルドの最後に
              「DLSS: OFF」を枠付きで表示し、DLSS 無しで成功する。
    require … SDK が無ければビルドせず終了コード 3。ビルド後に exe の隣へ nvngx_dlss.dll が無くても 3。
    off     … DLSS を入れない。古い nvngx_dlss.dll が出力に残っていれば消す。
    ゴールデンとショーケースは決定論モードで常に DLSS OFF なので、どのモードでも結果は同じ。

  やっていること:
    1. 名前付き Mutex(Global\dx12-build)で全ビルドを直列化。待っている間は待機中と表示する。
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
    [ValidateSet('auto', 'require', 'off')][string]$Dlss = 'auto'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# --- DLSS SDK の有無（CMakeLists.txt の UNO_DLSS_ACTIVE と同じ 3 ファイルで判定する）
$dlssSdk = Join-Path $root 'third_party\dlss'
# 構成済みなら CMake キャッシュの UNO_DLSS_SDK_DIR（ユーザーが差し替え可能）を優先する
$dlssCache = Join-Path (Join-Path $root $Dir) 'CMakeCache.txt'
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
if ($Jobs -le 0) {
    $logical = [Environment]::ProcessorCount
    $Jobs = [Math]::Max(2, [Math]::Min(12, [int]($logical / 2)))
}

# --- 1. 排他ロック（別プロセスのビルドが終わるまで待つ）
$mutex = [System.Threading.Mutex]::new($false, 'Global\dx12-build')
$sw = [Diagnostics.Stopwatch]::StartNew()
$got = $false
try {
    if (-not $mutex.WaitOne(0)) {
        Write-Host "[build] another build is running. waiting for the lock (up to $WaitMinutes min)..."
        $got = $mutex.WaitOne([TimeSpan]::FromMinutes($WaitMinutes))
        if (-not $got) { Write-Host "[build] lock timeout"; exit 3 }
        Write-Host ("[build] lock acquired after {0:n0}s" -f $sw.Elapsed.TotalSeconds)
    } else { $got = $true }
} catch [System.Threading.AbandonedMutexException] {
    $got = $true   # 前のビルドが異常終了して残したロック。引き継いで進める
}

try {
    # --- 2. 優先度を下げる（子プロセスに継承される）
    try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}

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
    Write-Host "[build] cmake $($args2 -join ' ')  (jobs=$Jobs, priority=BelowNormal)"
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
    if ($got) { try { $mutex.ReleaseMutex() } catch {} }
    $mutex.Dispose()
}
