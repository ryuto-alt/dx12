<#
  tools/lib/ChangedMap.psm1 のテスト(git もビルドも要らない。合成データと、このリポジトリの UI テスト登録だけを使う)
    pwsh tools/tests/changed_map_test.ps1
#>
$ErrorActionPreference = 'Stop'
$toolsDir = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $toolsDir
Import-Module (Join-Path $toolsDir 'lib\ChangedMap.psm1') -Force
$fail = 0
function Assert-That([bool]$c, [string]$m) { if ($c) { Write-Host "  PASS: $m" } else { Write-Host "  FAIL: $m"; $script:fail++ } }
function Same($a, $b) { return ((@($a) | Sort-Object) -join ',') -eq ((@($b) | Sort-Object) -join ',') }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('dx12-changedmap-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $cmake = @'
add_executable(AlphaTests alpha_test.cpp ../src/core/Alpha.cpp)
add_test(NAME AlphaTests COMMAND AlphaTests)
add_executable(BetaTests beta_test.cpp
    beta_extra.cpp)
add_test(NAME BetaTests COMMAND BetaTests)
add_test(NAME BetaScript COMMAND pwsh -File x.ps1)
add_executable(GammaTests gamma_test.cpp EXCLUDE_FROM_ALL)
add_test(NAME GammaTests COMMAND GammaTests)
'@
    $smap = Read-DxTestSourceMap $cmake 'tests'
    Write-Host '(a) CMake parsing'
    Assert-That (Same $smap.ExeSources['AlphaTests'] @('tests/alpha_test.cpp', 'src/core/Alpha.cpp')) 'sources resolved (tests/ and ../src/)'
    Assert-That (Same $smap.ExeSources['BetaTests'] @('tests/beta_test.cpp', 'tests/beta_extra.cpp')) 'multi-line add_executable'
    Assert-That ($smap.TestExe['AlphaTests'] -eq 'AlphaTests') 'test -> exe'

    $exeA = Join-Path $tmp 'AlphaTests.exe'; Set-Content $exeA 'a'
    $tests = @(
        [pscustomobject]@{ name = 'AlphaTests'; exe = $exeA; labels = @() },
        [pscustomobject]@{ name = 'BetaTests'; exe = (Join-Path $tmp 'BetaTests.exe'); labels = @() },
        [pscustomobject]@{ name = 'GammaTests'; exe = (Join-Path $tmp 'GammaTests.exe'); labels = @() })
    $src = { param($rel) if ($rel -eq 'tests/beta_test.cpp') { '#include "core/Widget.h"' + "`n" } else { '' } }

    Write-Host '(b) ctest selection'
    $s = Get-DxCtestSelection -Changed @('tests/alpha_test.cpp') -Tests $tests -SourceMap $smap
    Assert-That (($s.Mode -eq 'subset') -and (Same $s.Names @('AlphaTests'))) 'test source -> its test'
    # 全部のテストの exe が「前回合格のまま」(署名が一致)の記録
    $stampOk = @{ AlphaTests = (Get-DxExeSig $exeA); BetaTests = ''; GammaTests = '' }
    $s = Get-DxCtestSelection -Changed @('src/core/Alpha.cpp') -Tests $tests -SourceMap $smap -Stamp $stampOk
    Assert-That (Same $s.Names @('AlphaTests')) 'src listed in add_executable -> its test (nothing else relinked)'
    $s = Get-DxCtestSelection -Changed @('src/core/Widget.h') -Tests $tests -SourceMap $smap -TestSourceText $src -Stamp $stampOk
    Assert-That (Same $s.Names @('BetaTests')) 'header included by a test source -> that test'
    # レビュー指摘 1: 直接対応するテストがあっても、リンクし直された他の exe(ライブラリ経由・推移的な include)を落とさない
    $stampBetaStale = @{ AlphaTests = (Get-DxExeSig $exeA); BetaTests = 'old'; GammaTests = '' }
    $s = Get-DxCtestSelection -Changed @('src/core/Alpha.cpp') -Tests $tests -SourceMap $smap -Stamp $stampBetaStale
    Assert-That (Same $s.Names @('AlphaTests', 'BetaTests')) "direct hit + relinked exe both selected ($($s.Names -join ','))"
    $s = Get-DxCtestSelection -Changed @('src/core/Widget.h') -Tests $tests -SourceMap $smap -TestSourceText $src -Stamp @{ AlphaTests = 'old'; BetaTests = ''; GammaTests = '' }
    Assert-That (Same $s.Names @('AlphaTests', 'BetaTests')) 'header: include hit + transitively relinked exe both selected'
    $s = Get-DxCtestSelection -Changed @('src/core/Alpha.cpp') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'direct hit but no pass record -> full'
    $s = Get-DxCtestSelection -Changed @('docs/X.md', 'tools/mcp-server/a.ts') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'none') 'docs / mcp-server only -> nothing to run'
    $s = Get-DxCtestSelection -Changed @('src/core/Orphan.cpp') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'unmapped src and no pass record -> full'
    # 前回合格の記録あり: exe の署名が違う(= リンクし直された)テストだけ
    $stamp = @{ AlphaTests = (Get-DxExeSig $exeA); BetaTests = 'old'; GammaTests = 'old' }
    $s = Get-DxCtestSelection -Changed @('src/core/Orphan.cpp') -Tests $tests -SourceMap $smap -Stamp $stamp
    Assert-That (($s.Mode -eq 'subset') -and (Same $s.Names @('BetaTests', 'GammaTests'))) "unmapped src + pass record -> relinked exes only ($($s.Names -join ','))"
    $s = Get-DxCtestSelection -Changed @('CMakeLists.txt') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'root CMakeLists -> full'
    $s = Get-DxCtestSelection -Changed @('src/render/CMakeLists.txt') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'src CMakeLists -> full'
    $s = Get-DxCtestSelection -Changed @('tests/CMakeLists.txt') -Tests $tests -SourceMap $smap -CMakeNames ([pscustomobject]@{ AddedLines = 2; Names = @('GammaTests') })
    Assert-That (($s.Mode -eq 'subset') -and (Same $s.Names @('GammaTests'))) 'tests/CMakeLists.txt -> only the test named in added lines'
    $s = Get-DxCtestSelection -Changed @('tests/CMakeLists.txt') -Tests $tests -SourceMap $smap -CMakeNames ([pscustomobject]@{ AddedLines = 1; Names = @() })
    Assert-That ($s.Mode -eq 'full') 'tests/CMakeLists.txt with unrecognised additions -> full'
    $rules = [pscustomobject]@{ rules = @([pscustomobject]@{ pattern = '^shaders/'; tests = @('^Gamma') }) }
    $s = Get-DxCtestSelection -Changed @('shaders/x.hlsl') -Tests $tests -SourceMap $smap -Rules $rules -Stamp @{ x = '1' }
    Assert-That (Same $s.Names @('GammaTests')) 'rule table maps a path to tests'

    Write-Host '(b2) script tests, unmapped paths, not-rebuilt detection'
    $pyArgs = @('tools/check_anchors.mjs', 'docs')
    $tests2 = $tests + @([pscustomobject]@{ name = 'AnchorLint'; exe = 'C:\Program Files\nodejs\node.exe'; args = $pyArgs; labels = @() })
    $s = Get-DxCtestSelection -Changed @('tools/check_anchors.mjs') -Tests $tests2 -SourceMap $smap
    Assert-That (($s.Mode -eq 'subset') -and (Same $s.Names @('AnchorLint'))) "tools script named in a test command -> that test ($($s.Mode): $($s.Names -join ','))"
    $s = Get-DxCtestSelection -Changed @('src/core/Alpha.cpp') -Tests $tests2 -SourceMap $smap -Stamp $stampOk
    Assert-That (Same $s.Names @('AlphaTests', 'AnchorLint')) 'script tests always run when something changed'
    $s = Get-DxCtestSelection -Changed @('tools/never_heard_of.py') -Tests $tests2 -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'unmapped path -> full (never an empty green)'
    $s = Get-DxCtestSelection -Changed @('Dockerfile') -Tests $tests -SourceMap $smap
    Assert-That ($s.Mode -eq 'full') 'unmapped root file -> full'
    # ビルド忘れ: 変更ファイルが最新の exe より新しい
    $repo = Join-Path $tmp 'repo'; New-Item -ItemType Directory -Force (Join-Path $repo 'src\core') | Out-Null
    Set-Content (Join-Path $repo 'src\core\Orphan.cpp') 'x'
    (Get-Item $exeA).LastWriteTimeUtc = [datetime]::UtcNow.AddMinutes(-30)
    $stampA = @{ AlphaTests = (Get-DxExeSig $exeA); BetaTests = ''; GammaTests = '' }
    $s = Get-DxCtestSelection -Changed @('src/core/Orphan.cpp') -Tests $tests -SourceMap $smap -Stamp $stampA -RepoRoot $repo
    Assert-That ($s.Mode -eq 'full') "source newer than every test exe -> full ($($s.Reasons -join ' / '))"
    (Get-Item $exeA).LastWriteTimeUtc = [datetime]::UtcNow.AddMinutes(5)
    $stampA = @{ AlphaTests = (Get-DxExeSig $exeA); BetaTests = ''; GammaTests = '' }
    $s = Get-DxCtestSelection -Changed @('src/core/Orphan.cpp') -Tests $tests -SourceMap $smap -Stamp $stampA -RepoRoot $repo
    Assert-That ($s.Mode -ne 'full') 'exe newer than the source (built after editing) -> normal selection'

    Write-Host '(c) pass stamp / junit / known_red'
    $sp = Join-Path $tmp 'stamp.json'
    Write-DxPassStamp $sp @{ A = '1:2'; B = '3:4' }
    $rd = Read-DxPassStamp $sp
    Assert-That (($rd['A'] -eq '1:2') -and ($rd['B'] -eq '3:4')) 'stamp round trip'
    $jx = Join-Path $tmp 'j.xml'
    Set-Content $jx '<testsuite><testcase name="P" status="run"/><testcase name="F" status="fail"><failure/></testcase><testcase name="S"><skipped/></testcase></testsuite>'
    $jr = Read-DxJunit $jx
    Assert-That (($jr['P'] -eq 'passed') -and ($jr['F'] -eq 'failed') -and ($jr['S'] -eq 'skipped')) 'junit parsed'
    Set-Content $jx '<testsuite><testcase name="N" status="notrun"/></testsuite>'
    Assert-That ((Read-DxJunit $jx)['N'] -eq 'notrun') 'junit: notrun is its own state (does not count as skipped)'
    $kj = Join-Path $tmp 'kr.json'
    Set-Content $kj '{"ctest":[{"name":"Old","reason":"r1","expires":"2020-01-01"},{"name":"Live","reason":"r2","expires":"2099-01-01"},{"name":"NoExp","reason":"r3"}],"ui":[{"name":"U","reason":"x"}]}'
    $kr = Get-DxKnownRed $kj 'ctest' ([datetime]'2026-11-27')
    Assert-That (($kr.Active.ContainsKey('Live')) -and ($kr.Active.ContainsKey('NoExp')) -and -not $kr.Active.ContainsKey('Old') -and ($kr.Expired -contains 'Old')) 'known_red: expired entries are not active'
    Assert-That ((Get-DxKnownRed $kj 'ui').Active.ContainsKey('U')) 'known_red: ui section'

    Write-Host '(d) UI selection (real registrations of this repo)'
    $regs = @(Read-DxUiRegistrations $root)
    Assert-That ($regs.Count -ge 20) "registrations parsed ($($regs.Count))"
    Assert-That (($regs | Where-Object { $_.name -eq 'open_all_tool_windows' }).Count -eq 1) 'open_all_tool_windows exists'
    $rules = Read-DxJsonFile (Join-Path $toolsDir 'ui_tests_map.json')
    $u = Get-DxUiSelection -Changed @('docs/a.md') -Regs $regs -Rules $rules
    Assert-That (($u.Mode -eq 'subset') -and (Same $u.Names @('open_all_tool_windows'))) 'unrelated change -> smoke only'
    $u = Get-DxUiSelection -Changed @('src/editor/panels/HierarchyPanel.cpp') -Regs $regs -Rules $rules
    Assert-That (($u.Mode -eq 'subset') -and ($u.Names -contains 'hierarchy_click_all') -and ($u.Names -contains 'open_all_tool_windows')) 'HierarchyPanel -> hierarchy tests + smoke'
    Assert-That ($u.Names.Count -lt $regs.Count) 'subset is smaller than the full set'
    $u = Get-DxUiSelection -Changed @('src/gui/UiTestHarness.cpp') -Regs $regs -Rules $rules
    Assert-That (($u.Mode -eq 'full') -and ($u.Names.Count -eq $regs.Count)) 'test harness change -> full'
    $u = Get-DxUiSelection -Changed @('src/editor/EditorTheme.h') -Regs $regs -Rules $rules
    Assert-That ($u.Mode -eq 'full') 'theme change -> full'
    $u = Get-DxUiSelection -Changed @('src/scene/SceneGraph.cpp') -Regs $regs -Rules $rules
    Assert-That (($u.Mode -eq 'full') -and ($u.Names.Count -eq $regs.Count)) 'engine source outside editor/gui (scene/renderer/...) -> full, not smoke only'
    $u = Get-DxUiSelection -Changed @('shaders/forward/Forward.hlsl') -Regs $regs -Rules $rules
    Assert-That ($u.Mode -eq 'full') 'shader change -> full'
}
finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
if ($fail -gt 0) { Write-Host "FAILED: $fail"; exit 1 }
Write-Host 'ALL PASSED'
