<#
  フォワード系シェーダの「DXIL 同値」検証ツール（マテリアルグラフ G2a）。
  シェーダのリファクタ（ForwardShade.hlsli への尾部外出し等）の前後で、対象シェーダを全部 DXC でコンパイルし、
  DXIL の逆アセンブリを正規化して比較する。命令列が同一ならバイナリの意味も同一＝絵が 1 ビットも変わらない。

  使い方（ゴールデンの DXIL は生成物なので git に入れない。出力先は任意のフォルダ）:
    # 1) 変更前のツリーで撮る（git stash / 別ワークツリー / 変更前の shaders フォルダのコピーを -ShaderRoot に渡す）
    pwsh tools/shader_dxil_equiv.ps1 -Snapshot $env:TEMP\dxil_before -ShaderRoot C:\path\to\shaders_before
    # 2) 変更後のツリー（既定は <repo>\shaders）で撮る
    pwsh tools/shader_dxil_equiv.ps1 -Snapshot $env:TEMP\dxil_after
    # 3) 比較
    pwsh tools/shader_dxil_equiv.ps1 -Compare $env:TEMP\dxil_before,$env:TEMP\dxil_after

  比較は 3 段階で判定する（終了コード）:
    0 = 全対象が「同一」: 正規化後の命令列が 1 行も違わない。
    3 = 「順序のみ」を含む: 命令の多重集合（%N の番号だけ潰した行の集合）は同一だが、並び順（phi の並びなど）が違う。
        値は変わらない（同じ演算を同じ入力で行う）が、機械的には同一と言えないので、スクショのビット一致で裏を取ること。
    1 = 「差あり」: 命令が増減した / 定数が違う。中身を精査する。
   77 = DXC が無い（ctest の SKIP_RETURN_CODE 用）。

  正規化の内容: shader hash の行を落とす / インライン展開で付く内部定数名の接尾辞（@tint.i.0.hca → @tint.0.hca）を落とす /
  末尾の declare 群を並べ替える。cbuffer レイアウトやリソース束縛の並びは削らない（同一であるべきなので）。
#>
param(
    [string]$Snapshot = '',
    [string[]]$Compare = @(),
    [string]$ShaderRoot = '',
    [string]$Dxc = '',
    [switch]$ListTargets
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$repo = Split-Path -Parent $PSScriptRoot
if (-not $ShaderRoot) { $ShaderRoot = Join-Path $repo 'shaders' }

# 対象: 名前, 相対パス, エントリ, プロファイル, 追加の -D
# ★フォワードの尾部を持つ 3 本（Forward / ForwardSkinned / Terrain）と、その VS・派生バリアント。
#   ForwardInstanced / ForwardGrid は尾部を持たないが「触っていない」ことの確認用に含める。
$targets = @(
    @('Forward_VS',              'forward/Forward.hlsl',          'VSMain', 'vs_6_0', @()),
    @('Forward_PS',              'forward/Forward.hlsl',          'PSMain', 'ps_6_0', @()),
    @('ForwardLdr_PS',           'forward/Forward.hlsl',          'PSMain', 'ps_6_0', @('LDR_OUTPUT=1')),
    @('ForwardMask_PS',          'forward/Forward.hlsl',          'PSMain', 'ps_6_0', @('ALPHA_TEST=1')),
    @('ForwardSkinned_VS',       'forward/ForwardSkinned.hlsl',   'VSMain', 'vs_6_0', @()),
    @('ForwardSkinned_PS',       'forward/ForwardSkinned.hlsl',   'PSMain', 'ps_6_0', @()),
    @('ForwardSkinnedMask_PS',   'forward/ForwardSkinned.hlsl',   'PSMain', 'ps_6_0', @('ALPHA_TEST=1')),
    @('Terrain_VS',              'forward/Terrain.hlsl',          'VSMain', 'vs_6_0', @()),
    @('Terrain_PS',              'forward/Terrain.hlsl',          'PSMain', 'ps_6_0', @()),
    @('ForwardInstanced_VS',     'forward/ForwardInstanced.hlsl', 'VSMain', 'vs_6_0', @()),
    @('ForwardGrid_VS',          'forward/ForwardGrid.hlsl',      'VSMain', 'vs_6_0', @()),
    @('ForwardGrid_PS',          'forward/ForwardGrid.hlsl',      'PSMain', 'ps_6_0', @())
)

if ($ListTargets) { $targets | ForEach-Object { '{0}  {1}  {2}  {3}  {4}' -f $_[0], $_[1], $_[2], $_[3], ($_[4] -join ' ') }; exit 0 }

function Find-Dxc {
    if ($Dxc -and (Test-Path $Dxc)) { return $Dxc }
    if ($env:DXC_EXECUTABLE -and (Test-Path $env:DXC_EXECUTABLE)) { return $env:DXC_EXECUTABLE }
    $cache = Join-Path $repo 'build\release\CMakeCache.txt'
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^DXC_EXECUTABLE:FILEPATH=(.+)$' | Select-Object -First 1
        if ($m -and (Test-Path $m.Matches[0].Groups[1].Value)) { return $m.Matches[0].Groups[1].Value }
    }
    $c = Get-ChildItem (Join-Path $repo 'build') -Recurse -Filter dxc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.FullName }
    $c = Get-Command dxc.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

