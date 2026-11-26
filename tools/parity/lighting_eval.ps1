# ライティング評価ハーネス(LS-00b)の入口。専用 venv の Python で `python -m parity.lighting_eval <引数>` を実行する(終了コードもそのまま返す)。
#   pwsh -NoProfile -File tools/parity/lighting_eval.ps1 --build-dir <buildelease のあるフォルダ>     # 全シーン(LS-A は High と Epic の 2 条件)
#   pwsh -NoProfile -File tools/parity/lighting_eval.ps1 --scenes LS-A --conditions epic
#   pwsh -NoProfile -File tools/parity/lighting_eval.ps1 --from-dir .dx12\parity\lighting\<日時>      # 撮影済みから指標だけ再計算
#   pwsh -NoProfile -File tools/parity/lighting_eval.ps1 --compare a\lighting_eval.json b\lighting_eval.json   # 2 回の差が決定論の範囲か
# 出力: <repo>/.dx12/parity/lighting/<日時>/(git 管理外)。エンジンは engine_instance.ps1 で背景起動(name=lighteval・既定 port 8854)して終わりに必ず停止する。窓は出ない。
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $here '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    Write-Error "venv がありません。先に: pwsh -NoProfile -File $here\setup.ps1"
    exit 2
}
$env:PYTHONUTF8 = '1'
Push-Location $here
try {
    & $py -m parity.lighting_eval @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
