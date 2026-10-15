@echo off
rem 描画の回帰テスト（ゴールデン画像）を 1 コマンドで回す。
rem   run_golden.bat            基準と比べる（終了コード 0=合格 / 1=不合格）
rem   run_golden.bat --update   基準を撮り直す（意図した絵の変更のときだけ）
rem 詳細は docs/GOLDEN_TESTING.md。exe は DX12_ENGINE_EXE か build\dev / build\release を探す。
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist tools\mcp-server\node_modules\pngjs (
  echo pngjs が無いので tools\mcp-server で npm ci します
  pushd tools\mcp-server && call npm ci && popd
)
node --test tools\bench\golden.test.mjs || exit /b 1
node tools\bench\golden.mjs %*
exit /b %errorlevel%