# 逆アセンブリを正規化する（内容は上のヘッダ参照）。
function Normalize-Dxil([string[]]$lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $decls = New-Object System.Collections.Generic.List[string]
    $declSlot = -1
    $pendingAttr = $null
    foreach ($raw in $lines) {
        $l = $raw.TrimEnd()
        if ($l -match '^; shader hash:') { continue }
        if ($l.Contains('@')) { $l = $l -replace '\.i(?=\.\d)', '' }        # インライン展開で付く .i を落とす
        if ($l -match '^; Function Attrs:') { $pendingAttr = $l; continue }
        if ($l -match '^declare ') {
            if ($declSlot -lt 0) { $declSlot = $out.Count }
            $decls.Add(($(if ($pendingAttr) { $pendingAttr } else { '' }) + "`n" + $l)); $pendingAttr = $null; continue
        }
        if ($pendingAttr) { $out.Add($pendingAttr); $pendingAttr = $null }
        if ($l -eq '' -and $declSlot -ge 0 -and $decls.Count -gt 0 -and $out.Count -eq $declSlot) { continue }
        $out.Add($l)
    }
    if ($declSlot -ge 0) {
        $sorted = @($decls | Sort-Object)
        $ins = New-Object System.Collections.Generic.List[string]
        foreach ($d in $sorted) { foreach ($x in ($d -split "`n")) { if ($x -ne '') { $ins.Add($x) } }; $ins.Add('') }
        $out.InsertRange($declSlot, $ins)
    }
    return $out
}

# 関数本体の命令を「%N の番号だけ潰した行」の多重集合にする。順序の差と内容の差を見分けるための二次比較。
# ★定数（レジスタ番号・float 定数）は潰さない＝別の定数を読んでいれば差として出る。
function Get-InstrMultiset([string[]]$lines) {
    $h = @{}
    $inFn = $false
    foreach ($l in $lines) {
        if ($l -match '^define ') { $inFn = $true; continue }
        if ($inFn -and $l -match '^}') { $inFn = $false; continue }
        if (-not $inFn) { continue }
        $t = $l.Trim()
        if ($t -eq '' -or $t -match '^<label>:' -or $t.StartsWith(';')) { continue }
        $t = $t -replace '\s*;\s.*$', ''                                    # 行末コメント
        $t = $t -replace '^%\d+ = ', ''
        $t = $t -replace '%\d+', '%v' -replace '!\d+', '!m'
        if ($h.ContainsKey($t)) { $h[$t]++ } else { $h[$t] = 1 }
    }
    return $h
}

function Compare-Multiset($a, $b) {
    $d = @()
    $keys = @($a.Keys) + @($b.Keys) | Sort-Object -Unique
    foreach ($k in $keys) { $x = [int]$a[$k]; $y = [int]$b[$k]; if ($x -ne $y) { $d += ('    {0} -> {1} : {2}' -f $x, $y, $k) } }
    return $d
}

