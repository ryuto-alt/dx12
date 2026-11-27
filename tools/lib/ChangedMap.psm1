<#
  「変更したファイル -> 影響するテスト」の対応づけと、known_red(既知の赤)の台帳、前回合格の記録。
  ctest.ps1 -Changed / run_ui_tests.ps1 -Auto / kit.ps1 test-changed が使う。docs/AGENT_KIT.md 参照。

  方針: 迷ったら全件(full)。ただし「項目の途中の確認」を速くするための道具で、統合とゲートは常に全件。
#>

function ConvertTo-DxSlashPath([string]$p) { return (($p -replace '\\', '/') -replace '^(\./)+', '') }

# --- 変更ファイルの一覧 -------------------------------------------------------------------
# 基準にするブランチ: 引数 > DX12_BASE_REF > overhaul/wave0 > origin/overhaul/wave0 > master > origin/master のうち最初に在るもの
function Resolve-DxBaseRef([string]$Base, [string]$Root) {
    $cands = @()
    if ($Base) { $cands += $Base }
    if ($env:DX12_BASE_REF) { $cands += $env:DX12_BASE_REF }
    $cands += 'overhaul/wave0', 'origin/overhaul/wave0', 'master', 'origin/master'
    foreach ($c in $cands) {
        & git -C $Root rev-parse --verify --quiet "$c^{commit}" 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { return $c }
    }
    return $null
}

# 基準との共通祖先からの変更(コミット済み + 作業ツリー + 未追跡)。基準が無ければ $null(= 判断できない)
function Get-DxChangedFiles([string]$Root, [string]$Base = '') {
    $ref = Resolve-DxBaseRef $Base $Root
    if (-not $ref) { return $null }
    $mb = (& git -C $Root merge-base HEAD $ref 2>$null | Select-Object -First 1)
    if (-not $mb) { return $null }
    $files = @()
    $files += @(& git -C $Root diff --name-only $mb 2>$null)
    $files += @(& git -C $Root ls-files --others --exclude-standard 2>$null)
    return [pscustomobject]@{ Base = $ref; MergeBase = $mb; Files = @($files | Where-Object { $_ } | ForEach-Object { ConvertTo-DxSlashPath $_ } | Sort-Object -Unique) }
}

# tests/CMakeLists.txt の差分(追加行)に出てくる識別子。add_test / add_executable / set_tests_properties などの最初の引数
function Get-DxCMakeAddedNames([string]$Root, [string]$MergeBase, [string]$RelPath = 'tests/CMakeLists.txt') {
    $diff = @(& git -C $Root diff -U0 $MergeBase -- $RelPath 2>$null)
    $names = New-Object System.Collections.Generic.HashSet[string]
    $added = 0
    foreach ($l in $diff) {
        if ($l -notmatch '^\+(?!\+\+)') { continue }
        $added++
        foreach ($m in [regex]::Matches($l, '(?i)\b(?:add_test\(\s*NAME|add_executable\(|set_tests_properties\(|target_\w+\(|add_dependencies\(|set_target_properties\()\s*(\w+)')) { [void]$names.Add($m.Groups[1].Value) }
    }
    return [pscustomobject]@{ AddedLines = $added; Names = @($names) }
}

