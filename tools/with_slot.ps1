<#
  枠を取ってからコマンドを実行するラッパー(npm test などをテストの枠に載せる)。

    pwsh tools/with_slot.ps1 -Kind test npm test                  # tools/mcp-server で: 先に cd してから呼ぶ
    pwsh tools/with_slot.ps1 -Kind test -Label mcp-ts node --test tools/bench
    pwsh tools/with_slot.ps1 -Kind gpu node tools/bench/golden.mjs --only a,b   # golden.mjs は自分でも枠を取る(入れ子は通る)

  枠が取れなければ終了コード 3(待ちの既定は 8 分。DX12_SLOT_WAIT_MIN / -WaitMinutes)。順序は gpu -> test で、-Kind test から ctest -Gpu/-Ui を呼ぶときは先に gpu も取る。コマンドの終了コードをそのまま返す。DX12_SLOTS_DISABLE=1 なら枠なしで実行。
  スロット 2 以降(離席中)で取れたときは、コマンドを Idle 優先度で走らせる。
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory)][ValidateSet('gpu', 'test')][string]$Kind,
    [string]$Label = '',
    [int]$WaitMinutes = -1,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CmdLine
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\SlotLock.psm1') -Force
if (-not $CmdLine -or $CmdLine.Count -eq 0) { Write-Host '[with_slot] コマンドがありません'; exit 2 }
if ($CmdLine[0] -eq '--') { $CmdLine = $CmdLine[1..($CmdLine.Count - 1)] }
if (-not $Label) { $Label = $CmdLine[0] }
# 順序は gpu -> test。-Kind test で ctest の -Gpu/-Ui(中で gpu を取る)を包むなら、先に gpu を取っておく
$pre = $null
if ($Kind -eq 'test' -and (($CmdLine -join ' ') -match '(?i)ctest(\.ps1)?\b.*\s-(Gpu|Ui)\b')) {
    $pre = Enter-DxSlot -Kind gpu -Label "$Label (gpu first)" -WaitMinutes $WaitMinutes
    if (-not $pre) { Write-Host '[with_slot] retry: GPU の枠が空かなかった(終了コード 3)'; exit 3 }
}
$h = Enter-DxSlot -Kind $Kind -Label $Label -WaitMinutes $WaitMinutes
if (-not $h) { Exit-DxSlot $pre; Write-Host '[with_slot] retry: 枠が空かなかった。あとでもう一度(終了コード 3)'; exit 3 }
try {
    Set-DxSlotPriority $h
    & $CmdLine[0] @($CmdLine | Select-Object -Skip 1)
    exit $LASTEXITCODE
} finally { Exit-DxSlot $h; Exit-DxSlot $pre }
