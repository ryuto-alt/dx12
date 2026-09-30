# tools/parity の専用 venv を作って依存を入れる(何度実行しても安全)。
#   pwsh -NoProfile -File tools/parity/setup.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$venv = Join-Path $here '.venv'
$py = Join-Path $venv 'Scripts\python.exe'
if (-not (Test-Path $py)) {
    & python -m venv $venv
    if ($LASTEXITCODE -ne 0) { Write-Error 'python -m venv に失敗しました(Python 3.12 が PATH に必要)'; exit 1 }
}
& $py -m pip install --disable-pip-version-check -q -r (Join-Path $here 'requirements.txt')
if ($LASTEXITCODE -ne 0) { Write-Error 'pip install に失敗しました(ネットワークを確認)'; exit 1 }
Push-Location $here
try { & $py -m parity doctor } finally { Pop-Location }