# --- CMake から「ソース -> 実行ファイル -> テスト」を読む --------------------------------------
function Read-DxTestSourceMap([string]$CMakeText, [string]$CMakeRelDir = 'tests') {
    $exeSources = @{}      # target -> @(repo 相対パス)
    foreach ($m in [regex]::Matches($CMakeText, '(?is)add_executable\(\s*(\w+)\s+([^)]*)\)')) {
        $t = $m.Groups[1].Value
        $srcs = @()
        foreach ($tok in ($m.Groups[2].Value -split '\s+')) {
            if (-not $tok -or $tok -match '^(EXCLUDE_FROM_ALL|WIN32|MACOSX_BUNDLE)$' -or $tok -match '[\$#]' -or $tok -notmatch '\.(cpp|cc|cxx|c|h|hpp)$') { continue }
            $p = ConvertTo-DxSlashPath $tok
            if ($p -notmatch '^\.\./') { $p = "$CMakeRelDir/$p" } else { $p = ($p -replace '^\.\./', '') }
            $srcs += $p
        }
        if (-not $exeSources.ContainsKey($t)) { $exeSources[$t] = @() }
        $exeSources[$t] += $srcs
    }
    $testExe = @{}         # テスト名 -> target
    foreach ($m in [regex]::Matches($CMakeText, '(?is)add_test\(\s*NAME\s+(\w+)\s+COMMAND\s+(\$<TARGET_FILE:)?(\w+)')) { $testExe[$m.Groups[1].Value] = $m.Groups[3].Value }
    return [pscustomobject]@{ ExeSources = $exeSources; TestExe = $testExe }
}

# --- 前回合格の記録(exe の署名)と古い exe ------------------------------------------------
function Get-DxExeSig([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return '' }
    $i = Get-Item -LiteralPath $Path
    return "$($i.LastWriteTimeUtc.Ticks):$($i.Length)"
}
function Read-DxPassStamp([string]$Path) {
    $h = @{}
    if (Test-Path -LiteralPath $Path) {
        try { (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $h[$_.Name] = [string]$_.Value } } catch {}
    }
    return $h
}
function Write-DxPassStamp([string]$Path, [hashtable]$Stamp) {
    $o = [ordered]@{}
    foreach ($k in ($Stamp.Keys | Sort-Object)) { $o[$k] = $Stamp[$k] }
    ($o | ConvertTo-Json -Compress) | Set-Content -LiteralPath $Path -NoNewline -Encoding utf8
}

# --- 設定の表(tools/ctest_changed_map.json / tools/ui_tests_map.json) -------------------------
function Read-DxJsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { return (Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json) } catch { return $null }
}

function Test-DxScriptTest($t) {
    $leaf = [IO.Path]::GetFileName([string]$t.exe)
    return ((-not $leaf) -or ($leaf -match '^(python3?|py|node|cmake|pwsh|powershell|bash|sh|npm|npx)(\.exe|\.cmd)?$'))
}

<#
  ctest の選択。
    -Changed   変更ファイル(repo 相対・スラッシュ区切り)
    -Tests     @{ name; exe; args[]; labels[] } の配列(ctest --show-only=json-v1 から)
    -SourceMap Read-DxTestSourceMap の結果
    -Rules     ctest_changed_map.json の rules(pattern/tests/ignore)。$null 可
    -CMakeNames tests/CMakeLists.txt の追加行に出た識別子(Get-DxCMakeAddedNames)。$null = 不明
    -Stamp     前回合格の署名(Read-DxPassStamp)。空 = 記録なし
    -RepoRoot  リポジトリのルート(変更ファイルが exe より新しい=ビルド忘れ、の判定に使う)。空なら判定しない
    -TestSourceText  scriptblock { param($relPath) -> ファイル全文(include 検索用) }。$null 可
  戻り値: Mode = subset | full | none、Names、Reasons(説明の行)
