@echo off
rem usage: run_ui_tests.bat [tools\run_ui_tests.ps1 の引数]  (窓なし・背景・仮想入力で全件実行する本体へ委譲)
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\run_ui_tests.ps1" %*
exit /b %ERRORLEVEL%
