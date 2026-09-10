@echo off
cd /d "C:\Users\ryuto\Documents\dx12\build\release"
"C:\Users\ryuto\Documents\dx12\build\release\DX12Engine.exe" --ui-tests-run-all %* --project "C:\Users\ryuto\Documents\dev\game\MikuOblivion" > "%TEMP%\dx12_uitests_miku.txt" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%TEMP%\dx12_uitests_miku.txt"