#>
function Get-DxCtestSelection {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Changed,
        [Parameter(Mandatory)]$Tests,
        [Parameter(Mandatory)]$SourceMap,
        $Rules = $null,
        $CMakeNames = $null,
        [hashtable]$Stamp = @{},
        [scriptblock]$TestSourceText = $null,
        [string]$RepoRoot = ''
    )
    $reasons = New-Object System.Collections.Generic.List[string]
    $sel = New-Object System.Collections.Generic.HashSet[string]
    $allNames = @($Tests | ForEach-Object { $_.name })
    $full = { param($why) return [pscustomobject]@{ Mode = 'full'; Names = $allNames; Reasons = @($why) + $reasons.ToArray() } }
    $targetTests = @{}   # target -> テスト名[]
    foreach ($t in $allNames) {
        $exe = if ($SourceMap.TestExe.ContainsKey($t)) { $SourceMap.TestExe[$t] } else { $t }
        if (-not $targetTests.ContainsKey($exe)) { $targetTests[$exe] = @() }
        $targetTests[$exe] += $t
    }
    $srcToTargets = @{}
    foreach ($kv in $SourceMap.ExeSources.GetEnumerator()) { foreach ($s in $kv.Value) { if (-not $srcToTargets.ContainsKey($s)) { $srcToTargets[$s] = @() }; $srcToTargets[$s] += $kv.Key } }
    $addTargets = {
        param($targets, $why)
        $n = 0
        foreach ($tg in $targets) { if ($targetTests.ContainsKey($tg)) { foreach ($t in $targetTests[$tg]) { if ($sel.Add($t)) { $n++ } } } }
        if ($n -gt 0) { $reasons.Add("$why -> $n test(s)") }
    }

    $ignore = '^(docs/|.*\.(md|png|jpg|jpeg|gif|ico|svg)$|\.github/|[^/]+\.bat$|\.gitignore$|\.gitattributes$|\.editorconfig$|tools/mcp-server/|tools/briefs/|tools/lib/|tools/kit\.ps1$|tools/tests/|tools/known_red\.json$|tools/(ctest_changed_map|ui_tests_map)\.json$|LICENSE)'
    # スクリプト系のテスト(python / node / cmake など、ビルドした exe ではないもの)。安いので変更があれば常に流す
    $scriptTests = @($Tests | Where-Object { Test-DxScriptTest $_ })
    $builtTests = @($Tests | Where-Object { -not (Test-DxScriptTest $_) })
    $argText = @{}
    foreach ($t in $scriptTests) { $argText[$t.name] = ((@($t.args) -join ' ') -replace '\\', '/') }
    $argMatch = {
        param($f)
        $leaf = [IO.Path]::GetFileName($f); $hits = @()
        foreach ($t in $scriptTests) { $tx = $argText[$t.name]; if ($tx -and ($tx.Contains($f) -or ($leaf.Length -ge 5 -and $tx.Contains("/$leaf")))) { $hits += $t.name } }
        return $hits
    }
    $relinkFiles = @()
    foreach ($f in $Changed) {
        # スクリプト系テストの引数に出てくるパス(tools/check_anchors.mjs など)は、そのテストを選ぶ
        $ah = @(& $argMatch $f)
        if ($ah.Count -gt 0) { foreach ($n in $ah) { [void]$sel.Add($n) }; $reasons.Add("test command mentions $f -> $($ah.Count) test(s)") }
        if ($f -match $ignore) { continue }
        # 設定の表(ignore / tests)
        $ruleHit = $false
        if ($Rules -and $Rules.rules) {
            foreach ($r in $Rules.rules) {
                if ($f -match $r.pattern) {
                    $ruleHit = $true
                    if ($r.ignore) { continue }
                    foreach ($rx in @($r.tests)) { $n = 0; foreach ($t in $allNames) { if ($t -match $rx -and $sel.Add($t)) { $n++ } }; if ($n -gt 0) { $reasons.Add("rule '$($r.pattern)' ($f) -> $n test(s)") } }
                }
            }
        }
        if ($f -eq 'tests/CMakeLists.txt') {
            if ($null -eq $CMakeNames -or $CMakeNames.AddedLines -eq 0) { return (& $full 'tests/CMakeLists.txt changed (cannot tell which tests)') }
            $hit = 0
            foreach ($nm in $CMakeNames.Names) {
                if ($targetTests.ContainsKey($nm)) { foreach ($t in $targetTests[$nm]) { if ($sel.Add($t)) { $hit++ } } }
                elseif ($allNames -contains $nm -and $sel.Add($nm)) { $hit++ }
            }
            if ($hit -eq 0) { return (& $full 'tests/CMakeLists.txt changed (added lines name no known test)') }
            $reasons.Add("tests/CMakeLists.txt added lines -> $hit test(s)")
            continue
        }
        if ($f -match '(^|/)CMakeLists\.txt$|^cmake/|^CMakePresets\.json$|^vcpkg\.json$|\.cmake$|^third_party/') { return (& $full "build config changed: $f") }
        $isBuildInput = ($f -match '^(src|include|shaders|assets)/')
        if ($srcToTargets.ContainsKey($f)) { & $addTargets $srcToTargets[$f] "source $f" }
        if ($f -match '^tests/') {
            if ($srcToTargets.ContainsKey($f)) { continue }
            # スクリプト類のテスト(add_test に名前がある)は名前の一致で拾う
            $stem = [IO.Path]::GetFileNameWithoutExtension($f) -replace '[_\-]', ''
            $nameHit = $false
            foreach ($t in $allNames) { if (($t -replace '[_\-]', '') -ieq $stem) { [void]$sel.Add($t); $nameHit = $true; $reasons.Add("test script $f -> $t") } }
            if (-not $nameHit -and -not $ruleHit -and $ah.Count -eq 0) { return (& $full "tests file maps to no test: $f") }
            continue
        }
        if ($isBuildInput) {
            # テストのソースが basename を include していれば拾う(それだけでは足りない: 下の「リンクし直された exe」も常に足す)
            $base = [IO.Path]::GetFileName($f)
            $hit = 0
            if ($TestSourceText -and $f -match '\.(h|hpp|cpp|inc|hlsl|hlsli)$') {
                $rx = '(?m)^\s*#\s*include\s*[<"][^>"]*' + [regex]::Escape($base) + '[>"]'
                foreach ($kv in $SourceMap.ExeSources.GetEnumerator()) {
                    $found = $false
                    foreach ($s in $kv.Value) { $txt = & $TestSourceText $s; if ($txt -and $txt -match $rx) { $found = $true; break } }
                    if ($found -and $targetTests.ContainsKey($kv.Key)) { foreach ($t in $targetTests[$kv.Key]) { if ($sel.Add($t)) { $hit++ } } }
                }
            }
            if ($hit -gt 0) { $reasons.Add("tests include $base -> $hit test(s)") }
            if (-not $ruleHit) { $relinkFiles += $f }
            continue
        }
        # tools/ やルート直下など、どこにも対応しないもの: 迷ったら全件(「何も選ばず緑」にしない)
        if (-not $ruleHit -and $ah.Count -eq 0) { return (& $full "no mapping for: $f") }
    }
    if ($relinkFiles.Count -gt 0) {
        # ビルドを回していないと exe が古いままで、リンクし直しも起きない: 変更が最新の exe より新しければ全件にする
        $newestExe = [datetime]::MinValue; $exeCount = 0
        foreach ($t in $builtTests) {
            if ($t.exe -and (Test-Path -LiteralPath $t.exe)) { $exeCount++; $w = (Get-Item -LiteralPath $t.exe).LastWriteTimeUtc; if ($w -gt $newestExe) { $newestExe = $w } }
        }
        if ($RepoRoot) {
            if ($exeCount -eq 0) { return (& $full 'test executables are not built (run tools/build.ps1 -Tests first)') }
            foreach ($f in $relinkFiles) {
                $p = Join-Path $RepoRoot $f
                if ((Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).LastWriteTimeUtc -gt $newestExe) { return (& $full "tests not rebuilt after editing $f (run tools/build.ps1 -Tests, then -Changed)") }
            }
        }
        # ninja が実際にリンクし直した exe(前回合格の署名と違うもの)。直接対応するテストがあっても、他の exe に推移的に入ることがあるので常に足す
        if ($Stamp.Count -eq 0) { return (& $full "no pass record yet and $($relinkFiles.Count) changed build input(s) (e.g. $($relinkFiles[0]))") }
        $stale = 0
        foreach ($t in $builtTests) {
            $sig = Get-DxExeSig $t.exe
            if (-not $Stamp.ContainsKey($t.name) -or $Stamp[$t.name] -ne $sig) { if ($sel.Add($t.name)) { $stale++ } }
        }
        $reasons.Add("exe changed since last pass (relinked or new) -> $stale test(s); $($relinkFiles.Count) build input(s)")
    }
    if ($Changed.Count -gt 0 -and $scriptTests.Count -gt 0) {
        $n = 0; foreach ($t in $scriptTests) { if ($sel.Add($t.name)) { $n++ } }
        if ($n -gt 0) { $reasons.Add("script/python/node tests always run on any change -> $n test(s)") }
    }
    $names = @($allNames | Where-Object { $sel.Contains($_) })
    return [pscustomobject]@{ Mode = $(if ($names.Count -gt 0) { 'subset' } else { 'none' }); Names = $names; Reasons = $reasons.ToArray() }
}

