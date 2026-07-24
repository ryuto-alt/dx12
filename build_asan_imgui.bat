@echo off
setlocal
rem ASan 計装ビルド(vcpkg 依存 = imgui 含めて全部 /fsanitize=address でリビルド)。
rem 通常の build/asan と違い「imgui 内部の域外アクセス」も検出できる。
rem 実行時: ASAN_OPTIONS は ':' 区切りのため log_path はドライブレター不可(cwd 相対にする)。
rem 例: set ASAN_OPTIONS=continue_on_error=1:halt_on_error=0:log_path=asan_report
set "VCPKG_ROOT=C:\Users\ryuto\vcpkg"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"
powershell -ExecutionPolicy Bypass -File tools/gen_asset_key.ps1 || exit /b 1

rem overlay triplet をビルドディレクトリへ生成(自己完結)
if not exist build\asan-triplets mkdir build\asan-triplets
(
echo set^(VCPKG_TARGET_ARCHITECTURE x64^)
echo set^(VCPKG_CRT_LINKAGE dynamic^)
echo set^(VCPKG_LIBRARY_LINKAGE dynamic^)
echo set^(VCPKG_CXX_FLAGS "/fsanitize=address /Zi /D_DISABLE_STRING_ANNOTATION /D_DISABLE_VECTOR_ANNOTATION /D_DISABLE_OPTIONAL_ANNOTATION"^)
echo set^(VCPKG_C_FLAGS "/fsanitize=address /Zi"^)
) > build\asan-triplets\x64-windows-asan.cmake

cmake -B build/asan-imgui -G Ninja -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
 -DVCPKG_TARGET_TRIPLET=x64-windows-asan ^
 -DVCPKG_OVERLAY_TRIPLETS="%~dp0build\asan-triplets" ^
 -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc /fsanitize=address /Zi /D_DISABLE_STRING_ANNOTATION /D_DISABLE_VECTOR_ANNOTATION /D_DISABLE_OPTIONAL_ANNOTATION" ^
 || exit /b 1
if not defined DX12_BUILD_JOBS set "DX12_BUILD_JOBS=10"
powershell -NoProfile -Command "$psi = New-Object System.Diagnostics.ProcessStartInfo('cmake', '--build build/asan-imgui -j %DX12_BUILD_JOBS%'); $psi.UseShellExecute = $false; $p = [System.Diagnostics.Process]::Start($psi); try { $p.PriorityClass = 'BelowNormal' } catch {}; $p.WaitForExit(); exit $p.ExitCode" || exit /b 1
echo BUILD_ASAN_IMGUI_OK
