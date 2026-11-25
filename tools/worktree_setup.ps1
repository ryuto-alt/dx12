<#
  並列作業用の git worktree を作り、本体と資産を共有したまま configure まで済ませる。

    pwsh tools/worktree_setup.ps1 -Lane A                       # %USERPROFILE%\dx12-wt\lane-a、ブランチ wave-lane-a
    pwsh tools/worktree_setup.ps1 -Lane D                       # ビルドしないレーン(NB)。既定で configure しない(-NoBuild)
    pwsh tools/worktree_setup.ps1 -Lane D -WhatIf               # 何もせず、ポート・junction・npm ci の予定だけ表示
    pwsh tools/worktree_setup.ps1 -Lane B -Branch wave1/lane-b -Base overhaul/wave0
    pwsh tools/worktree_setup.ps1 -Path %USERPROFILE%\dx12-wt\wt-selftest -Branch wt-selftest   # レーン外(使い捨て)
    pwsh tools/worktree_setup.ps1 -Path ... -Remove             # worktree を片付ける(--force)

  やっていること:
    1. git worktree add(ブランチが無ければ -Base から作る)。
    2. 本体からコピー: src/core/generated/AssetKey.h(gitignore 対象)、third_party/gh-cli(約 40MB、未追跡)。
    3. chcp 65001 の下で configure(Ninja / Release)。次を本体と共有してディスクと時間を節約する:
         -DVCPKG_INSTALLED_DIR=<本体>\build\release\vcpkg_installed  -DVCPKG_MANIFEST_INSTALL=OFF
         -DFETCHCONTENT_SOURCE_DIR_ASSIMP=<本体>\build\release\_deps\assimp-src
    4. 本体に third_party/dlss と third_party/agility があれば、各 worktree へ junction を張る(取得物の二重ダウンロードを避ける。
       -Remove は junction を先に外し、リンク先(本体側)を消さない)。
    5. tools/mcp-server で npm ci(-NoNpm で省略)。
    6. VCPKG_DEFAULT_BINARY_CACHE を共通の固定パス(未設定なら %LOCALAPPDATA%\dx12-vcpkg-cache)にそろえる(configure の間だけ)。
    7. ポートを表示する(エンジン MCP: A 8851 / B 8852 / C 8853 / D 8855、UI テスト: A〜D = 8911〜8914。
       ドッグフードと E2E 専用は MCP 8856 / UI 8915 でレーンには割り当てない)。
  ビルドは configure 後に `pwsh tools/build.ps1`(PC 全体で直列化される)。本体の build\release は触らない。
