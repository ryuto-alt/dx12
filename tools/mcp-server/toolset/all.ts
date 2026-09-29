// ツールモジュールの読み込み(= 登録)順。ここの並びが tools/list の並びになるので、入れ替えない。
// shell 5 本が先頭、続いて旧 220 本(index.ts から機械分割したモジュール群)。
import "./shell.ts";
import "./read.ts";
import "./edit.ts";
import "./undo.ts";
import "./spawn.ts";
import "./play.ts";
import "./imguiInput.ts";
import "./physics.ts";
import "./render.ts";
import "./sceneNaming.ts";
import "./testplay.ts";
import "./playtestStore.ts";
import "./blender.ts";
import "./materialAnim.ts";
import "./multiplayer.ts";
import "./assets.ts";
import "./composite.ts";
import "./capture.ts";
import "./uiAssets.ts";
import "./pick.ts";
import "./terrain.ts";
import "./vfx.ts";
import "./lighting.ts";
import "./sequence.ts";
import "./decals.ts";
import "./navmesh.ts";
import "./diag.ts";
import "./quality.ts";
import "./git.ts";
import "./jev.ts";
import "./perceive.ts";
import "./qualityGate.ts";
// Core の統合ツール(旧ツールの登録表を引くので、旧ツールの全モジュールより後ろ)と、マニフェストの expose:"core" による動的昇格。
import "./coreTools.ts";
// フリート(専用エンジンの管理)。full 面では旧 220 本の後ろ(tools/list の末尾)。
import "./fleet.ts";
