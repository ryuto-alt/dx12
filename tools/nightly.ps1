#Requires -Version 7.0
<#
  夜間ビルド / 改修前バイナリ作成。統合ブランチ(または任意の Ref)を専用 worktree でビルドして、
  exe 一式を出力フォルダへ置く。ビルドは tools/build.ps1 経由(名前付き Mutex Global\dx12-build、BelowNormal)。
  窓は出さない(ビルドと robocopy のみ。エンジンは起動しない)。

    pwsh tools/nightly.ps1                                         # overhaul/wave0 を dx12-wt\nightly でビルド → %USERPROFILE%\dx12-nightly
    pwsh tools/nightly.ps1 -Ref pre-overhaul -Out dx12-preoverhaul # 改修前(1 回だけ)。-Out は名前なら %USERPROFILE% 配下
    pwsh tools/nightly.ps1 -WhatIf                                 # 予定だけ表示(何も作らない・ビルドしない)
    pwsh tools/nightly.ps1 -Dlss require|auto|off                  # 既定 require

  - -Dlss require: third_party\dlss(本体の取得物を junction で共有)が無ければ、ビルド前に失敗する。
    構成は -DUNO_ENABLE_DLSS=ON、ビルド後に nvngx_dlss.dll が exe の隣にあることを確かめる。
    auto は SDK があれば有効、off は無効。
  - 置く物: exe(DX12Engine / GameRuntime / gh)・直下の dll・shaders・shaders-src・assets、
    そして D3D12\(Agility。ビルド出力にフォルダがあるときだけ。無ければ出力側の D3D12\ を消す)。
  - 環境に ANTHROPIC_API_KEY があれば(プロセス・ユーザー・マシンのどれでも)起動を拒む。
  - 増分: worktree と build\release を使い回す。2 回目以降は差分だけビルドする。
  - スケジュール登録はこのスクリプトではしない(メインがオーナー承認後に行う)。
  - 出力フォルダへ nightly.json(ref / commit / 版 / DLSS / D3D12 の有無 / 時刻)を書く。
  - 版の確認: エンジンに --version は無い(起動すると GUI になる)ので、Version.cpp の kEngineVersion と
    exe 内の ASCII 文字列を突き合わせる。
