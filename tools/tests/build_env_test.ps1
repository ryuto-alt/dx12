<#
  tools/lib/BuildEnv.psm1(vcvars の環境キャッシュと #deps 0 検査の省略判定)のテスト。
  偽の vcvars64.bat を使うので、本物の VS も cmake も ninja も動かさない。
    pwsh tools/tests/build_env_test.ps1
#>
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\BuildEnv.psm1') -Force
$fail = 0
function Assert-That([bool]$c, [string]$m) { if ($c) { Write-Host "  PASS: $m" } else { Write-Host "  FAIL: $m"; $script:fail++ } }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('dx12-buildenv-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$savedPath = $env:PATH
try {
    Write-Host '(a) vcvars cache: second import does not run vcvars again'
    $vsBuild = Join-Path $tmp 'VC\Auxiliary\Build'; New-Item -ItemType Directory -Force $vsBuild | Out-Null
    $toolsDir = Join-Path $tmp 'MSVC\14.99'; New-Item -ItemType Directory -Force $toolsDir | Out-Null
    $counter = Join-Path $tmp 'runs.txt'
    $bat = Join-Path $vsBuild 'vcvars64.bat'
    Set-Content $bat -Encoding ascii -Value @(
        '@echo off',
        "echo x>>`"$counter`"",
        "set `"VCToolsInstallDir=$toolsDir\`"",
        "set `"DXTEST_FAKE_INCLUDE=$toolsDir\include`"",
        "set `"PATH=$toolsDir\bin;%PATH%`"")
    Set-Content (Join-Path $vsBuild 'Microsoft.VCToolsVersion.default.txt') '14.99.1'
    $cacheDir = Join-Path $tmp 'cache'
    $env:DX12_BUILD_CACHE_DIR = $cacheDir
    Remove-Item Env:\DX12_BUILD_NO_VCVARS_CACHE -ErrorAction SilentlyContinue
    $r1 = Import-VcvarsEnv -VcPath $bat
    Assert-That (-not $r1.Hit) 'first import runs vcvars (cache miss)'
    Assert-That ($env:DXTEST_FAKE_INCLUDE -eq "$toolsDir\include") 'variables applied'
    Assert-That ($env:PATH.StartsWith("$toolsDir\bin;")) 'PATH entry prepended'
    Assert-That ((Get-Content $counter).Count -eq 1) 'vcvars ran once'
    # 2 回目: 環境を戻してから
    $env:PATH = $savedPath; Remove-Item Env:\DXTEST_FAKE_INCLUDE; Remove-Item Env:\VCToolsInstallDir
    $env:PATH = "C:\keep-me;$savedPath"
    $r2 = Import-VcvarsEnv -VcPath $bat
    Assert-That $r2.Hit 'second import is a cache hit'
    Assert-That ((Get-Content $counter).Count -eq 1) 'vcvars was NOT run again'
    Assert-That (($env:DXTEST_FAKE_INCLUDE -eq "$toolsDir\include") -and ($env:VCToolsInstallDir -eq "$toolsDir\")) 'variables restored from cache'
    Assert-That ($env:PATH.StartsWith("$toolsDir\bin;C:\keep-me;")) 'caller PATH kept, cached entries prepended'
    Assert-That ((@($env:PATH -split ';' | Where-Object { $_ -eq "$toolsDir\bin" }).Count) -eq 1) 'no duplicate PATH entry on repeated import'
    $null = Import-VcvarsEnv -VcPath $bat
    Assert-That ((@($env:PATH -split ';' | Where-Object { $_ -eq "$toolsDir\bin" }).Count) -eq 1) 'importing twice in a row keeps one entry'

    Write-Host '(b) cache is invalidated when vcvars64.bat or the tools version changes'
    (Get-Item $bat).LastWriteTimeUtc = (Get-Date).AddMinutes(1).ToUniversalTime()
    $r3 = Import-VcvarsEnv -VcPath $bat
    Assert-That (-not $r3.Hit) 'bat mtime changed -> miss'
    Set-Content (Join-Path $vsBuild 'Microsoft.VCToolsVersion.default.txt') '14.99.2'
    $r4 = Import-VcvarsEnv -VcPath $bat
    Assert-That (-not $r4.Hit) 'default tools version changed -> miss'
    Remove-Item -Recurse -Force $toolsDir
    $r5 = Import-VcvarsEnv -VcPath $bat -Runner { param($p) @('VCToolsInstallDir=X:\gone\', 'PATH=X:\gone\bin;' + $env:PATH) }
    Assert-That (-not $r5.Hit) 'cached VCToolsInstallDir missing on disk -> miss (rebuilt)'
    $env:DX12_BUILD_NO_VCVARS_CACHE = '1'
    $r6 = Import-VcvarsEnv -VcPath $bat -Runner { param($p) @('VCToolsInstallDir=X:\gone\') }
    Assert-That (-not $r6.Hit) 'DX12_BUILD_NO_VCVARS_CACHE=1 -> never a hit'
    Remove-Item Env:\DX12_BUILD_NO_VCVARS_CACHE

    Write-Host '(c) #deps 0 check is skipped only when .ninja_deps is unchanged since the last clean check'
    $bd = Join-Path $tmp 'build'; New-Item -ItemType Directory -Force $bd | Out-Null
    Assert-That (-not (Test-DepsCheckNeeded $bd)) 'no .ninja_deps -> nothing to check'
    Set-Content (Join-Path $bd '.ninja_deps') 'aaaa'
    Assert-That (Test-DepsCheckNeeded $bd) 'never checked -> needed'
    Write-DepsCheckStamp $bd (Get-NinjaDepsSig $bd)
    Assert-That (-not (Test-DepsCheckNeeded $bd)) 'stamp matches -> skipped'
    Add-Content (Join-Path $bd '.ninja_deps') 'more'
    Assert-That (Test-DepsCheckNeeded $bd) '.ninja_deps changed (build wrote deps) -> needed'

    Write-Host '(d) a caller that already has part of vcvars: the cache still stores the full vcvars entries'
    $vc = Join-Path $tmp 'VS\VC\Auxiliary\Build\vcvars64.bat'
    $kits = 'C:\Fake\Windows Kits\10\bin\x64'; $vsCmake = Join-Path $tmp 'VS\Common7\IDE\CommonExtensions\CMake\bin'
    $baseline = @{ PATH = "C:\keep;$kits;$vsCmake"; INCLUDE = 'C:\inc'; Other = 'same' }
    $lines = @("PATH=C:\newtool;C:\keep;$kits;$vsCmake", 'INCLUDE=C:\inc', 'Other=same', 'VCToolsInstallDir=C:\vc\', 'UCRTVersion=10.0.1')
    $d = ConvertTo-VcvarsDelta $lines $baseline $vc
    Assert-That (($d.pathAdd -contains $kits) -and ($d.pathAdd -contains $vsCmake) -and ($d.pathAdd -contains 'C:\newtool')) 'Windows Kits / VS entries kept even when the caller already had them'
    Assert-That (-not ($d.pathAdd -contains 'C:\keep')) 'ordinary caller PATH entries are not stored'
    Assert-That ($d.vars.Contains('INCLUDE') -and $d.vars.Contains('UCRTVersion') -and -not $d.vars.Contains('Other')) 'vcvars variables kept by name, unrelated unchanged ones are not'
}
finally {
    $env:PATH = $savedPath
    Remove-Item Env:\DX12_BUILD_CACHE_DIR, Env:\DXTEST_FAKE_INCLUDE, Env:\VCToolsInstallDir -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($fail -gt 0) { Write-Host "FAILED: $fail"; exit 1 }
Write-Host 'ALL PASSED'
