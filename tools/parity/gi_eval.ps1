# GI 評価ハーネス(S1)の入口。専用 venv の Python で `python -m parity.gi_eval <引数>` を実行する(終了コードもそのまま返す)。
#   pwsh -NoProfile -File tools/parity/gi_eval.ps1                            # 全シーン × legacy / ddgi_current / gi_new
#   pwsh -NoProfile -File tools/parity/gi_eval.ps1 --scenes gr1 --configs legacy,ddgi_current
#   pwsh -NoProfile -File tools/parity/gi_eval.ps1 --from-dir .dx12\parity\gi\<日時>     # 撮影済みから指標だけ再計算
# 出力: <repo>/.dx12/parity/gi/<日時>/(git 管理外)。エンジンは engine_instance.ps1 で背景起動(name=gi・port 8821)して終わりに必ず停止する。
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $here '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    Write-Error "venv がありません。先に: pwsh -NoProfile -File $here\setup.ps1"
    exit 2
}
$env:PYTHONUTF8 = '1'
Push-Location $here
try {
    & $py -m parity.gi_eval @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
