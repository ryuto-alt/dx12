<#
  ゲート(§2.10 / DG1 G11)用の cmake 構成。Python の依存が無くて「スキップ = 走らなかったのに緑」になる ctest を失敗扱いにする。

    pwsh tools/gate_configure.ps1                 # build\release を -DUNO_GATE=ON で構成し直す（ビルドはしない）
    pwsh tools/gate_configure.ps1 -Dir build\x    # 別のビルドディレクトリ
    pwsh tools/gate_configure.ps1 -CheckOnly      # 依存と python の確認だけ（cmake は呼ばない）

  やっていること:
    1. 依存入りの python を決める: -Python → tools/parity/.venv（無ければ tools/parity/setup.ps1 で作る）。
    2. その python で VfxBakePyTests(numpy,pytest) / SplashAvPyTests(numpy,PIL) / ParityHarnessPyTests(scipy,skimage,flip_evaluator,...) の依存を確かめる。足りなければ失敗。
    3. cmake -DUNO_GATE=ON -DSPLASH_AV_REQUIRED=ON -DVFX_BAKE_REQUIRED=ON -DPARITY_REQUIRED=ON と各 *_PYTHON を渡して構成し直す（キャッシュに残るので、以後の tools\build.ps1 -Tests も同じ構成）。
  ゲートの ctest が終わったら tools/gate_skip_audit.ps1 でスキップ 0 を確かめる。
#>
param(
    [string]$Dir = 'build\release',
    [string]$Python = '',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$venvPy = Join-Path $root 'tools\parity\.venv\Scripts\python.exe'
if (-not $Python) {
    if (-not (Test-Path $venvPy)) {
        Write-Host '[gate] tools/parity/.venv が無いので tools/parity/setup.ps1 で作る'
        & pwsh -NoProfile -File (Join-Path $root 'tools\parity\setup.ps1')
        if ($LASTEXITCODE -ne 0) { Write-Host '[gate] setup.ps1 が失敗'; exit 1 }
    }
    $Python = $venvPy
}
if (-not (Test-Path $Python)) { Write-Host "[gate] python が無い: $Python"; exit 1 }
$checks = @(
    @{ test = 'VfxBakePyTests';       mods = 'numpy, pytest' },
    @{ test = 'SplashAvPyTests';      mods = 'numpy, PIL' },
    @{ test = 'ParityHarnessPyTests'; mods = 'numpy, scipy, PIL, skimage, flip_evaluator, jsonschema, pytest' }
)
$bad = 0
foreach ($c in $checks) {
    & $Python -c "import $($c.mods)" 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "[gate] OK   $($c.test): $($c.mods)" }
    else { Write-Host "[gate] NG   $($c.test): $($c.mods) が $Python に無い（pwsh tools/parity/setup.ps1 で入る）"; $bad++ }
}
if ($bad -gt 0) { exit 1 }
if ($CheckOnly) { exit 0 }
$cfg = @('-S', $root, '-B', (Join-Path $root $Dir), '-DUNO_GATE=ON',
         '-DSPLASH_AV_REQUIRED=ON', '-DVFX_BAKE_REQUIRED=ON', '-DPARITY_REQUIRED=ON',
         "-DSPLASH_AV_PYTHON=$Python", "-DVFX_BAKE_PYTHON=$Python", "-DPARITY_PYTHON=$Python")
Write-Host "[gate] cmake $($cfg -join ' ')"
& cmd /c "chcp 65001>nul && cmake $(($cfg | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')"
if ($LASTEXITCODE -ne 0) { Write-Host "[gate] configure が失敗 (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Host '[gate] OK。続けて tools\build.ps1 -Tests → tools\ctest.ps1 -Gpu -Ui。終わったら tools\gate_skip_audit.ps1'
