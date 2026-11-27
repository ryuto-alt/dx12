<#
  枠を取ってからコマンドを実行するラッパー(npm test などをテストの枠に載せる)。

    pwsh tools/with_slot.ps1 -Kind test npm test                  # tools/mcp-server で: 先に cd してから呼ぶ
    pwsh tools/with_slot.ps1 -Kind test -Label mcp-ts node --test tools/bench
    pwsh tools/with_slot.ps1 -Kind gpu node tools/bench/golden.mjs --only a,b   # golden.mjs は自分でも枠を取る(入れ子は通る)

  枠が取れなければ終了コード 3。コマンドの終了コードをそのまま返す。DX12_SLOTS_DISABLE=1 なら枠なしで実行。
  スロット 2 以降(離席中)で取れたときは、コマンドを Idle 優先度で走らせる。
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory)][ValidateSet('gpu', 'test')][string]$Kind,
    [string]$Label = '',
    [int]$WaitMinutes = 40,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CmdLine
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
if (-not $CmdLine -or $CmdLine.Count -eq 0) { Write-Host '[with_slot] コマンドがありません'; exit 2 }
if ($CmdLine[0] -eq '--') { $CmdLine = $CmdLine[1..($CmdLine.Count - 1)] }
if (-not $Label) { $Label = $CmdLine[0] }
$h = Enter-DxSlot -Kind $Kind -Label $Label -WaitMinutes $WaitMinutes
if (-not $h) { exit 3 }
try {
    Set-DxSlotPriority $h
    & $CmdLine[0] @($CmdLine | Select-Object -Skip 1)
    exit $LASTEXITCODE
} finally { Exit-DxSlot $h }