# --- known_red -----------------------------------------------------------------------------
# tools/known_red.json: { "ctest":[{name,reason,lane,expires:"YYYY-MM-DD"}], "ui":[...] }。期限切れは「既知」として扱わない
function Get-DxKnownRed([string]$Path, [string]$Section = 'ctest', [datetime]$Now = (Get-Date)) {
    $j = Read-DxJsonFile $Path
    $res = @{ Active = @{}; Expired = @() }
    if (-not $j -or -not $j.$Section) { return $res }
    foreach ($e in @($j.$Section)) {
        if (-not $e.name) { continue }
        $exp = $null
        if ($e.expires) { try { $exp = [datetime]::Parse($e.expires).Date.AddDays(1) } catch {} }
        if ($exp -and $Now -ge $exp) { $res.Expired += $e.name } else { $res.Active[$e.name] = [string]$e.reason }
    }
    return $res
}

# ctest の JUnit XML(--output-junit)から 名前 -> passed|failed|skipped|notrun
function Read-DxJunit([string]$Path) {
    $r = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path)) { return $r }
    try {
        $x = [xml](Get-Content -LiteralPath $Path -Raw -Encoding utf8)
        foreach ($tc in $x.SelectNodes('//testcase')) {
            $st = 'passed'
            if ($tc.SelectSingleNode('failure') -or $tc.SelectSingleNode('error')) { $st = 'failed' }
            elseif ($tc.status -eq 'notrun') { $st = 'notrun' }
            elseif ($tc.SelectSingleNode('skipped')) { $st = 'skipped' }
            $r[$tc.name] = $st
        }
    } catch {}
    return $r
}

