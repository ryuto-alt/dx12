// SC-1「校正の庭（Calibrate Yard）」を、MCP の呼び出しだけで組む（W1-NB13）。
//   node tests/golden/projects/showcase/sc1_yard/build_sc1.mjs [--port 8854]
// 起動済みのエンジン（このプロジェクトを開いたもの）へ繋ぎ、new_scene（Epic のルック）から始めて
// create_entity / set_transform / set_color / set_pbr / set_sun / vfx_place / spawn_model / attach_lua_component
// だけでシーンを作り、save_scene で assets/scenes/sc1_yard.json へ保存する。
// 呼び出しの記録は build_calls.json（MCP-14r の課題の素材）。乱数は固定シードで決定論。
// 前提: 焚き火と松明は VFX プリセット（p_campfire / p_torch）、人形は engine/characters/mannequin（NB14。import_asset でプロジェクトへ入れる）。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { connect } from "../../../../../tools/bench/lib/engine.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../../../../..");
const portArg = process.argv.indexOf("--port");
const PORT = portArg >= 0 ? Number(process.argv[portArg + 1]) : 8854;

const eng = await connect(PORT);
const calls = [];
async function call(method, params = {}) {
  const r = await eng.call(method, params, 300_000);
  // 記録には個人のパスを残さない（リポジトリの絶対パスは <repo> に置き換える）
  calls.push({ method, params: JSON.parse(JSON.stringify(params).split(REPO.split(path.sep).join("/")).join("<repo>")) });
  return r;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 固定シードの乱数（mulberry32）
let seed = 0x5c1a0001;
const rnd = () => {
  seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const range = (a, b) => a + (b - a) * rnd();

/** 箱を 1 つ置く。size は各軸の寸法（m）、pos は箱の中心、rotY は度。 */
async function box(name, pos, size, { color, metallic = 0, roughness = 0.8, rotY = 0, parent } = {}) {
  await call("create_entity", { name, type: "box", position: pos, ...(parent ? { parentName: parent } : {}) });
  await call("set_transform", { name, position: pos, rotation: [0, rotY, 0], scale: size });
  if (color) await call("set_color", { name, color });
  await call("set_pbr", { name, metallic, roughness });
}

// ─── 0. 新規シーン（Epic のルック）と不要物の削除 ─────────────────────────
await call("new_scene", { profile: "epic" });
await sleep(1500);
for (const n of ["Cube", "Ground", "Grid"]) await call("delete_entity", { name: n });

// ─── 1. 時刻と太陽（夕方、高度 15°。西から東へ長い影） ──────────────────
await call("set_sun", { elevation: 15, azimuth: 250 });
// Epic のルック一式（RT 太陽影 + 動的 CSM + PCSS + shadingRevision 2 など）をシーンの設定へ書き込む（保存されて golden もこの値で撮る）
await call("set_quality_preset", { preset: "epic" });

// ─── 2. 地面（1000m 四方。塔の影が約 220m 伸びる） ──────────────────────
await call("create_entity", { name: "Ground", type: "plane", position: [0, 0, 0], size: 1000 });
await call("set_color", { name: "Ground", color: [0.26, 0.27, 0.2] });
await call("set_pbr", { name: "Ground", metallic: 0, roughness: 0.95 });

// ─── 3. 測量塔（高さ 60m） ───────────────────────────────────────────
//   脚 4 本（56m）、10m ごとの水平材、頂部の観測室（4m）。合計 60m。
const TX = -22, TZ = 8;
const legOff = 1.6;
for (const [i, [sx, sz]] of [[-1, -1], [1, -1], [-1, 1], [1, 1]].entries())
  await box(`Tower_Leg_${i}`, [TX + sx * legOff, 28, TZ + sz * legOff], [0.5, 56, 0.5], { color: [0.55, 0.56, 0.58], metallic: 0.6, roughness: 0.45 });
for (let k = 1; k <= 5; k++) {
  const y = k * 10;
  await box(`Tower_BandX_${k}`, [TX, y, TZ - legOff], [legOff * 2, 0.25, 0.25], { color: [0.5, 0.51, 0.53], metallic: 0.6, roughness: 0.5 });
  await box(`Tower_BandX2_${k}`, [TX, y, TZ + legOff], [legOff * 2, 0.25, 0.25], { color: [0.5, 0.51, 0.53], metallic: 0.6, roughness: 0.5 });
  await box(`Tower_BandZ_${k}`, [TX - legOff, y, TZ], [0.25, 0.25, legOff * 2], { color: [0.5, 0.51, 0.53], metallic: 0.6, roughness: 0.5 });
  await box(`Tower_BandZ2_${k}`, [TX + legOff, y, TZ], [0.25, 0.25, legOff * 2], { color: [0.5, 0.51, 0.53], metallic: 0.6, roughness: 0.5 });
}
await box("Tower_Cabin", [TX, 58, TZ], [4.4, 4, 4.4], { color: [0.82, 0.8, 0.74], roughness: 0.7 });
await box("Tower_Base", [TX, 0.4, TZ], [5.2, 0.8, 5.2], { color: [0.4, 0.4, 0.4], roughness: 0.9 });

// ─── 4. 柱の林（細い柱 36 本。薄い物の影とピーターパンの検証） ──────────────
const pillars = [];
while (pillars.length < 36) {
  const x = range(5, 24), z = range(-14, 12);
  if (pillars.every(([px, pz]) => Math.hypot(px - x, pz - z) > 1.7)) pillars.push([x, z]);
}
for (const [i, [x, z]] of pillars.entries()) {
  const h = range(3, 7.5);
  await box(`Pillar_${String(i).padStart(2, "0")}`, [x, h / 2, z], [0.22, h, 0.22], { color: [0.62, 0.58, 0.5], roughness: 0.75, rotY: range(0, 90) });
}

// ─── 5. 柵（南側 z=-13 に 18 本の杭と 2 本の横木、東側に折れる） ───────────
for (let i = 0; i <= 16; i++)
  await box(`Fence_Post_${String(i).padStart(2, "0")}`, [-16 + i * 2, 0.65, -13], [0.14, 1.3, 0.14], { color: [0.42, 0.3, 0.2], roughness: 0.85 });
await box("Fence_Rail_Lo", [0, 0.5, -13], [32, 0.08, 0.06], { color: [0.45, 0.33, 0.22], roughness: 0.85 });
await box("Fence_Rail_Hi", [0, 1.05, -13], [32, 0.08, 0.06], { color: [0.45, 0.33, 0.22], roughness: 0.85 });
for (let i = 1; i <= 5; i++)
  await box(`Fence_PostE_${i}`, [16, 0.65, -13 + i * 2], [0.14, 1.3, 0.14], { color: [0.42, 0.3, 0.2], roughness: 0.85 });
await box("Fence_RailE_Lo", [16, 0.5, -8], [0.06, 0.08, 10], { color: [0.45, 0.33, 0.22], roughness: 0.85 });
await box("Fence_RailE_Hi", [16, 1.05, -8], [0.06, 0.08, 10], { color: [0.45, 0.33, 0.22], roughness: 0.85 });

// ─── 6. 段差（5 段。西向きに登る） ───────────────────────────────────
for (let i = 0; i < 5; i++) {
  const h = (i + 1) * 0.22;
  await box(`Step_${i}`, [-13 - i * 0.9, h / 2, -4], [0.9, h, 5], { color: [0.5, 0.48, 0.44], roughness: 0.85 });
}

// ─── 7. 焚き火（石の輪 + VFX）と松明 4 本 ────────────────────────────
for (let i = 0; i < 8; i++) {
  const a = (i / 8) * Math.PI * 2;
  await box(`FireStone_${i}`, [Math.cos(a) * 0.75, 0.13, Math.sin(a) * 0.75], [0.3, 0.26, 0.24], { color: [0.36, 0.35, 0.34], roughness: 0.9, rotY: (a * 180) / Math.PI });
}
await call("vfx_place", { preset: "p_campfire", name: "Campfire", position: [0, 0.05, 0] });
const torches = [[-6, -6], [6, -6], [-6, 6], [6, 6]];
for (const [i, [x, z]] of torches.entries()) {
  await box(`Torch_Pole_${i + 1}`, [x, 1.1, z], [0.1, 2.2, 0.1], { color: [0.3, 0.22, 0.15], roughness: 0.9 });
  await call("vfx_place", { preset: "p_torch", name: `Torch_${i + 1}`, position: [x, 2.25, z] });
}

// ─── 8. 風見（柱 + 回る腕。腕は Lua で Play 中に回る。エディタでは固定角） ──
const VX = 9, VZ = -4;
await box("Vane_Pole", [VX, 2.2, VZ], [0.1, 4.4, 0.1], { color: [0.45, 0.46, 0.48], metallic: 0.7, roughness: 0.4 });
await call("create_entity", { name: "Vane_Rotor", type: "empty", position: [VX, 4.4, VZ] });
await call("set_transform", { name: "Vane_Rotor", position: [VX, 4.4, VZ], rotation: [0, 35, 0], scale: [1, 1, 1] });
await box("Vane_Arm", [0, 0, 0], [1.8, 0.07, 0.07], { color: [0.7, 0.55, 0.2], metallic: 0.8, roughness: 0.35, rotY: 0, parent: "Vane_Rotor" });
await box("Vane_Fin", [-0.8, 0, 0], [0.4, 0.5, 0.03], { color: [0.7, 0.55, 0.2], metallic: 0.8, roughness: 0.35, rotY: 0, parent: "Vane_Rotor" });
await call("create_lua_component", {
  name: "sc1_vane",
  code: [
    "-- SC-1: 風見の腕を Y 軸まわりに一定の角速度で回す（Play 中だけ。エディタの撮影では固定角のまま）",
    "function OnUpdate(self, dt)",
    "    self.angle = (self.angle or 35) + 40 * dt",
    "    self.transform.rotation = Vec3.new(0, self.angle, 0)",
    "end",
    "",
  ].join("\n"),
});
await call("attach_lua_component", { name: "Vane_Rotor", script: "components/sc1_vane.lua" });

// ─── 9. 歩く人形（NB14 のマネキン。スキンド。Play 中は Walk を回して円を歩く） ──
await call("import_asset", { sourcePath: path.join(REPO, "assets/engine/characters/mannequin/mannequin.glb").replace(/\\/g, "/"), destPath: "models/mannequin.glb", overwrite: true });
await sleep(1500);
await call("spawn_model", { name: "Mannequin", path: "models/mannequin.glb", position: [-1.2, 0, -2.2] });
await call("set_transform", { name: "Mannequin", position: [-1.2, 0, -2.2], rotation: [0, 70, 0], scale: [1, 1, 1] });
await call("create_lua_component", {
  name: "sc1_walker",
  code: [
    "-- SC-1: 人形が焚き火のまわり（半径 4m）を Walk のループで歩く（Play 中だけ）",
    "function OnStart(self)",
    "    local e = scene:findEntity(self.name)",
    "    e:playAnimByName('Walk', 0.0)",
    "    e:setLooping(true)",
    "    self.t = 0",
    "end",
    "function OnUpdate(self, dt)",
    "    self.t = self.t + dt * 0.35",
    "    local x, z = 4.0 * math.cos(self.t), 4.0 * math.sin(self.t)",
    "    self.transform.position = Vec3.new(x, 0, z)",
    "    self.transform.rotation = Vec3.new(0, -math.deg(self.t), 0)",
    "end",
    "",
  ].join("\n"),
});
await call("attach_lua_component", { name: "Mannequin", script: "components/sc1_walker.lua" });

// ─── 10. カメラと保存 ──────────────────────────────────────────────
await call("set_editor_camera", { position: [-4, 5, -26], target: [-8, 16, 8] });
await call("save_scene", { path: "scenes/sc1_yard.json" });

fs.writeFileSync(path.join(HERE, "build_calls.json"), JSON.stringify({ port: PORT, count: calls.length, calls }, null, 1) + "\n");
console.log(`done: ${calls.length} calls`);
eng.close();
