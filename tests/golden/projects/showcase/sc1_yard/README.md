# SC-1「校正の庭（Calibrate Yard）」

W1 のショーケース。プリミティブと engine 同梱の資産だけで組む（外部の素材なし）。

- 夕方の庭（太陽の高度 15°、品質プリセット Epic をシーンへ反映済み）、高さ 60m の測量塔、柱の林 36 本、柵、5 段の段差、
  焚き火 1 基と松明 4 本（UnoFX のプリセット `p_campfire` / `p_torch`）、風見、歩く人形（NB14 のマネキン）。
- 組み立て: `node tests/golden/projects/showcase/sc1_yard/build_sc1.mjs [--port 8854]`（このプロジェクトを開いたエンジンへ MCP で繋ぐ。
  呼び出しの記録は `build_calls.json`。MCP-14r の課題の素材）。
- 塔の影の試験: `node tests/golden/projects/showcase/sc1_yard/sc1_shadow_check.mjs [--port N | --exe <DX12Engine.exe>] [--write]`
  （Epic は合否、High は記録だけ。記録は `shadow_baseline.json`）。
- ゴールデン: `tests/golden/entries/sc1_{tower,yard,fire}.json`（`group:"repeat"`。`node tools/bench/golden.mjs --group repeat --only sc1_tower,sc1_yard,sc1_fire`）。
- 風見と人形の Lua（`assets/components/sc1_*.lua`）は Play 中だけ動く。エディタでの撮影は静止ポーズ。
- 焚き火・松明のプリセットは `assets/engine/vfx2/presets/`（エンジン側）を参照する。`ASSETS_DIR` にそれが無いビルドでは `vfx_place` も読み込みも失敗する。
