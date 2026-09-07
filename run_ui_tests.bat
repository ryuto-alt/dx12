@echo off
rem usage: run_ui_tests.bat [--ui-tests-deep]
cd /d "C:\Users\ryuto\Documents\dx12\build\release"
"C:\Users\ryuto\Documents\dx12\build\release\DX12Engine.exe" --ui-tests-run-all %* --project "C:\Users\ryuto\AppData\Local\Temp\claude\C--Windows-System32\9d387373-6f7f-4947-bea5-68345578a7b6\scratchpad\AlphaTest" > "%TEMP%\dx12_uitests.txt" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%TEMP%\dx12_uitests.txt"