# --- UI テストの選択 ---------------------------------------------------------------------------
# UiTestHarness.cpp / uitests/*.cpp の kTests 登録行 { "category", "name", "group", "display", T_Body, ... } を読む
function Read-DxUiRegistrations([string]$Root) {
    $files = @(Join-Path $Root 'src\gui\UiTestHarness.cpp') + @(Get-ChildItem -Path (Join-Path $Root 'src\gui\uitests') -Filter *.cpp -ErrorAction SilentlyContinue | ForEach-Object FullName)
    $regs = @()
    $texts = @{}
    foreach ($f in $files) { if (Test-Path $f) { $texts[$f] = Get-Content -LiteralPath $f -Raw -Encoding utf8 } }
    foreach ($kv in $texts.GetEnumerator()) {
        foreach ($m in [regex]::Matches($kv.Value, '(?m)^\s*\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*(\w+)')) {
            $regs += [pscustomobject]@{ category = $m.Groups[1].Value; name = $m.Groups[2].Value; body = $m.Groups[3].Value; file = $kv.Key }
        }
    }
    # 本体のテキスト(関数定義から次の行頭の } まで)
    $all = ($texts.Values -join "`n")
    foreach ($r in $regs) {
        $mm = [regex]::Match($all, '(?ms)^(?:static\s+)?void\s+' + [regex]::Escape($r.body) + '\s*\(.*?^\}')
        $r | Add-Member -NotePropertyName text -NotePropertyValue $(if ($mm.Success) { $mm.Value } else { '' })
    }
    return $regs
}

<#
  UI テストの選択(run_ui_tests.ps1 -Auto)。
  Mode: subset(Names を -Only に)| full(迷ったら全件)。変更が UI と無関係でも、最低 1 本(smoke)は流す。
