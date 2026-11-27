<#
  ctest のラッパー。昼に GPU テストや UI テストを走らせてしまわないための既定を持つ。

    pwsh tools/ctest.ps1                       # 既定: gpu と ui のラベルを除外して実行
    pwsh tools/ctest.ps1 -R Anchor             # 名前の正規表現（ctest の -R をそのまま渡す）
    pwsh tools/ctest.ps1 -Changed              # 基準ブランチからの変更ファイルに関係するテストだけ(下記)
    pwsh tools/ctest.ps1 -Changed -Base master # 基準を指定(既定: DX12_BASE_REF > overhaul/wave0 > master)
    pwsh tools/ctest.ps1 -Changed -Plan        # 実行せず、選んだテストと理由だけを JSON で出す
    pwsh tools/ctest.ps1 -Gpu                  # gpu ラベルも含める（オーナーの離席中か、メインの指示があるときだけ）
    pwsh tools/ctest.ps1 -Ui                   # ui ラベルも含める（UiTests はエンジンを起動する。同上）
    pwsh tools/ctest.ps1 -N                    # 実行せず一覧だけ
    pwsh tools/ctest.ps1 -StrictRed            # known_red.json を無視する(統合・ゲートの全件確認はこれ)
    pwsh tools/ctest.ps1 -Dir build\tracy      # 別ビルドディレクトリ

  やっていること:
    1. VS 同梱の ctest.exe を自動で探す（PATH の ctest は使わない）。
    2. プロセス優先度を BelowNormal にする（子プロセスに継承される）。
    3. -j3 --timeout 300 --output-on-failure。利用者が離席中(DX12_BUILD_IDLE_MIN 分以上無操作)なら -j6（-Jobs 指定で上書き）。
    4. 既定で -LE "^(gpu|ui)$"（ラベルは小文字。-Gpu で gpu、-Ui で ui を除外から外す）。
    5. テストの枠(Global\dx12-test。昼 1 本・離席中 最大 3 本。枠 2 以降は Idle 優先度)を取ってから走らせる。
       -Gpu / -Ui のときは先に GPU の枠(Global\dx12-gpu)も取る(順序は常に gpu -> test)。DX12_SLOTS_DISABLE=1 で従来どおり。
       tools/lib/SlotLock.psm1 / docs/AGENT_KIT.md の「枠」の節を参照。
    6. known_red(tools/known_red.json。名前・理由・期限つきの既知の赤)に載っているテストだけが赤のときは終了コード 0。
       新しい赤(載っていない・期限切れ)があれば非 0。-StrictRed で無効。
    7. 終わりに「N 件中 M 件合格、新しい赤、既知の赤」の要約を出し、合格したテストの exe の署名を
       <Dir>\.ctest-pass-stamp.json に記録する(-Changed が「リンクし直された exe」を見分けるのに使う)。

  -Changed の選び方(tools/lib/ChangedMap.psm1。迷ったら全件):
    - 変更ファイル = 基準との共通祖先からの差分 + 作業ツリー + 未追跡。docs / *.md / mcp-server などは無視。
    - tests/<x>.cpp、CMake の add_executable に載っている src/*.cpp -> その exe のテスト。
    - src/ の他のファイルは、テストのソースがそのファイル名を #include していれば拾う。
    - それでも対応が無い src/ の変更は「前回合格のあとにリンクし直された exe」(要ビルド)で補う。合格の記録が無ければ全件。
    - tests/CMakeLists.txt は追加行に出てきたテスト名だけ。名前が読めなければ全件。他の CMakeLists / cmake/ / vcpkg.json は全件。
    - tools/ctest_changed_map.json の rules(パターン -> テスト名の正規表現)で足し引きできる。
    統合とゲートは -Changed を使わず全件のまま。
  ビルドはしない。先に pwsh tools/build.ps1 -Tests で建てておくこと。
#>
param(
    [string]$R = '',
    [switch]$Gpu,
    [switch]$Ui,
    [switch]$N,
    [switch]$Changed,
    [string]$Base = '',
    [switch]$Plan,
    [switch]$StrictRed,
    [string]$KnownRed = '',
    [string]$Dir = 'build\release',
    [int]$Jobs = 3,
    [int]$Timeout = 300,
    [int]$SlotWaitMinutes = -1,  # -1 = 既定: DX12_SLOT_WAIT_MIN > 8 分(エージェントの前景ツールは 10 分で切れる。取れなければ終了コード 3 + retry の案内)
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib\ChangedMap.psm1') -Force

# --- 1. ctest.exe を探す（VS 同梱）
$ctest = $null
$candidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
)
foreach ($c in $candidates) { if (Test-Path $c) { $ctest = $c; break } }
if (-not $ctest) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vs) {
            $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
            if (Test-Path $p) { $ctest = $p }
        }
    }
}
if (-not $ctest) {
    $cmd = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($cmd) { $ctest = $cmd.Source }
}
if (-not $ctest) { Write-Host '[ctest] ctest.exe が見つかりません'; exit 2 }

$buildDir = if ([IO.Path]::IsPathRooted($Dir)) { $Dir } else { Join-Path $root $Dir }
if (-not (Test-Path (Join-Path $buildDir 'CTestTestfile.cmake'))) {
    Write-Host "[ctest] CTestTestfile.cmake がありません: $buildDir（先に configure / build.ps1 -Tests）"
    exit 2
}

# --- 3/4. 除外ラベル
$exclude = @()
if (-not $Gpu) { $exclude += 'gpu' }
if (-not $Ui)  { $exclude += 'ui' }

# --- テスト一覧(名前・exe・ラベル)。-N 以外では常に取る(-Changed の選択と合格の記録に使う)
function Get-TestList {
    $json = & $ctest --test-dir $buildDir --show-only=json-v1 2>$null | Out-String
    $j = $json | ConvertFrom-Json
    $list = @()
    foreach ($t in $j.tests) {
        $labels = @()
        foreach ($p in @($t.properties)) { if ($p.name -eq 'LABELS') { $labels = @($p.value) } }
        $exe = if ($t.command -and $t.command.Count -gt 0) { [string]$t.command[0] } else { '' }
        $targs = if ($t.command -and $t.command.Count -gt 1) { @($t.command | Select-Object -Skip 1 | ForEach-Object { [string]$_ }) } else { @() }
        $list += [pscustomobject]@{ name = $t.name; exe = $exe; args = $targs; labels = $labels }
    }
    return $list
}
$tests = @()
if (-not $N) {
    try { $tests = @(Get-TestList) } catch { Write-Host "[ctest] テスト一覧を読めませんでした($($_.Exception.Message))。-Changed は全件にします"; $tests = @() }
}
$eligible = @($tests | Where-Object { $t = $_; -not ($t.labels | Where-Object { $exclude -contains $_ }) })

# --- -Changed: 選択
$selectedNames = $null
$selMode = 'all'
if ($Changed -and $tests.Count -gt 0) {
    $ch = Get-DxChangedFiles $root $Base
    if (-not $ch) {
        Write-Host '[ctest] -Changed: 基準ブランチが見つからない(-Base か DX12_BASE_REF で指定)。全件にします'
    } else {
        $cmakePath = Join-Path $root 'tests\CMakeLists.txt'
        $cmakeText = if (Test-Path $cmakePath) { Get-Content -LiteralPath $cmakePath -Raw -Encoding utf8 } else { '' }
        $smap = Read-DxTestSourceMap $cmakeText 'tests'
        $rules = Read-DxJsonFile (Join-Path $PSScriptRoot 'ctest_changed_map.json')
        $cmNames = $null
        if ($ch.Files -contains 'tests/CMakeLists.txt') { $cmNames = Get-DxCMakeAddedNames $root $ch.MergeBase }
        $stamp = Read-DxPassStamp (Join-Path $buildDir '.ctest-pass-stamp.json')
        $srcText = { param($rel) $p = Join-Path $root $rel; if (Test-Path -LiteralPath $p) { Get-Content -LiteralPath $p -Raw -Encoding utf8 } else { '' } }
        $sel = Get-DxCtestSelection -Changed $ch.Files -Tests $eligible -SourceMap $smap -Rules $rules -CMakeNames $cmNames -Stamp $stamp -TestSourceText $srcText -RepoRoot $root
        $selMode = $sel.Mode
        Write-Host ("[ctest] -Changed vs {0} ({1} files): mode={2}, {3}/{4} tests" -f $ch.Base, $ch.Files.Count, $sel.Mode, $sel.Names.Count, $eligible.Count)
        foreach ($r in ($sel.Reasons | Select-Object -First 12)) { Write-Host "         - $r" }
        if ($sel.Mode -ne 'full') { $selectedNames = @($sel.Names) }
        if ($Plan) {
            @{ base = $ch.Base; mode = $sel.Mode; tests = $sel.Names; reasons = $sel.Reasons; files = $ch.Files } | ConvertTo-Json -Depth 4 -Compress
            exit 0
        }
        if ($sel.Mode -eq 'none') { Write-Host '[ctest] 影響するテストはありません(0 件)。終了コード 0'; exit 0 }
    }
}
if ($Plan -and -not $selectedNames) { @{ mode = $selMode; tests = @($eligible | ForEach-Object name) } | ConvertTo-Json -Compress; exit 0 }

$args2 = @('--test-dir', $buildDir, '--timeout', "$Timeout", '--output-on-failure')
if ($exclude.Count -gt 0) { $args2 += @('-LE', ('^(' + ($exclude -join '|') + ')$')) }
$rx = $R
if ($selectedNames) {
    $alt = ($selectedNames | Where-Object { $_ -match $(if ($R) { $R } else { '.' }) } | ForEach-Object { [regex]::Escape($_) }) -join '|'
    if (-not $alt) { Write-Host '[ctest] -Changed と -R の両方に合うテストはありません'; exit 0 }
    $rx = '^(' + $alt + ')$'
}
if ($rx) { $args2 += @('-R', $rx) }
if ($N) { $args2 += '-N' }
if ($Rest) { $args2 += $Rest }

if ($N) {
    Write-Host "[ctest] $ctest $($args2 -join ' ')"
    & $ctest @args2
    exit $LASTEXITCODE
}

# --- 5. 枠(gpu -> test の順)
if ($SlotWaitMinutes -lt 0) { $SlotWaitMinutes = Get-DxSlotDefaultWaitMin }
$gpuSlot = $null; $testSlot = $null
try {
    if ($Gpu -or $Ui) {
        $gpuSlot = Enter-DxSlot -Kind gpu -Label 'ctest -Gpu/-Ui' -WaitMinutes $SlotWaitMinutes
        if (-not $gpuSlot) { Write-Host '[ctest] retry: GPU の枠が空かなかった。しばらくしてからこのコマンドをもう一度(終了コード 3)'; exit 3 }
    }
    $testSlot = Enter-DxSlot -Kind test -Label 'ctest' -WaitMinutes $SlotWaitMinutes
    if (-not $testSlot) { Write-Host '[ctest] retry: テストの枠が空かなかった。しばらくしてからこのコマンドをもう一度(終了コード 3)'; exit 3 }
    # --- 2. 優先度(枠 2 以降は Idle)
    try { (Get-Process -Id $PID).PriorityClass = $(if ($testSlot.Priority -eq 'Idle' -or ($gpuSlot -and $gpuSlot.Priority -eq 'Idle')) { 'Idle' } else { 'BelowNormal' }) } catch {}
    $jobs2 = $Jobs
    # -j6 は枠 2 以降(離席中に取れた分。Idle 優先度)だけ。枠 1 は -j3 のまま(枠 3 本 x -j6 の同時過多と、戻ってきた後に -j6 のまま残るのを避ける)
    if (-not $PSBoundParameters.ContainsKey('Jobs') -and $testSlot.Slot -ge 2) { $jobs2 = 6 }
    $args2 = @('-j', "$jobs2") + $args2
    $junit = Join-Path ([IO.Path]::GetTempPath()) ("dx12-ctest-" + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.xml')
    $args2 += @('--output-junit', $junit)

    Write-Host "[ctest] $ctest $($args2 -join ' ')"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $ctest @args2
    $code = $LASTEXITCODE

    # --- 6/7. 結果の集計
    $res = Read-DxJunit $junit
    Remove-Item -LiteralPath $junit -Force -ErrorAction SilentlyContinue
    $red = @($res.Keys | Where-Object { $res[$_] -eq 'failed' })
    $passed = @($res.Keys | Where-Object { $res[$_] -eq 'passed' })
    $skipped = @($res.Keys | Where-Object { $res[$_] -eq 'skipped' })
    $notRun = @($res.Keys | Where-Object { $res[$_] -eq 'notrun' })
    $kr = if ($StrictRed -or $env:DX12_KNOWN_RED_DISABLE -eq '1') { @{ Active = @{}; Expired = @() } }
          else { Get-DxKnownRed $(if ($KnownRed) { $KnownRed } else { Join-Path $PSScriptRoot 'known_red.json' }) 'ctest' }
    $newRed = @($red | Where-Object { -not $kr.Active.ContainsKey($_) })
    $knownRedNames = @($red | Where-Object { $kr.Active.ContainsKey($_) })
    $fixed = @($kr.Active.Keys | Where-Object { $res.Contains($_) -and $res[$_] -eq 'passed' })
    if ($res.Count -gt 0) {
        Write-Host ("[ctest] summary: {0} run, {1} passed, {2} skipped, {3} NEW RED, {4} known red, {5:n0}s" -f $res.Count, $passed.Count, $skipped.Count, $newRed.Count, $knownRedNames.Count, $sw.Elapsed.TotalSeconds)
        foreach ($tn in $newRed) { Write-Host "  NEW RED: $tn" }
        foreach ($tn in $notRun) { Write-Host "  NOT RUN (exe missing?): $tn" }
        foreach ($tn in $knownRedNames) { Write-Host "  known red: $tn ($($kr.Active[$tn]))" }
        foreach ($tn in $kr.Expired) { if ($res.Contains($tn) -and $res[$tn] -eq 'failed') { Write-Host "  EXPIRED known red (counts as new): $tn" } }
        foreach ($tn in $fixed) { Write-Host "  now passing - remove from tools/known_red.json: $tn" }
        # 合格の記録(exe の署名)
        $stampPath = Join-Path $buildDir '.ctest-pass-stamp.json'
        $stamp = Read-DxPassStamp $stampPath
        foreach ($t in $tests) { if ($res.Contains($t.name)) { if ($res[$t.name] -eq 'passed') { $stamp[$t.name] = Get-DxExeSig $t.exe } else { $stamp.Remove($t.name) } } }
        try { Write-DxPassStamp $stampPath $stamp } catch {}
        if ($code -ne 0 -and $newRed.Count -eq 0 -and $knownRedNames.Count -gt 0 -and $red.Count -eq $knownRedNames.Count -and $notRun.Count -eq 0) {
            Write-Host '[ctest] 赤はすべて known_red に載っている既知のものだけ -> 終了コード 0'
            $code = 0
        }
    }
    exit $code
}
finally {
    Exit-DxSlot $testSlot
    Exit-DxSlot $gpuSlot
}
