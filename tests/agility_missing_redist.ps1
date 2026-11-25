<#
  AgilityMissingRedist（夜間）: D3D12\ を抜いた一時コピーを --headless で動かし、
  「in-box に縮退して動く」か「決まった終了コード 0x2A」のどちらかで終わる（クラッシュしない）ことを確かめる。
    pwsh tests/agility_missing_redist.ps1 -ExeDir build\release -McpPort 8851
  -McpPort は自分のレーンのポートを渡す（他のエンジンと衝突させない。未指定は環境変数 UNO_LANE_MCP_PORT、無ければ 8859）。
  窓は出さない（--headless + --background）。クラッシュ時の WER ダイアログも出さない（SetErrorMode を子に継承）。
#>
param(
    [Parameter(Mandatory)][string]$ExeDir,
    [int]$McpPort = 0
)
$ErrorActionPreference = 'Stop'
if ($McpPort -le 0) { $McpPort = if ($env:UNO_LANE_MCP_PORT) { [int]$env:UNO_LANE_MCP_PORT } else { 8859 } }

Add-Type -Namespace Uno -Name ErrMode -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint m);'
# SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX（子プロセスへ継承される）
[void][Uno.ErrMode]::SetErrorMode(0x8003)

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("agility_missing_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp, (Join-Path $tmp 'data'), (Join-Path $tmp 'proj') | Out-Null
$p = $null
try {
    # D3D12\ 以外（exe、直下の dll、shaders、assets）を運ぶ。D3D12\ は運ばない。
    robocopy $ExeDir $tmp DX12Engine.exe *.dll /NFL /NDL /NJH /NJS /NP | Out-Null
    foreach ($d in 'shaders', 'assets') {
        if (Test-Path (Join-Path $ExeDir $d)) { robocopy (Join-Path $ExeDir $d) (Join-Path $tmp $d) /E /NFL /NDL /NJH /NJS /NP | Out-Null }
    }
    if (Test-Path (Join-Path $tmp 'D3D12')) { Write-Host 'FAIL: D3D12 が残っている'; exit 1 }

    $env:DX12E_DATA_DIR = Join-Path $tmp 'data'
    $env:DX12E_NO_SPLASH = '1'
    # --idle-exit は分単位。待ち時間（WaitForExit）はそれより十分長くして競合させない。
    $p = Start-Process -FilePath (Join-Path $tmp 'DX12Engine.exe') `
        -ArgumentList @('--headless', '--project', (Join-Path $tmp 'proj'), '--background=hidden,notool', '--mcp-port', "$McpPort", '--idle-exit', '1') `
        -WorkingDirectory $tmp -WindowStyle Hidden -PassThru
    $exited = $p.WaitForExit(150000)
    $logs = Get-ChildItem $tmp -Recurse -Filter '*.log' -ErrorAction SilentlyContinue
    $text = ($logs | ForEach-Object { Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue }) -join "`n"
    if (-not $exited) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        if ($text -match 'D3D12 runtime:') { Write-Host 'OK: in-box のランタイムに縮退して動作（ログに D3D12 runtime）'; exit 0 }
        Write-Host 'FAIL: 時間内に終わらず、D3D12 runtime のログも無い'; exit 1
    }
    $code = $p.ExitCode
    if ($code -eq 0x2A) { Write-Host 'OK: 決まった終了コード 0x2A'; exit 0 }
    if ($code -eq 0) { Write-Host 'OK: 正常終了（in-box に縮退）'; exit 0 }
    ($text -split "`n" | Select-Object -Last 15) | ForEach-Object { Write-Host "  log: $_" }
    Write-Host ("FAIL: 想定外の終了コード 0x{0:X}（クラッシュ）" -f $code); exit 1
}
finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