if ($Snapshot) {
    $dxc = Find-Dxc
    if (-not $dxc) { Write-Host 'SKIP: dxc.exe が見つかりません'; exit 77 }
    New-Item -ItemType Directory -Force $Snapshot | Out-Null
    $tmp = Join-Path $Snapshot '_tmp'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $manifest = @{}
    $fail = 0
    foreach ($t in $targets) {
        $name = $t[0]; $src = Join-Path $ShaderRoot $t[1]
        if (-not (Test-Path $src)) { Write-Host "MISSING: $src"; $fail++; continue }
        $asm = Join-Path $tmp "$name.txt"
        $dargs = @('-T', $t[3], '-E', $t[2], '-Fc', $asm, '-Fo', (Join-Path $tmp "$name.cso"))
        foreach ($d in $t[4]) { $dargs += @('-D', $d) }
        $dargs += $src
        $log = & $dxc @dargs 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAIL: $name"; $log | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }; $fail++; continue }
        $lines = Get-Content $asm
        $hash = ($lines | Where-Object { $_ -match '^; shader hash:' } | Select-Object -First 1) -replace '^; shader hash:\s*', ''
        Set-Content -Path (Join-Path $Snapshot "$name.dxil.txt") -Value (Normalize-Dxil $lines) -Encoding utf8
        $manifest[$name] = @{ hash = $hash; bytes = (Get-Item (Join-Path $tmp "$name.cso")).Length }
        Write-Host ('{0,-24} hash={1}  cso={2}B' -f $name, $hash, $manifest[$name].bytes)
    }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    $manifest | ConvertTo-Json | Set-Content (Join-Path $Snapshot 'manifest.json') -Encoding utf8
    if ($fail -gt 0) { Write-Host "失敗 $fail 件"; exit 1 }
    Write-Host "OK: $($targets.Count) 本 -> $Snapshot"
    exit 0
}

# pwsh -File 経由だと 'a,b' が 1 本の文字列で来るので割る
if ($Compare.Count -eq 1 -and $Compare[0].Contains(',')) { $Compare = $Compare[0].Split(',') }
if ($Compare.Count -eq 2) {
    $a = $Compare[0]; $b = $Compare[1]
    $ma = Get-Content (Join-Path $a 'manifest.json') -Raw | ConvertFrom-Json
    $mb = Get-Content (Join-Path $b 'manifest.json') -Raw | ConvertFrom-Json
    $nSame = 0; $nOrder = 0; $nDiff = 0
    foreach ($t in $targets) {
        $name = $t[0]
        $la = Join-Path $a "$name.dxil.txt"; $lb = Join-Path $b "$name.dxil.txt"
        if (-not (Test-Path $la) -or -not (Test-Path $lb)) { Write-Host ('{0,-24} 片方に無い' -f $name); $nDiff++; continue }
        $ta = @(Get-Content $la); $tb = @(Get-Content $lb)
        $sameText = ($ta.Count -eq $tb.Count) -and -not (Compare-Object $ta $tb -SyncWindow 0 | Select-Object -First 1)
        $rawHash = ($ma.$name.hash -eq $mb.$name.hash)
        if ($sameText) {
            $nSame++
            Write-Host ('{0,-24} 同一（正規化後の命令列が一致{1}）' -f $name, $(if ($rawHash) { '・ハッシュも一致' } else { '。ハッシュ差は内部定数名などの表記のみ' }))
            continue
        }
        $md = Compare-Multiset (Get-InstrMultiset $ta) (Get-InstrMultiset $tb)
        if ($md.Count -eq 0) {
            $nOrder++
            $nd = @(Compare-Object $ta $tb -SyncWindow 0).Count
            Write-Host ('{0,-24} 順序のみ: 命令の多重集合は同一 / 並びが違う行 {1}（phi の並び等）' -f $name, $nd)
        } else {
            $nDiff++
            Write-Host ('{0,-24} 差あり: 行数 {1} -> {2}' -f $name, $ta.Count, $tb.Count)
            $md | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
        }
    }
    Write-Host ("合計 {0} 本: 同一 {1} / 順序のみ {2} / 差あり {3}" -f $targets.Count, $nSame, $nOrder, $nDiff)
    if ($nDiff -gt 0) { exit 1 }
    if ($nOrder -gt 0) { exit 3 }
    exit 0
}

Write-Host 'usage: -Snapshot <dir> [-ShaderRoot <shaders>] | -Compare <dirA>,<dirB> | -ListTargets'
exit 2