#>
param(
    [string]$Ref = 'overhaul/wave0',
    [string]$Out = '',
    [ValidateSet('require', 'auto', 'off')][string]$Dlss = 'require',
    [string]$Worktree = '',
    [int]$Jobs = 0,
    [switch]$IgnoreManifestMismatch,
    [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'

# --- 課金の経路を持ち込まない
foreach ($scope in 'Process', 'User', 'Machine') {
    if ([Environment]::GetEnvironmentVariable('ANTHROPIC_API_KEY', $scope)) {
        Write-Host "[nightly] ANTHROPIC_API_KEY が $scope に設定されている。API 課金を避けるため起動を拒否する。"
        exit 2
    }
}

# --- 本体(メイン worktree)
$here = Split-Path -Parent $PSScriptRoot
$common = (git -C $here rev-parse --path-format=absolute --git-common-dir).Trim()
$main = (Split-Path -Parent $common) -replace '/', '\'
$home2 = $env:USERPROFILE

$isDefaultRef = ($Ref -eq 'overhaul/wave0')
$slug = ($Ref -replace '[^A-Za-z0-9._-]', '-')
if (-not $Out) { $Out = if ($isDefaultRef) { 'dx12-nightly' } else { "dx12-nightly-$slug" } }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $home2 $Out }
$Out = [IO.Path]::GetFullPath($Out)
if (-not $Worktree) {
    $wtName = if ($isDefaultRef) { 'nightly' } else { "nightly-$slug" }
    $Worktree = Join-Path (Join-Path (Split-Path -Parent $main) 'dx12-wt') $wtName
}
$Worktree = [IO.Path]::GetFullPath($Worktree)

# 安全: 出力と worktree が本体・ソースと重ならないこと
foreach ($p in @($Out, $Worktree)) {
    if ($p.TrimEnd('\') -ieq $main.TrimEnd('\')) { Write-Host "[nightly] 本体は指定できない: $p"; exit 2 }
}
if ($Out.TrimEnd('\') -ieq $Worktree.TrimEnd('\')) { Write-Host '[nightly] -Out と -Worktree が同じ'; exit 2 }

# -Out は本体・登録済み worktree の中や等しい場所、ホーム直下を禁止(robocopy /MIR で消さないため)
$allWt = @(git -C $main worktree list --porcelain | Where-Object { $_ -like 'worktree *' } |
    ForEach-Object { ([IO.Path]::GetFullPath($_.Substring(9).Trim())).TrimEnd('\') })
$outT = $Out.TrimEnd('\')
if ($outT -ieq ([IO.Path]::GetFullPath($home2)).TrimEnd('\')) { Write-Host "[nightly] -Out にホームは指定できない: $Out"; exit 2 }
foreach ($w in $allWt) {
    if ($outT -ieq $w -or $outT.StartsWith($w + '\', [StringComparison]::OrdinalIgnoreCase)) { Write-Host "[nightly] -Out が worktree の中: $Out ($w)"; exit 2 }
    # worktree の祖先(例: dx12-wt 全体)も不可
    if ($w.StartsWith($outT + '\', [StringComparison]::OrdinalIgnoreCase)) { Write-Host "[nightly] -Out が worktree の祖先: $Out ($w)"; exit 2 }
}
# ドライブのルートとその直下、ホームの祖先も不可
$homeT = ([IO.Path]::GetFullPath($home2)).TrimEnd('\')
if ($homeT.StartsWith($outT + '\', [StringComparison]::OrdinalIgnoreCase)) { Write-Host "[nightly] -Out がホームの祖先: $Out"; exit 2 }
$outRoot = [IO.Path]::GetPathRoot($Out).TrimEnd('\')
if ($outT -ieq $outRoot -or (Split-Path -Parent $outT).TrimEnd('\') -ieq $outRoot) { Write-Host "[nightly] -Out にドライブのルート直下は指定できない: $Out"; exit 2 }
# 使えるのは「存在しない」「空」「前回の nightly.json がある」フォルダだけ(robocopy /MIR や dll 削除で他物を消さない)
if (Test-Path -LiteralPath $Out) {
    $hasItems = $null -ne (Get-ChildItem -LiteralPath $Out -Force -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($hasItems -and -not (Test-Path -LiteralPath (Join-Path $Out 'nightly.json'))) {
        Write-Host "[nightly] -Out は空か前回の nightly.json があるフォルダだけ使える: $Out"; exit 2
    }
}

# 登録済みの worktree を再利用してよいのは dx12-wt\nightly* の detached だけ(レーンの作業を reset --hard で消さない)
if ($allWt -icontains $Worktree.TrimEnd('\')) {
    $blkAll = (git -C $main worktree list --porcelain) -join "`n"
    $mine0 = ($blkAll -split "`n`n") | Where-Object { ($_ -split "`n")[0].Substring(9).Trim().Replace('/', '\').TrimEnd('\') -ieq $Worktree.TrimEnd('\') } | Select-Object -First 1
    if ((Split-Path -Leaf $Worktree) -notlike 'nightly*' -or -not $mine0 -or $mine0 -match '(?m)^branch ') {
        Write-Host "[nightly] 再利用できない worktree(nightly* かつ detached のみ可): $Worktree"; exit 2
    }
}

$commit = (git -C $main rev-parse --verify "$Ref^{commit}" 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $commit) { Write-Host "[nightly] Ref が見つからない: $Ref"; exit 2 }
$commit = $commit.Trim()

# 依存の取り違え防止: 本体の vcpkg_installed を使い回すので、Ref の vcpkg.json が本体と違えば拒否する。
# (改修前ビルドは M2 で vcpkg.json が変わる前に実行すること)
$oldEnc = [Console]::OutputEncoding; [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$refManifest = (git -C $main show "${commit}:vcpkg.json" 2>$null) -join "`n"
[Console]::OutputEncoding = $oldEnc
$mainManifestPath = Join-Path $main 'vcpkg.json'
if ($refManifest -and (Test-Path $mainManifestPath)) {
    $mainManifest = (Get-Content -Raw $mainManifestPath)
    $norm = { param($t) ($t -replace '[^\x21-\x7E]', '') }
    if ((& $norm $refManifest) -ne (& $norm $mainManifest)) {
        Write-Host "[nightly] $Ref の vcpkg.json が本体と違う。本体の vcpkg_installed を使い回すと依存が食い違うため拒否する(-IgnoreManifestMismatch で強行)。"
        if (-not $IgnoreManifestMismatch) { exit 2 }
    }
}

$junctionNames = @('third_party\dlss', 'third_party\agility')
Write-Host "[nightly] ref=$Ref ($($commit.Substring(0,9))) worktree=$Worktree out=$Out dlss=$Dlss"
if ($Dlss -eq 'require' -and -not (Test-Path (Join-Path $main 'third_party\dlss\include\nvsdk_ngx_helpers.h'))) {
    Write-Host '[nightly] -Dlss require だが本体に third_party\dlss が無い。pwsh tools/fetch_dlss.ps1 を先に実行すること。'
    if (-not $WhatIf) { exit 1 }
}
$wtRegistered = $allWt -icontains $Worktree.TrimEnd('\')
if ((Test-Path $Worktree) -and -not $wtRegistered) { Write-Host "[nightly] 登録されていないフォルダが既にある: $Worktree"; exit 2 }
if (-not (Test-Path (Join-Path $Worktree 'build\release\build.ninja'))) {
    foreach ($p in @((Join-Path $main 'build\release\vcpkg_installed'), (Join-Path $main 'build\release\_deps\assimp-src'))) {
        if (-not (Test-Path $p)) { Write-Host "[nightly] 本体に $p が無い。本体を先にビルドしてあること"; exit 1 }
    }
}
if ($WhatIf) {
    Write-Host "[nightly] WhatIf: worktree $(if (Test-Path $Worktree) { '再利用' } else { '新規作成(--detach)' }), configure $(if (Test-Path (Join-Path $Worktree 'build\release\build.ninja')) { '済み(増分)' } else { '実行' })"
    foreach ($j in $junctionNames) { Write-Host ("[nightly] WhatIf: junction {0} ({1})" -f $j, $(if (Test-Path (Join-Path $main $j)) { '張る' } else { '本体に無い' })) }
    Write-Host "[nightly] WhatIf: build tools/build.ps1 -Target DX12Engine,GameRuntime (Mutex Global\dx12-build, BelowNormal)"
    Write-Host "[nightly] WhatIf: robocopy 一式 → $Out (D3D12\ はあるときだけ)"
    exit 0
}

$sw = [Diagnostics.Stopwatch]::StartNew()

# --- Mutex(全体を通して持つ。build.ps1 の同名 Mutex は同一スレッドなので再入できる)
$mutex = [System.Threading.Mutex]::new($false, 'Global\dx12-build')
$got = $false
try {
    try {
        if (-not $mutex.WaitOne(0)) {
            Write-Host '[nightly] 別のビルドが走行中。ロック待ち(最大 60 分)...'
            $got = $mutex.WaitOne([TimeSpan]::FromMinutes(60))
            if (-not $got) { Write-Host '[nightly] lock timeout'; exit 3 }
        } else { $got = $true }
    } catch [System.Threading.AbandonedMutexException] { $got = $true }
    try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}

    # --- worktree(detached。overhaul/wave0 は本体が checkout 済みなのでブランチは使わない)
    $registered = @(git -C $main worktree list --porcelain | Where-Object { $_ -like 'worktree *' } |
        ForEach-Object { ($_.Substring(9).Trim() -replace '/', '\').TrimEnd('\') })
    $isReg = $registered -icontains $Worktree.TrimEnd('\')
    if (-not $isReg) {
        if (Test-Path $Worktree) { Write-Host "[nightly] 登録されていないフォルダが既にある: $Worktree"; exit 2 }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Worktree) | Out-Null
        git -C $main worktree add --detach $Worktree $commit
        if ($LASTEXITCODE -ne 0) { Write-Host '[nightly] git worktree add failed'; exit 1 }
    } else {
        # 再利用してよいのは dx12-wt の nightly* の detached worktree だけ(レーンの作業を消さない)
        $leaf = Split-Path -Leaf $Worktree
        $blk = (git -C $main worktree list --porcelain) -join "`n"
        $mine = ($blk -split "`n`n") | Where-Object { $_ -match ('(?m)^worktree ' + [regex]::Escape(($Worktree -replace '\\', '/')) + '\s*$') -or $_ -match ('(?m)^worktree ' + [regex]::Escape($Worktree) + '\s*$') } | Select-Object -First 1
        if ($leaf -notlike 'nightly*' -or -not $mine -or $mine -match '(?m)^branch ') {
            Write-Host "[nightly] 再利用できない worktree(nightly* かつ detached のみ可): $Worktree"; exit 2
        }
        # 追跡ファイルだけ戻す(build\ や取得物の junction は未追跡 / ignore なので残る)
        git -C $Worktree reset --hard -q
        git -C $Worktree -c advice.detachedHead=false checkout --detach $commit
        if ($LASTEXITCODE -ne 0) { Write-Host '[nightly] git checkout failed'; exit 1 }
    }

    # --- 未追跡だがビルドに必要な物を本体から
    $key = 'src\core\generated\AssetKey.h'
    $keyDst = Join-Path $Worktree $key
    $keySrc = Join-Path $main $key
    if (-not (Test-Path $keySrc)) { Write-Host "[nightly] 本体に $key が無い"; exit 1 }
    if (-not (Test-Path $keyDst) -or (Get-FileHash $keySrc).Hash -ne (Get-FileHash $keyDst).Hash) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $keyDst) | Out-Null
        Copy-Item -Force $keySrc $keyDst
    }
    $gh = Join-Path $main 'third_party\gh-cli'
    if ((Test-Path $gh) -and -not (Test-Path (Join-Path $Worktree 'third_party\gh-cli'))) {
        Copy-Item -Recurse -Force $gh (Join-Path $Worktree 'third_party\gh-cli')
    }
    foreach ($j in $junctionNames) {
        $src = Join-Path $main $j; $dst = Join-Path $Worktree $j
        if ((Test-Path $src) -and -not (Test-Path $dst)) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
            cmd /c "mklink /J `"$dst`" `"$src`"" | Out-Null
        }
    }
    if ($Dlss -eq 'require' -and -not (Test-Path (Join-Path $Worktree 'third_party\dlss\include\nvsdk_ngx_helpers.h'))) {
        Write-Host '[nightly] worktree に third_party\dlss が無い(-Dlss require)'; exit 1
    }

    # --- vcvars / vcpkg の環境
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path $vc)) { Write-Host "[nightly] vcvars64.bat not found"; exit 2 }
        $vsInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
        if ((Test-Path $vsInstaller) -and ($env:PATH -notlike "*$vsInstaller*")) { $env:PATH = "$env:PATH;$vsInstaller" }
        cmd /c "`"$vc`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
    }
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $home2 'vcpkg' }
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $home2 'dx12-vcpkg-cache' }
    New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
    Remove-Item Env:\VSLANG -ErrorAction SilentlyContinue

    # --- configure(初回だけ。以降は ninja が必要なときに自動で再 configure する)
    $dlssFlag = switch ($Dlss) { 'off' { 'OFF' } default { 'ON' } }
    $bdir = Join-Path $Worktree 'build\release'
    $needCfg = -not (Test-Path (Join-Path $bdir 'build.ninja'))
    if ($needCfg) {
        $vcpkgInst = Join-Path $main 'build\release\vcpkg_installed'
        $assimp = Join-Path $main 'build\release\_deps\assimp-src'
        foreach ($p in @($vcpkgInst, $assimp)) { if (-not (Test-Path $p)) { Write-Host "[nightly] 本体に $p が無い。本体を先にビルドしてあること"; exit 1 } }
        $tc = $null
        $cache = Join-Path $main 'build\release\CMakeCache.txt'
        if (Test-Path $cache) {
            $m = Select-String -Path $cache -Pattern '^CMAKE_TOOLCHAIN_FILE:[A-Z]+=(.+)$' | Select-Object -First 1
            if ($m) { $tc = $m.Matches[0].Groups[1].Value.Trim() }
        }
        if (-not $tc) { $tc = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" }
        $cfg = @('-S', '.', '-B', 'build\release', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_TOOLCHAIN_FILE=$($tc -replace '\\','/')", '-DVCPKG_TARGET_TRIPLET=x64-windows',
            "-DVCPKG_INSTALLED_DIR=$($vcpkgInst -replace '\\','/')", '-DVCPKG_MANIFEST_INSTALL=OFF',
            "-DFETCHCONTENT_SOURCE_DIR_ASSIMP=$($assimp -replace '\\','/')", "-DUNO_ENABLE_DLSS=$dlssFlag")
        $q = ($cfg | ForEach-Object { "`"$_`"" }) -join ' '
        Push-Location $Worktree
        try {
            $oldCp = ((& cmd /c chcp 2>$null) -replace '\D', '')
            $log = & cmd /c "chcp 65001>nul && cmake $q 2>&1"
            $code = $LASTEXITCODE
            if ($oldCp -match '^\d+$' -and $oldCp -ne '65001') { & cmd /c "chcp $oldCp>nul" }
            $log | Select-Object -Last 15 | ForEach-Object { Write-Host $_ }
            if ($code -ne 0) { Write-Host '[nightly] configure failed'; exit 1 }
            if ($Dlss -eq 'require' -and -not ($log -match 'DLSS: ON')) { Write-Host '[nightly] -Dlss require だが configure が DLSS: ON にならなかった'; exit 1 }
        } finally { Pop-Location }
    } else {
        # 既存キャッシュでも -Dlss の指定を毎回反映する
        Push-Location $Worktree
        try {
            $oldCp = ((& cmd /c chcp 2>$null) -replace '\D', '')
            $log = & cmd /c "chcp 65001>nul && cmake -DUNO_ENABLE_DLSS=$dlssFlag -S . -B build\release 2>&1"
            $code = $LASTEXITCODE
            if ($oldCp -match '^\d+$' -and $oldCp -ne '65001') { & cmd /c "chcp $oldCp>nul" }
            if ($code -ne 0) { $log | Select-Object -Last 15 | ForEach-Object { Write-Host $_ }; Write-Host '[nightly] re-configure failed'; exit 1 }
            if ($Dlss -eq 'require' -and -not ($log -match 'DLSS: ON')) { Write-Host '[nightly] -Dlss require だが configure が DLSS: ON にならなかった'; exit 1 }
        } finally { Pop-Location }
    }

    # --- ビルド(増分)
    $buildArgs = @{ Target = @('DX12Engine', 'GameRuntime') }
    if ($Jobs -gt 0) { $buildArgs.Jobs = $Jobs }
    $buildPs1 = Join-Path $Worktree 'tools\build.ps1'
    Push-Location $Worktree
    try {
        & $buildPs1 @buildArgs
        $bcode = $LASTEXITCODE
    } finally { Pop-Location }
    if ($bcode -ne 0) { Write-Host "[nightly] build failed (exit $bcode)"; exit $bcode }

    # --- 検証と配置
    $src = $bdir
    foreach ($f in 'DX12Engine.exe', 'GameRuntime.exe') {
        if (-not (Test-Path (Join-Path $src $f))) { Write-Host "[nightly] ビルド出力に $f が無い"; exit 4 }
    }
    if ($Dlss -eq 'off') { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $src 'nvngx_dlss.dll') }
    if ($Dlss -eq 'require' -and -not (Test-Path (Join-Path $src 'nvngx_dlss.dll'))) {
        Write-Host '[nightly] -Dlss require だが nvngx_dlss.dll が出力に無い'; exit 4
    }
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    function Invoke-Robo([string[]]$a) {
        robocopy @a /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Write-Host "[nightly] robocopy failed ($LASTEXITCODE): $($a -join ' ')(出力側の exe が起動中ではないか)"; exit 5 }
    }
    # 直下の dll はビルド出力と揃える(出なくなった dll を残さない)
    Get-ChildItem -Path $Out -Filter *.dll -File -ErrorAction SilentlyContinue | Where-Object { -not (Test-Path (Join-Path $src $_.Name)) } | Remove-Item -Force
    Invoke-Robo @($src, $Out, 'DX12Engine.exe', 'GameRuntime.exe', 'gh.exe', '*.dll')
    Invoke-Robo @((Join-Path $src 'shaders'), (Join-Path $Out 'shaders'), '/MIR')
    Invoke-Robo @((Join-Path $Worktree 'assets'), (Join-Path $Out 'assets'), '/MIR', '/XF', 'splash_d.*')
    Invoke-Robo @((Join-Path $Worktree 'shaders'), (Join-Path $Out 'shaders-src'), '/MIR')
    $d3d = Join-Path $src 'D3D12'
    $hasD3D = Test-Path $d3d
    if ($hasD3D) { Invoke-Robo @($d3d, (Join-Path $Out 'D3D12'), '/MIR') }
    elseif (Test-Path (Join-Path $Out 'D3D12')) { Remove-Item -Recurse -Force (Join-Path $Out 'D3D12') }

    # --- 版の確認(Version.cpp と exe の ASCII 文字列)
    $ver = ''
    $vf = Join-Path $Worktree 'src\core\Version.cpp'
    if (Test-Path $vf) {
        $vm = Select-String -Path $vf -Pattern 'kEngineVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
        if ($vm) { $ver = $vm.Matches[0].Groups[1].Value }
    }
    $inExe = $false
    if ($ver) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $Out 'DX12Engine.exe'))
        $needle = [Text.Encoding]::ASCII.GetBytes($ver + [char]0)
        $text = [Text.Encoding]::Latin1.GetString($bytes)
        $inExe = $text.Contains([Text.Encoding]::Latin1.GetString($needle))
    }
    $meta = [ordered]@{
        ref = $Ref; commit = $commit; engineVersion = $ver; versionInExe = $inExe
        dlss = $Dlss; dlssDll = (Test-Path (Join-Path $Out 'nvngx_dlss.dll')); d3d12Folder = $hasD3D
        d3d12CoreDll = (Test-Path (Join-Path $Out 'D3D12\D3D12Core.dll'))
        builtAt = (Get-Date).ToString('s'); seconds = [int]$sw.Elapsed.TotalSeconds
    }
    ($meta | ConvertTo-Json) | Set-Content -Path (Join-Path $Out 'nightly.json') -Encoding utf8
    Write-Host ("[nightly] OK ref={0} commit={1} version={2} (exe 内で確認: {3}) D3D12\={4} out={5} ({6:n0}s)" -f $Ref, $commit.Substring(0, 9), $ver, $inExe, $hasD3D, $Out, $sw.Elapsed.TotalSeconds)
    if ($ver -and -not $inExe) { Write-Host '[nightly] WARNING: 版の文字列が exe に見つからない'; exit 6 }
    exit 0
}
finally {
    if ($got) { try { $mutex.ReleaseMutex() } catch {} }
    $mutex.Dispose()
}
