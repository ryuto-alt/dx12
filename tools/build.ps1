<#
  dx12 エンジンの標準ビルドラッパー。CPU を使い切らず、複数のエージェント/セッションが
  同時にビルドしても潰し合わないようにする。ビルドは必ずこれ経由で行う。

    pwsh tools/build.ps1                     # DX12Engine だけ Release 増分ビルド(テストは建てない)
    pwsh tools/build.ps1 -Target GameRuntime # ターゲット指定(複数可: -Target DX12Engine,GameRuntime)
    pwsh tools/build.ps1 -Tests              # 全ターゲット(テスト exe 含む)
    pwsh tools/build.ps1 -Jobs 8             # 並列数を明示(既定 = 論理コア/2, 最大 12)
    pwsh tools/build.ps1 -Dir build\tracy    # 別ビルドディレクトリ

  やっていること:
    1. 名前付き Mutex(Global\dx12-build)で全ビルドを直列化。待っている間は待機中と表示する。
    2. プロセス優先度を BelowNormal にする(cl.exe / link.exe に継承される)。PC 操作が重くならない。
    3. 並列数を制限(-j)し、Ninja の負荷平均制限(-l)も付ける。
    4. vcvars64 を自動で読み込む(通常の PowerShell / bash から直接呼べる)。
    5. 所要時間を表示。終了コードは cmake の終了コードをそのまま返す。
#>
param(
    [string[]]$Target = @('DX12Engine'),
    [string]$Dir = 'build\release',
    [int]$Jobs = 0,
    [switch]$Tests,
    [int]$WaitMinutes = 40
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

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
        cmd /c "`"$vc`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
    }
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'C:\Users\ryuto\vcpkg' }

    # cl.exe の /showIncludes を英語で出させる。日本語ロケールのままだと ninja が既定の英語接頭辞
    # ("Note: including file:")を見つけられず、ヘッダ依存が記録されない(.ninja_deps に `#deps 0` が残る)。
    # → ヘッダを変えても再コンパイルされず、古い obj が混ざって SEGFAULT や嘘のテスト失敗になる。
    $env:VSLANG = '1033'

    # `#deps 0` の obj を検出し、あれば一度だけ clean して再コンパイルさせる(次回以降は依存が正しく記録される)。
    $cache = Join-Path (Join-Path $root $Dir) 'CMakeCache.txt'
    $ninja = $null
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^CMAKE_MAKE_PROGRAM:[A-Z]+=(.+)$' | Select-Object -First 1
        if ($m) { $ninja = $m.Matches[0].Groups[1].Value.Trim() }
    }
    if ($ninja -and (Test-Path $ninja)) {
        $bad = & $ninja -C $Dir -t deps 2>$null | Where-Object { $_ -match '^(.+\.obj): #deps 0,' } |   # .ddi(C++20 モジュール走査)の #deps 0 は正常なので .obj だけ
               ForEach-Object { $Matches[1] }
        if ($bad -and $bad.Count -gt 0) {
            Write-Host "[build] WARNING: $($bad.Count) objects have no header deps (#deps 0). cleaning them once so they rebuild with correct deps."
            $bad | ForEach-Object -Begin { $batch = @() } -Process {
                $batch += $_
                if ($batch.Count -ge 50) { & $ninja -C $Dir -t clean @batch 2>$null | Out-Null; $batch = @() }
            } -End { if ($batch.Count -gt 0) { & $ninja -C $Dir -t clean @batch 2>$null | Out-Null } }
        }
    }

    # --- 3. ビルド
    $args2 = @('--build', $Dir, '-j', $Jobs)
    if (-not $Tests) { foreach ($t in $Target) { $args2 += @('--target', $t) } }
    $args2 += @('--', '-l', ([Environment]::ProcessorCount - 2))
    Write-Host "[build] cmake $($args2 -join ' ')  (jobs=$Jobs, priority=BelowNormal)"
    $bsw = [Diagnostics.Stopwatch]::StartNew()
    & cmake @args2
    $code = $LASTEXITCODE
    Write-Host ("[build] {0} in {1:n1}s (exit {2})" -f ($(if ($code -eq 0) { 'OK' } else { 'FAILED' })), $bsw.Elapsed.TotalSeconds, $code)
    exit $code
}
finally {
    if ($got) { try { $mutex.ReleaseMutex() } catch {} }
    $mutex.Dispose()
}
