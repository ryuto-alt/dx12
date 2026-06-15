@echo off
setlocal
set "VCPKG_ROOT=C:\Users\ryuto\vcpkg"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "C:\Users\ryuto\Documents\GitHub\dx12"
cmake --preset windows-debug || exit /b 1
cmake --build build/debug || exit /b 1
echo BUILD_DEBUG_OK
