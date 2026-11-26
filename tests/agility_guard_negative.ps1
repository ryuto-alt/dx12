<#
  AgilityGuardNegative: AgilityTestGuard が実際に落とすことを確かめる（守りが一度も働かないまま緑、を防ぐ）。
    pwsh tests/agility_guard_negative.ps1 -ExeDir build\release\tests
  1. D3D12\ を抜いた一時コピーで AgilityLoadTests.exe を動かすと、起動時に終了コード 98 で落ちる。
  2. 親から UNO_DEBUG_LAYER_FAILED=1 を継承しても、D3D12\ がある通常の実行は 97 にならない（静的初期化で消す）。
  デバイスは 1 では作らない。2 は GPU を使う（gpu ラベル）。窓は出さない。
#>
param([Parameter(Mandatory)][string]$ExeDir)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace Uno -Name ErrMode2 -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint m);'
[void][Uno.ErrMode2]::SetErrorMode(0x8003)

$exe = Join-Path $ExeDir 'AgilityLoadTests.exe'
if (-not (Test-Path $exe)) { Write-Host "FAIL: $exe が無い"; exit 1 }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("agility_guard_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    # D3D12\ 以外（exe と直下の dll）を運ぶ。
    robocopy $ExeDir $tmp AgilityLoadTests.exe *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
    if (Test-Path (Join-Path $tmp 'D3D12')) { Write-Host 'FAIL: D3D12 が残っている'; exit 1 }
    $p = Start-Process -FilePath (Join-Path $tmp 'AgilityLoadTests.exe') -WorkingDirectory $tmp -WindowStyle Hidden -PassThru
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; Write-Host 'FAIL: D3D12 無しの実行が終わらない'; exit 1 }
    if ($p.ExitCode -ne 98) { Write-Host ("FAIL: D3D12 無しの終了コードが 98 ではない: 0x{0:X}" -f $p.ExitCode); exit 1 }
    Write-Host 'OK: D3D12 無しは起動時に 98 で落ちる'

    $env:UNO_DEBUG_LAYER_FAILED = '1'
    try {
        $q = Start-Process -FilePath $exe -WorkingDirectory $ExeDir -WindowStyle Hidden -PassThru
        if (-not $q.WaitForExit(100000)) { Stop-Process -Id $q.Id -Force; Write-Host 'FAIL: 継承した変数ありの実行が終わらない'; exit 1 }
        if ($q.ExitCode -eq 97) { Write-Host 'FAIL: 継承した UNO_DEBUG_LAYER_FAILED で 97 になった（消えていない）'; exit 1 }
        if ($q.ExitCode -ne 0) { Write-Host ("FAIL: 通常の実行が失敗: 0x{0:X}" -f $q.ExitCode); exit 1 }
        Write-Host 'OK: 継承した UNO_DEBUG_LAYER_FAILED は無視される'
    } finally { Remove-Item Env:\UNO_DEBUG_LAYER_FAILED -ErrorAction SilentlyContinue }
    exit 0
}
finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