#>
function Get-DxUiSelection {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Changed, [Parameter(Mandatory)]$Regs, $Rules = $null)
    $reasons = New-Object System.Collections.Generic.List[string]
    $sel = New-Object System.Collections.Generic.HashSet[string]
    $all = @($Regs | ForEach-Object name)
    $smoke = @('open_all_tool_windows')
    if ($Rules -and $Rules.smoke) { $smoke = @($Rules.smoke) }
    $fullPat = '^src/gui/(UiTestHarness|uitests/|ImGuiManager)|^src/editor/(EditorTheme|EditorLayer|EditorContext|ui/)|^src/core/(Application\.|ApplicationInternal)|CMakeLists\.txt$|^cmake/|^assets/editor/'
    if ($Rules -and $Rules.fullPattern) { $fullPat = $Rules.fullPattern }
    foreach ($f in $Changed) {
        if ($f -match $fullPat) { return [pscustomobject]@{ Mode = 'full'; Names = $all; Reasons = @("shared UI infrastructure changed: $f") } }
        $handled = $false
        if ($Rules -and $Rules.rules) {
            foreach ($r in $Rules.rules) {
                if ($f -match $r.pattern) {
                    $handled = $true
                    $n = 0
                    foreach ($x in @($r.only)) { foreach ($nm in $all) { if ($nm -eq $x -and $sel.Add($nm)) { $n++ } } }
                    foreach ($c in @($r.categories)) { foreach ($rg in $Regs) { if ($rg.category -eq $c -and $sel.Add($rg.name)) { $n++ } } }
                    if ($n -gt 0) { $reasons.Add("rule '$($r.pattern)' ($f) -> $n test(s)") }
                }
            }
        }
        if ($f -match '^src/(editor|gui)/(.+/)?([A-Za-z0-9_]+)\.(cpp|h|hpp|inc)$') {
            $handled = $true
            $stem = $Matches[3] -replace 'Panel$|Logic$|Window$', ''
            if ($stem.Length -lt 3) { continue }
            $flat = ($stem -replace '[_\-]', '').ToLowerInvariant()
            $n = 0
            foreach ($rg in $Regs) {
                $nameFlat = ($rg.name -replace '[_\-]', '').ToLowerInvariant()
                if ($nameFlat.Contains($flat) -or ($rg.text -and $rg.text.Contains($stem))) { if ($sel.Add($rg.name)) { $n++ } }
            }
            if ($n -gt 0) { $reasons.Add("UI source $f -> $n test(s) by name/body match") }
            else { foreach ($rg in $Regs) { if ($rg.category -eq 'panel' -and $sel.Add($rg.name)) { } }; $reasons.Add("UI source ${f}: no direct match -> category 'panel'") }
        }
        # editor/gui の外のエンジン本体(scene / renderer / ecs / core ...)や shaders・assets は、ヒエラルキー・生成・Undo・インスペクタなどの UI テストを壊しうる。
        # 対応が読めないものは「smoke だけ」にせず全件にする
        if (-not $handled -and $f -match '^(src|include|shaders|assets)/') { return [pscustomobject]@{ Mode = 'full'; Names = $all; Reasons = @("engine source outside editor/gui changed, UI impact unknown: $f") } }
    }
    foreach ($s in $smoke) { if ($all -contains $s) { [void]$sel.Add($s) } }
    $names = @($all | Where-Object { $sel.Contains($_) })
    if ($names.Count -eq 0) { return [pscustomobject]@{ Mode = 'full'; Names = $all; Reasons = @('no test names resolved') } }
    return [pscustomobject]@{ Mode = 'subset'; Names = $names; Reasons = $reasons.ToArray() }
}

Export-ModuleMember -Function Resolve-DxBaseRef, Get-DxChangedFiles, Get-DxCMakeAddedNames, Read-DxTestSourceMap, Get-DxExeSig, Read-DxPassStamp,
    Write-DxPassStamp, Read-DxJsonFile, Get-DxCtestSelection, Get-DxKnownRed, Read-DxJunit, Read-DxUiRegistrations, Get-DxUiSelection
