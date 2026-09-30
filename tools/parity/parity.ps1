# パリティ基盤の入口。専用 venv の Python で `python -m parity <引数>` を実行する(終了コードもそのまま返す)。
#   pwsh -NoProfile -File tools/parity/parity.ps1 run tools/parity/scenes/smoke_generated.json --stage G1
#   終了コード: 0 = 合格 / 1 = 不合格 / 2 = エラー / 3 = skipped のみ
# venv が無ければ tools/parity/setup.ps1 を案内して 2 で終わる。
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $here '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    Write-Error "venv がありません。先に: pwsh -NoProfile -File $here\setup.ps1"
    exit 2
}
$env:PYTHONUTF8 = '1'
Push-Location $here
try {
    & $py -m parity @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
