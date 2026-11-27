<#
  tools/build.ps1 の固定オーバーヘッド(中央値 6.4 秒/ビルド)を削る部品。
    1. vcvars64 の環境のキャッシュ(毎回 `cmd /c vcvars64.bat && set` を読み直さない。2〜4 秒)
    2. `ninja -t deps` の「#deps 0」検査を、.ninja_deps が前回の検査から変わったときだけにする(全件を前後 2 回 = 1〜2 秒)

  キャッシュの置き場: %LOCALAPPDATA%\dx12-build-cache\vcvars-<キー>.json(DX12_BUILD_CACHE_DIR で変更可)
  キー = vcvars64.bat のパス + その更新時刻 + VS の MSVC 既定バージョンファイル(Microsoft.VCToolsVersion.default.txt)の内容と更新時刻。
         VS を更新すればキーが変わり、自動で作り直す。DX12_BUILD_NO_VCVARS_CACHE=1 でキャッシュを使わない。
  保存するもの: vcvars が「変えた/足した」環境変数だけ。PATH は足されたエントリだけを持ち、適用時に今の PATH の前へ足す
                (呼び出し元の PATH を壊さない)。VCToolsInstallDir が無ければ古いキャッシュとみなして作り直す。
#>

function Get-DxBuildCacheDir {
    if ($env:DX12_BUILD_CACHE_DIR) { return $env:DX12_BUILD_CACHE_DIR }
    $base = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { [IO.Path]::GetTempPath() }
    return (Join-Path $base 'dx12-build-cache')
}

function Get-VcvarsCacheKey([string]$VcPath) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append($VcPath.ToLowerInvariant()).Append('|')
    try { [void]$sb.Append((Get-Item -LiteralPath $VcPath).LastWriteTimeUtc.Ticks).Append('|') } catch {}
    $verFile = Join-Path (Split-Path -Parent $VcPath) 'Microsoft.VCToolsVersion.default.txt'
    if (Test-Path -LiteralPath $verFile) {
        try { [void]$sb.Append((Get-Item -LiteralPath $verFile).LastWriteTimeUtc.Ticks).Append('|').Append((Get-Content -LiteralPath $verFile -Raw).Trim()) } catch {}
    }
    return [BitConverter]::ToString([Security.Cryptography.SHA1]::HashData([Text.Encoding]::UTF8.GetBytes($sb.ToString()))).Replace('-', '').ToLowerInvariant().Substring(0, 16)
}

# 今の環境変数(名前 -> 値)
function Get-DxEnvSnapshot {
    $h = @{}
    foreach ($e in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $h[[string]$e.Key] = [string]$e.Value }
    return $h
}

