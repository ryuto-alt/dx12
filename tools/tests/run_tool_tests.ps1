<#
  道具(tools/)自身のテストを全部まとめて流す。tools/lib / tools/kit.ps1 / tools/ctest.ps1 / tools/engine_instance.ps1 / tools/briefs /
  tools/scorecard/baseline_perf.mjs / tools/mcp-server/testing/perfCore.ts を変えたら、コミット前にこれが ALL PASSED であること。
  重い処理(実ビルド・実エンジン・実 GPU)は動かさない。偽の無操作秒数・偽のスロット名・偽のエンジンだけを使う。

    pwsh tools/tests/run_tool_tests.ps1            # 全部(1〜2 分)
    pwsh tools/tests/run_tool_tests.ps1 -Quick     # 時間のかかる slot_lock_test と kit_test を除く
#>
param([switch]$Quick)
$ErrorActionPreference = 'Continue'
$tools = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $tools
$failed = @()
function Invoke-Step([string]$name, [scriptblock]$run) {
    Write-Host "=== $name"
    $global:LASTEXITCODE = 0
    & $run 2>&1 | ForEach-Object { if ("$_" -match 'FAIL|FAILED|not ok|✖') { Write-Host "$_" } }
    if ($LASTEXITCODE -ne 0) { $script:failed += $name; Write-Host "  -> FAILED ($LASTEXITCODE)" } else { Write-Host '  -> ok' }
}
$ps = @('changed_map_test.ps1', 'build_env_test.ps1', 'build_slots_test.ps1')
if (-not $Quick) { $ps += @('slot_lock_test.ps1', 'kit_test.ps1') }
foreach ($t in $ps) { Invoke-Step $t { pwsh -NoProfile -File (Join-Path $PSScriptRoot $t) } }
Push-Location $root
try {
    Invoke-Step 'node --test tools/lib tools/briefs tools/scorecard' { node --test tools/lib tools/briefs tools/scorecard/baseline_perf.test.mjs }
    Invoke-Step 'mcp-server perfGate.test.ts' { Push-Location tools/mcp-server; try { node perfGate.test.ts } finally { Pop-Location } }
    Invoke-Step 'mcp-server run_tests.mjs --self-test' { node tools/mcp-server/scripts/run_tests.mjs --self-test }
} finally { Pop-Location }
if ($failed.Count -gt 0) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host 'ALL PASSED'
