<#
  巨大ベンチ素材（設計書 §5.2 vg_bench_50m の素材）を作る: 手続きメッシュ 4 種（blob / knot / rock / torus）x 既定 1250 万三角形 = 合計 5000 万。
  決定的（同じ引数なら同じバイト列）。出力は既定で build\vg-bench\（.gitignore 済み。コミットしないこと）。

    pwsh tools/vgeo_cook/gen_bench.ps1                       # 4 種 x 12.5M tri を cook して build\vg-bench\bench_<kind>_12m.vgeo
    pwsh tools/vgeo_cook/gen_bench.ps1 -Tris 1000000         # 小さい版（動作確認用）
    pwsh tools/vgeo_cook/gen_bench.ps1 -Threads 4 -Vgsrc     # VGSRC も残す（P6 の突き合わせ / 別ツール用）

  ★PC を占有しない: vgeo_cook は既定で BelowNormal・論理コア/4 スレッド。作業メモリは 12.5M tri で約 0.8 GB。
  事前に `pwsh tools/build.ps1 -Target vgeo_cook` でビルドしておく。
#>
param(
    [long]$Tris = 12500000,
    [string]$OutDir = 'build\vg-bench',
    [int]$Threads = 0,
    [string[]]$Kinds = @('blob', 'knot', 'rock', 'torus'),
    [switch]$Vgsrc
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root 'build\release\tools\vgeo_cook\vgeo_cook.exe'
if (-not (Test-Path $exe)) { throw "vgeo_cook.exe not found: $exe (run: pwsh tools/build.ps1 -Target vgeo_cook)" }
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force $out | Out-Null
$label = if ($Tris -ge 1000000) { '{0}m' -f [math]::Round($Tris / 1000000.0, 1) } else { '{0}k' -f [math]::Round($Tris / 1000.0) }
$seed = 1
foreach ($k in $Kinds) {
    $vgeo = Join-Path $out "bench_${k}_$label.vgeo"
    $common = @('--gen-bench', $k, '--tris', $Tris, '--seed', $seed, '--mem-limit-gb', 8)
    if ($Threads -gt 0) { $common += @('--threads', $Threads) }
    if ($Vgsrc) { & $exe @common --out (Join-Path $out "bench_${k}_$label.vgsrc") }
    & $exe @common --cook $vgeo
    if ($LASTEXITCODE -ne 0) { throw "vgeo_cook failed for $k (exit $LASTEXITCODE)" }
    $seed++
}
Write-Host "done: $out"
