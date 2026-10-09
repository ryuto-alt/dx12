@echo off
rem スマホをカートレースのコントローラーにする中継。この窓を開いている間だけつながる
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0phone-relay.ps1"
pause