#>
param(
    [ValidateSet('A', 'B', 'C', 'D', '')][string]$Lane = '',
    [string]$Path = '',
    [string]$Branch = '',
    [string]$Base = 'HEAD',   # 本体 worktree の HEAD(実行元 worktree の HEAD ではない)
    [Alias('NoBuild')][switch]$NoConfigure,
    [switch]$NoNpm,
    [switch]$WhatIf,   # 何も作らず、予定(ポート / junction / npm ci / バイナリキャッシュ)だけ表示
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'

# 本体(メイン worktree)のルートを求める。worktree の中から呼ばれても本体を指す。
$here = Split-Path -Parent $PSScriptRoot
$common = (git -C $here rev-parse --path-format=absolute --git-common-dir).Trim()
$main = (Split-Path -Parent $common) -replace '/', '\'

if ($Lane) {
    $Lane = $Lane.ToUpper()
    if ($Lane -eq 'D') { $NoConfigure = $true }   # NB レーンはビルドしない
    if (-not $Path)   { $Path = Join-Path (Join-Path (Split-Path -Parent $main) 'dx12-wt') "lane-$($Lane.ToLower())" }
    if (-not $Branch) { $Branch = "wave-lane-$($Lane.ToLower())" }
}
if (-not $Path) { Write-Host '[wt] -Lane か -Path を指定してください'; exit 2 }
$Path = [IO.Path]::GetFullPath($Path)
if (-not $Branch) { $Branch = Split-Path -Leaf $Path }

# 本体から junction で共有する取得物（スクリプトが取得する。コミットしない）
$junctionNames = @('third_party\dlss', 'third_party\agility')

if ($Remove) {
    # 登録済みのリンク worktree だけを対象にする(本体や無関係なフォルダは絶対に消さない)。
    if ($Path.TrimEnd('\') -ieq $main.TrimEnd('\')) { Write-Host "[wt] 本体は削除できません: $Path"; exit 2 }
    $registered = @(git -C $main worktree list --porcelain | Where-Object { $_ -like 'worktree *' } |
        ForEach-Object { ($_.Substring(9).Trim() -replace '/', '\').TrimEnd('\') })
    if ($LASTEXITCODE -ne 0) { Write-Host '[wt] git worktree list failed'; exit 1 }
    if ($registered -inotcontains $Path.TrimEnd('\')) {
        Write-Host "[wt] 登録された worktree ではないため削除しません: $Path"
        Write-Host '[wt] (git worktree list で確認。残骸フォルダは手で削除してください)'
        exit 2
    }
    # junction(取得物の共有)を先に外す。リンク先(本体側)は消さない(rmdir はリンクだけを外す)。
    foreach ($j in $junctionNames) {
        $jp = Join-Path $Path $j
        $it = Get-Item -LiteralPath $jp -Force -ErrorAction SilentlyContinue
        if ($it -and ($it.Attributes -band [IO.FileAttributes]::ReparsePoint)) { cmd /c "rmdir `"$jp`"" | Out-Null }
    }
    git -C $main worktree remove --force $Path
    if ($LASTEXITCODE -ne 0) { Write-Host '[wt] git worktree remove failed'; exit 1 }
    # git 成功後に残ったフォルダ(ロック等で消えなかった分)だけ片付ける。
    if (Test-Path $Path) { Remove-Item -Recurse -Force $Path }
    git -C $main worktree prune
    Write-Host "[wt] removed $Path"
    exit 0
}

$mcpPortsW = @{ A = 8851; B = 8852; C = 8853; D = 8855 }
$uiPortsW  = @{ A = 8911; B = 8912; C = 8913; D = 8914 }
if ($WhatIf) {
    Write-Host "[wt] WhatIf: path=$Path branch=$Branch configure=$(-not $NoConfigure)"
    if ($Lane) { Write-Host ("[wt] WhatIf: lane {0}: engine MCP port {1}, UI test port {2}" -f $Lane, $mcpPortsW[$Lane], $uiPortsW[$Lane]) }
    foreach ($j in $junctionNames) {
        $src = Join-Path $main $j
        Write-Host ("[wt] WhatIf: junction {0} -> {1} ({2})" -f $j, $src, $(if (Test-Path $src) { 'あり' } else { '本体に無いので張らない' }))
    }
    Write-Host ("[wt] WhatIf: npm ci in tools\mcp-server = {0}" -f (-not $NoNpm))
    $bc = if ($env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE } else { Join-Path $env:LOCALAPPDATA 'dx12-vcpkg-cache' }
    Write-Host "[wt] WhatIf: VCPKG_DEFAULT_BINARY_CACHE=$bc"
    exit 0
}

$sw = [Diagnostics.Stopwatch]::StartNew()
if (Test-Path $Path) { Write-Host "[wt] already exists: $Path (半端な残骸なら -Remove を先に実行)"; exit 2 }

# 事前条件は worktree add の前に確認する(失敗して半端な worktree を残さない)。
$key = 'src\core\generated\AssetKey.h'
if (-not (Test-Path (Join-Path $main $key))) { Write-Host "[wt] 本体に $key が無い(本体を一度 configure してください)"; exit 1 }
$vcpkgInst = Join-Path $main 'build\release\vcpkg_installed'
$assimp = Join-Path $main 'build\release\_deps\assimp-src'
if (-not $NoConfigure) {
    foreach ($p in @($vcpkgInst, $assimp)) { if (-not (Test-Path $p)) { Write-Host "[wt] 本体に $p が無い。本体を先にビルドしてください"; exit 1 } }
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null

git -C $main show-ref --verify --quiet "refs/heads/$Branch"
if ($LASTEXITCODE -eq 0) { git -C $main worktree add $Path $Branch }
else { git -C $main worktree add -b $Branch $Path $Base }
if ($LASTEXITCODE -ne 0) { Write-Host '[wt] git worktree add failed'; exit 1 }

# 未追跡だがビルドに必要なものを本体からコピーする。
New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $Path $key)) | Out-Null
Copy-Item (Join-Path $main $key) (Join-Path $Path $key)
$gh = Join-Path $main 'third_party\gh-cli'
if (Test-Path $gh) { Copy-Item -Recurse -Force $gh (Join-Path $Path 'third_party\gh-cli') }

# ポートの案内（MCP: A 8851 / B 8852 / C 8853 / D 8855、UI: A〜D = 8911〜8914。8856 / 8915 はドッグフードと E2E 専用）
$mcpPorts = @{ A = 8851; B = 8852; C = 8853; D = 8855 }
$uiPorts  = @{ A = 8911; B = 8912; C = 8913; D = 8914 }
if ($Lane) { Write-Host ("[wt] lane {0}: engine MCP port {1}, UI test port {2}" -f $Lane, $mcpPorts[$Lane], $uiPorts[$Lane]) }

# junction（本体に取得物があるときだけ）
foreach ($j in $junctionNames) {
    $src = Join-Path $main $j
    if (Test-Path $src) {
        $dst = Join-Path $Path $j
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        if (-not (Test-Path $dst)) { cmd /c "mklink /J `"$dst`" `"$src`"" | Out-Null; Write-Host "[wt] junction: $j -> $src" }
    }
}

# npm ci（TS のテストを worktree 単体で走らせるため）
$mcpDir = Join-Path $Path 'tools\mcp-server'
if (-not $NoNpm -and (Test-Path (Join-Path $mcpDir 'package-lock.json'))) {
    Push-Location $mcpDir
    try { npm ci --no-audit --no-fund; if ($LASTEXITCODE -ne 0) { Write-Host '[wt] npm ci failed (続行)' } } finally { Pop-Location }
}

# vcpkg のバイナリキャッシュを共通の固定パスにそろえる
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:LOCALAPPDATA 'dx12-vcpkg-cache' }
New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null

if (-not $NoConfigure) {
    $cache = Join-Path $main 'build\release\CMakeCache.txt'
    $tc = $null
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^CMAKE_TOOLCHAIN_FILE:[A-Z]+=(.+)$' | Select-Object -First 1
        if ($m) { $tc = $m.Matches[0].Groups[1].Value.Trim() }
    }
    if (-not $tc) { if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $env:USERPROFILE 'vcpkg' }; $tc = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" }
    # CMake に渡すパスは / 区切りにする(\r や \U が再パースで化けるのを避ける)。
    $tc = $tc -replace '\\', '/'
    $vcpkgInstC = $vcpkgInst -replace '\\', '/'
    $assimpC = $assimp -replace '\\', '/'

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
        cmd /c "`"$vc`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
    }
    Remove-Item Env:\VSLANG -ErrorAction SilentlyContinue
    try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
    Push-Location $Path
    try {
        $cfg = @('-S', '.', '-B', 'build\release', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_TOOLCHAIN_FILE=$tc", '-DVCPKG_TARGET_TRIPLET=x64-windows',
            "-DVCPKG_INSTALLED_DIR=$vcpkgInstC", '-DVCPKG_MANIFEST_INSTALL=OFF',
            "-DFETCHCONTENT_SOURCE_DIR_ASSIMP=$assimpC")
        $q = ($cfg | ForEach-Object { "`"$_`"" }) -join ' '
        & cmd /c "chcp 65001>nul && cmake $q"
        if ($LASTEXITCODE -ne 0) { Write-Host '[wt] configure failed'; exit 1 }
    } finally { Pop-Location }
}
Write-Host ("[wt] ready: {0} (branch {1}) in {2:n0}s. 次: cd {0}; pwsh tools/build.ps1" -f $Path, $Branch, $sw.Elapsed.TotalSeconds)
