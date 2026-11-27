<#
  ゲートの ctest の出力から「スキップされたテスト」を拾い、許可リスト以外にあれば失敗にする（走らなかったのに緑、を通さない）。

    pwsh tools/ctest.ps1 -Gpu -Ui *>&1 | Tee-Object gate_ctest.log
    pwsh tools/gate_skip_audit.ps1 -Log gate_ctest.log
    pwsh tools/gate_skip_audit.ps1 -Log gate_ctest.log -Allow DredReportTests    # 環境で許す名前（既定は空）

  ctest の 'Test #N: <name> ...***Skipped' と末尾の 'The following tests did not run:' の '<N> - <name> (Skipped)' の両方を見る。
  Python 系（VfxBakePyTests / SplashAvPyTests / ParityHarnessPyTests）は許可に入れない。tools/gate_configure.ps1 で構成すれば、依存が無いと失敗になりスキップにならない。
#>
param(
    [Parameter(Mandatory)][string]$Log,
    [string[]]$Allow = @()
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Log)) { Write-Host "[skip-audit] ログが無い: $Log"; exit 2 }
$text = Get-Content -Raw -Encoding UTF8 $Log
$names = [System.Collections.Generic.HashSet[string]]::new()
foreach ($m in [regex]::Matches($text, 'Test\s+#\d+:\s+(\S+)\s+\.+\s*\*+Skipped')) { [void]$names.Add($m.Groups[1].Value) }
foreach ($m in [regex]::Matches($text, '(?m)^\s*\d+\s+-\s+(\S+)\s+\(Skipped\)')) { [void]$names.Add($m.Groups[1].Value) }
$Allow = @($Allow | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # -File 経由だとカンマ区切りが 1 文字列で来る
$bad = @($names | Where-Object { $Allow -notcontains $_ } | Sort-Object)
Write-Host "[skip-audit] skipped=$($names.Count) allowed=$($names.Count - $bad.Count) unexpected=$($bad.Count)"
foreach ($n in $bad) { Write-Host "[skip-audit] 許可されていないスキップ: $n" }
if ($bad.Count -gt 0) { exit 1 }
exit 0
