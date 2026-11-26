<#
  D3D12 Agility SDK のランタイム（D3D12Core.dll / d3d12SDKLayers.dll）を third_party/agility へ取得する。
  cmake の configure が自動で同じことを行う（cmake/UnoAgility.cmake）ので、普段は呼ぶ必要が無い。
  configure せずに取得だけ確かめたいとき、CI のキャッシュを温めたいときに使う薄いラッパー。

    pwsh tools/fetch_agility.ps1

  - 版・SDK 番号・SHA256 の正は cmake/agility.lock.json の 1 つ（ここに数字を書かない）。
  - 再頒布ライセンスなのでリポジトリにコミットしない（.gitignore 済み）。docs/THIRD_PARTY_FETCHED.md を参照。
  - 外へ出るのは nuget.org だけ。
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cand = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $cand) { $cmake = $cand }
}
if (-not $cmake) { Write-Host '[agility] cmake が見つからない'; exit 2 }
& $cmake -DUNO_AGILITY_FETCH_ONLY=ON -P (Join-Path $root 'cmake/UnoAgility.cmake')
exit $LASTEXITCODE
