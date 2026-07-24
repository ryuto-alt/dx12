@echo off
setlocal
set "VCPKG_ROOT=C:\Users\ryuto\vcpkg"
rem Limit parallel jobs to avoid swap-freeze on 16GB RAM. Override via DX12_BUILD_JOBS.
if not defined DX12_BUILD_JOBS set "DX12_BUILD_JOBS=10"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"
powershell -ExecutionPolicy Bypass -File tools/gen_asset_key.ps1 || exit /b 1
cmake --preset windows-release || exit /b 1
rem Run at BelowNormal priority (inherited by cl.exe) so the PC stays responsive.
powershell -NoProfile -Command "$psi = New-Object System.Diagnostics.ProcessStartInfo('cmake', '--build build/release -j %DX12_BUILD_JOBS%'); $psi.UseShellExecute = $false; $p = [System.Diagnostics.Process]::Start($psi); try { $p.PriorityClass = 'BelowNormal' } catch {}; $p.WaitForExit(); exit $p.ExitCode" || exit /b 1
echo BUILD_RELEASE_OK
