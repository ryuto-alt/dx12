<#
  並列作業用の git worktree を作り、本体と資産を共有したまま configure まで済ませる。

    pwsh tools/worktree_setup.ps1 -Lane A                       # %USERPROFILE%\dx12-wt\lane-a、ブランチ wave-lane-a
    pwsh tools/worktree_setup.ps1 -Lane B -Branch wave1/lane-b -Base overhaul/wave0
    pwsh tools/worktree_setup.ps1 -Path %USERPROFILE%\dx12-wt\wt-selftest -Branch wt-selftest   # レーン外(使い捨て)
    pwsh tools/worktree_setup.ps1 -Path ... -Remove             # worktree を片付ける(--force)

  やっていること:
    1. git worktree add(ブランチが無ければ -Base から作る)。
    2. 本体からコピー: src/core/generated/AssetKey.h(gitignore 対象)、third_party/gh-cli(約 40MB、未追跡)。
    3. chcp 65001 の下で configure(Ninja / Release)。次を本体と共有してディスクと時間を節約する:
         -DVCPKG_INSTALLED_DIR=<本体>\build\release\vcpkg_installed  -DVCPKG_MANIFEST_INSTALL=OFF
         -DFETCHCONTENT_SOURCE_DIR_ASSIMP=<本体>\build\release\_deps\assimp-src
    4. ポートの帯を表示する(エンジン MCP: A 8851 / B 8852 / C 8853、UI テスト: 8881〜8883)。
  ビルドは configure 後に `pwsh tools/build.ps1`(PC 全体で直列化される)。本体の build\release は触らない。
#>
param(
    [ValidateSet('A', 'B', 'C', '')][string]$Lane = '',
    [string]$Path = '',
    [string]$Branch = '',
    [string]$Base = 'HEAD',   # 本体 worktree の HEAD(実行元 worktree の HEAD ではない)
    [switch]$NoConfigure,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'

# 本体(メイン worktree)のルートを求める。worktree の中から呼ばれても本体を指す。
$here = Split-Path -Parent $PSScriptRoot
$common = (git -C $here rev-parse --path-format=absolute --git-common-dir).Trim()
$main = (Split-Path -Parent $common) -replace '/', '\'

if ($Lane) {
    $Lane = $Lane.ToUpper()
    if (-not $Path)   { $Path = Join-Path (Join-Path (Split-Path -Parent $main) 'dx12-wt') "lane-$($Lane.ToLower())" }
    if (-not $Branch) { $Branch = "wave-lane-$($Lane.ToLower())" }
}
if (-not $Path) { Write-Host '[wt] -Lane か -Path を指定してください'; exit 2 }
$Path = [IO.Path]::GetFullPath($Path)
if (-not $Branch) { $Branch = Split-Path -Leaf $Path }

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
    git -C $main worktree remove --force $Path
    if ($LASTEXITCODE -ne 0) { Write-Host '[wt] git worktree remove failed'; exit 1 }
    # git 成功後に残ったフォルダ(ロック等で消えなかった分)だけ片付ける。
    if (Test-Path $Path) { Remove-Item -Recurse -Force $Path }
    git -C $main worktree prune
    Write-Host "[wt] removed $Path"
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

# ポート帯の案内
$idx = if ($Lane) { [int][char]$Lane - [int][char]'A' } else { -1 }
if ($idx -ge 0) { Write-Host ("[wt] lane {0}: engine MCP port {1}, UI test port {2}" -f $Lane, (8851 + $idx), (8881 + $idx)) }

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
