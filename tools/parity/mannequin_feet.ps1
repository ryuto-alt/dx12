# マネキン(NB14)の足が床に着いているかをエンジン(--headless・自分のポート 8854)で確かめる(FU-E-34)。窓は出ない・マウスに触らない。
#   pwsh -NoProfile -File tools/parity/mannequin_feet.ps1 --build-dir <build/release のあるフォルダ>
#   pwsh -NoProfile -File tools/parity/mannequin_feet.ps1 --from-dir <撮影済みフォルダ>      # 判定だけやり直す(エンジン不要)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $here '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    Write-Error "venv がありません。先に: pwsh -NoProfile -File $here\setup.ps1"
    exit 2
}
$env:PYTHONUTF8 = '1'
Push-Location $here
try {
    & $py -m parity.mannequin_feet @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