# `set` の出力行から、ベースラインとの差分(変更/追加された変数。PATH は足されたエントリ)を作る
# 呼び出し元がすでに vcvars の一部(VS の cmake/ninja、Windows Kits、INCLUDE など)を持っていると、差分だけでは足りなくなる。
# そこで「差分」に加えて、vcvars 由来と分かる変数(名前で判定)と PATH エントリ(VS のルート / Windows Kits / SDK 配下)は、ベースラインにあっても必ず保存する。
$script:VcvarsVarPattern = '^(INCLUDE|LIB|LIBPATH|EXTERNAL_INCLUDE|VC.*|VS.*|__VS.*|VSCMD.*|Windows.*|UCRT.*|UniversalCRT.*|DevEnvDir|ExtensionSdkDir|Platform|Framework.*|NETFX.*|CommandPromptType|__DOTNET.*)$'
function ConvertTo-VcvarsDelta([string[]]$SetLines, [hashtable]$Baseline, [string]$VcPath = '') {
    $vars = [ordered]@{}
    $pathAdd = @()
    $roots = @('\windows kits\', '\microsoft sdks\', '\.net framework\')
    if ($VcPath) { try { $roots += ((Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $VcPath)))).TrimEnd('\') + '\').ToLowerInvariant() } catch {} }
    foreach ($line in $SetLines) {
        if ($line -notmatch '^([^=]+)=(.*)$') { continue }
        $n = $Matches[1]; $v = $Matches[2]
        $old = $null
        foreach ($k in $Baseline.Keys) { if ($k -ieq $n) { $old = $Baseline[$k]; break } }
        if ($n -ieq 'PATH') {
            $oldEntries = @(($old -split ';') | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\').ToLowerInvariant() })
            foreach ($p in ($v -split ';')) {
                if (-not $p) { continue }
                $pl = $p.TrimEnd('\').ToLowerInvariant() + '\'
                $isVc = $false; foreach ($rt in $roots) { if ($pl.Contains($rt)) { $isVc = $true; break } }
                if ($isVc -or ($oldEntries -notcontains $p.TrimEnd('\').ToLowerInvariant())) { if ($pathAdd -notcontains $p) { $pathAdd += $p } }
            }
        }
        elseif ($null -eq $old -or $old -ne $v -or $n -match $script:VcvarsVarPattern) { $vars[$n] = $v }
    }
    return [pscustomobject]@{ vars = $vars; pathAdd = $pathAdd }
}

function Set-VcvarsDelta($Delta) {
    foreach ($p in $Delta.vars.GetEnumerator()) { [Environment]::SetEnvironmentVariable($p.Key, [string]$p.Value) }
    if ($Delta.pathAdd -and @($Delta.pathAdd).Count -gt 0) {
        $cur = [Environment]::GetEnvironmentVariable('PATH')
        $curEntries = @(($cur -split ';') | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\').ToLowerInvariant() })
        $add = @($Delta.pathAdd | Where-Object { $curEntries -notcontains $_.TrimEnd('\').ToLowerInvariant() })
        [Environment]::SetEnvironmentVariable('PATH', ((@($add) + @($cur)) -join ';'))
    }
}

<#
  vcvars64 の環境をこのプロセスへ取り込む。戻り値: @{ Hit = キャッシュから; Seconds; Key }
  -Runner は テスト用: scriptblock { param($vcPath) -> `set` の出力行 } を差し替える(既定は cmd /c "vcvars64.bat && set")
#>
function Import-VcvarsEnv {
    param([Parameter(Mandatory)][string]$VcPath, [string]$CacheDir = '', [scriptblock]$Runner = $null)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if (-not $CacheDir) { $CacheDir = Get-DxBuildCacheDir }
    $useCache = ($env:DX12_BUILD_NO_VCVARS_CACHE -ne '1')
    $key = Get-VcvarsCacheKey $VcPath
    $file = Join-Path $CacheDir "vcvars-$key.json"
    if ($useCache -and (Test-Path -LiteralPath $file)) {
        try {
            $j = Get-Content -LiteralPath $file -Raw -Encoding utf8 | ConvertFrom-Json
            $vars = [ordered]@{}
            foreach ($p in $j.vars.PSObject.Properties) { $vars[$p.Name] = [string]$p.Value }
            $tools = $vars['VCToolsInstallDir']
            if ($j.key -eq $key -and $tools -and (Test-Path -LiteralPath $tools)) {
                Set-VcvarsDelta ([pscustomobject]@{ vars = $vars; pathAdd = @($j.pathAdd) })
                return [pscustomobject]@{ Hit = $true; Seconds = $sw.Elapsed.TotalSeconds; Key = $key }
            }
        } catch {}
    }
    $baseline = Get-DxEnvSnapshot
    $lines = if ($Runner) { @(& $Runner $VcPath) } else { @(& cmd /c "`"$VcPath`" >nul && set") }
    $delta = ConvertTo-VcvarsDelta $lines $baseline $VcPath
    Set-VcvarsDelta $delta
    if ($useCache -and $delta.vars.Contains('VCToolsInstallDir')) {
        try {
            New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
            $tmp = "$file.$PID.tmp"
            ([ordered]@{ key = $key; vcvars = $VcPath; created = (Get-Date).ToString('o'); vars = $delta.vars; pathAdd = @($delta.pathAdd) } | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $tmp -Encoding utf8
            Move-Item -LiteralPath $tmp -Destination $file -Force
        } catch {}
    }
    return [pscustomobject]@{ Hit = $false; Seconds = $sw.Elapsed.TotalSeconds; Key = $key }
}

# --- #deps 0 検査の「前回から変わったか」判定 ----------------------------------------------------
# .ninja_deps の更新時刻+サイズ。無ければ ''
function Get-NinjaDepsSig([string]$BuildDirFull) {
    $f = Join-Path $BuildDirFull '.ninja_deps'
    if (-not (Test-Path -LiteralPath $f)) { return '' }
    $i = Get-Item -LiteralPath $f
    return "$($i.LastWriteTimeUtc.Ticks):$($i.Length)"
}
function Read-DepsCheckStamp([string]$BuildDirFull) {
    $f = Join-Path $BuildDirFull '.deps0-stamp'
    if (Test-Path -LiteralPath $f) { try { return (Get-Content -LiteralPath $f -Raw).Trim() } catch {} }
    return ''
}
function Write-DepsCheckStamp([string]$BuildDirFull, [string]$Sig) {
    if (-not $Sig) { return }
    try { Set-Content -LiteralPath (Join-Path $BuildDirFull '.deps0-stamp') -Value $Sig -NoNewline } catch {}
}
# 検査が要るか: .ninja_deps が無い(=記録なし)なら不要、前回「異常 0 件」と確認した署名と同じなら不要、それ以外は要る
function Test-DepsCheckNeeded([string]$BuildDirFull) {
    $sig = Get-NinjaDepsSig $BuildDirFull
    if (-not $sig) { return $false }
    return ($sig -ne (Read-DepsCheckStamp $BuildDirFull))
}

Export-ModuleMember -Function Get-DxBuildCacheDir, Get-VcvarsCacheKey, ConvertTo-VcvarsDelta, Import-VcvarsEnv,
    Get-NinjaDepsSig, Read-DepsCheckStamp, Write-DepsCheckStamp, Test-DepsCheckNeeded
