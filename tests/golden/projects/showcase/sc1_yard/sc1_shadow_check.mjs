// SC-1 の「塔の影の試験」（W1-NB13）。
//   node tests/golden/projects/showcase/sc1_yard/sc1_shadow_check.mjs [--port 8854] [--write] [--exe <DX12Engine.exe>]
// 測量塔（高さ 60m）の脚 4 本が、太陽の高度 15° で地面に落とす細長い影（幅 0.5〜0.65m）を、固定カメラから撮って数える。
//   影の中心（脚の影）12 点と、日向（影の間と外側）9 点の輝度を比べる。
//   Epic（品質プリセット Epic = RT 太陽影 ON + 動的 CSM）: 合否を判定する。
//     合格 = 影 12 点の輝度の平均 / 日向の平均 < 0.60、かつ影の 12 点がどれも日向の平均の 0.80 倍より暗い。
//   High（RT OFF）: W1 は記録だけ（合否にしない。W2 の LS-02b で合否にする）。
// 起動済みのエンジンへ繋ぐ（--port）。--exe を渡すと、このスクリプトがヘッドレスで 1 台起動して終わりに落とす
// （tools/bench/lib/engine.mjs の launchEngine。自分の PID だけを止める）。
// ★set_quality_preset は「シーンの設定へ反映」するので、シーンは保存しない（撮り終えたら開き直せば元に戻る）。
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, connect, launchEngine, sleep } from "../../../../../tools/bench/lib/engine.mjs";

const require = createRequire(import.meta.url);
const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));
const HERE = path.dirname(fileURLToPath(import.meta.url));
const arg = (k) => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
const WRITE = process.argv.includes("--write");

// build_sc1.mjs と同じ値（塔の位置と脚の間隔、太陽の向き）
const TX = -22, TZ = 8, LEG = 1.6;
const SUN_ELEV = 15, SUN_AZ = 250;
// 光の進む向きの水平成分（set_sun {elevation:15, azimuth:250} の応答 direction=(0.9077,-0.2588,0.3304) の x,z）
const D = (() => { const x = 0.9076733589172363, z = 0.3303660750389099, n = Math.hypot(x, z); return [x / n, z / n]; })();
const N = [-D[1], D[0]];

const legs = [[-1, -1], [1, -1], [-1, 1], [1, 1]].map(([sx, sz]) => [TX + sx * LEG, TZ + sz * LEG]);
const ts = [18, 22, 26];
const shadowPts = [], litPts = [];
for (const t of ts) {
  for (const [lx, lz] of legs) shadowPts.push([lx + D[0] * t, 0, lz + D[1] * t]);
  const cx = TX + D[0] * t, cz = TZ + D[1] * t;
  for (const o of [0, 3.2, -3.2]) litPts.push([cx + N[0] * o, 0, cz + N[1] * o]);
}
const c22 = [TX + D[0] * 22, TZ + D[1] * 22];
const CAM = { position: [c22[0] + N[0] * 14, 10, c22[1] + N[1] * 14], target: [c22[0], 0, c22[1]] };

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a) => { const l = Math.hypot(...a); return [a[0] / l, a[1] / l, a[2] / l]; };

function lumAt(img, x, y, r = 2) {
  let s = 0, n = 0;
  for (let j = -r; j <= r; j++) for (let i = -r; i <= r; i++) {
    const xx = Math.round(x) + i, yy = Math.round(y) + j;
    if (xx < 0 || yy < 0 || xx >= img.width || yy >= img.height) continue;
    const o = (yy * img.width + xx) * 4;
    s += 0.2126 * img.data[o] + 0.7152 * img.data[o + 1] + 0.0722 * img.data[o + 2]; n++;
  }
  return n ? s / n : NaN;
}

async function measure(c, label, preset) {
  const q = await c.call("set_quality_preset", { preset });
  await c.call("set_editor_camera", CAM);
  await c.call("step_frames", { frames: 90, deterministic: true }, 300_000);
  const file = path.join(REPO_ROOT, "build", "golden-out", `sc1_tower_shadow.${label}.png`);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const shot = await c.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: 8, path: file }, 300_000);
  const img = PNG.sync.read(fs.readFileSync(shot.files?.png ?? file));
  // 投影は自前（エディタカメラ = 左手系・縦 FOV 45°・上は +Y。project_world_to_screen はエンティティしか受けない）
  const fwd = norm(sub(CAM.target, CAM.position));
  const right = norm(cross([0, 1, 0], fwd));
  const up = cross(fwd, right);
  const tanH = Math.tan((45 * Math.PI) / 360), aspect = img.width / img.height;
  const project = (p) => {
    const v = sub(p, CAM.position);
    const z = dot(v, fwd);
    return [img.width * (0.5 + dot(v, right) / (z * tanH * aspect) / 2), img.height * (0.5 - dot(v, up) / (z * tanH) / 2)];
  };
  const sample = async (pts) => pts.map((p) => { const [px, py] = project(p); return lumAt(img, px, py); });
  if (process.argv.includes("--debug")) { // 測った点に印を付けた絵（目視確認用）
    const dbg = new PNG({ width: img.width, height: img.height }); img.data.copy(dbg.data);
    for (const [pts, col] of [[shadowPts, [255, 0, 0]], [litPts, [0, 255, 0]]])
      for (const p of pts) { const [px, py] = project(p); for (let k = -4; k <= 4; k++) for (const [xx, yy] of [[px + k, py], [px, py + k]]) { const o = (Math.round(yy) * img.width + Math.round(xx)) * 4; if (o >= 0 && o < dbg.data.length) { dbg.data[o] = col[0]; dbg.data[o + 1] = col[1]; dbg.data[o + 2] = col[2]; dbg.data[o + 3] = 255; } } }
    fs.writeFileSync(file.replace(".png", ".marks.png"), PNG.sync.write(dbg));
  }
  const sh = await sample(shadowPts), li = await sample(litPts);
  const mean = (a) => a.reduce((x, y) => x + y, 0) / a.length;
  const litMean = mean(li), shMean = mean(sh);
  const ratio = shMean / litMean;
  const darkest = sh.filter((x) => x < litMean * 0.8).length;
  return {
    label, preset, rtSun: q.plan?.shadows?.rtSun ?? null, dynamicCsm: q.plan?.shadows?.dynamicCsm ?? null,
    shadowMean: +shMean.toFixed(2), litMean: +litMean.toFixed(2), ratio: +ratio.toFixed(4),
    shadowPointsDarkerThan80pct: `${darkest}/${sh.length}`,
    pass: ratio < 0.6 && darkest === sh.length,
  };
}

let eng = null, port = Number(arg("--port") || 8854);
if (arg("--exe")) {
  eng = await launchEngine({ exe: arg("--exe"), project: path.join(HERE), scene: "scenes/sc1_yard.json", name: "sc1-shadow" });
  port = eng.port;
}
let rc = 0;
try {
  const c = await connect(port);
  if ((await c.call("ping")).currentScene !== "scenes/sc1_yard.json") await c.call("open_scene", { path: "scenes/sc1_yard.json" }, 300_000);
  await sleep(1500);
  const dxr = await c.call("get_dxr");
  const results = [];
  if (dxr.supported) results.push(await measure(c, "epic", "epic"));
  else results.push({ label: "epic", skipped: "DXR 非対応のハード（Tier 1.1 / SM 6.5 未満）なので SKIP" });
  const high = await measure(c, "high", "high");
  high.note = "W1 は記録だけ（合否にしない）。W2 の LS-02b で合否にする";
  delete high.pass;
  results.push(high);
  const out = { scene: "scenes/sc1_yard.json", camera: CAM, sunElevationDeg: SUN_ELEV, samples: { shadow: shadowPts.length, lit: litPts.length }, results };
  console.log(JSON.stringify(out, null, 2));
  if (results[0].pass === false) rc = 1;
  if (WRITE) fs.writeFileSync(path.join(HERE, "shadow_baseline.json"), JSON.stringify(out, null, 2) + "\n");
  c.close();
} finally {
  eng?.kill();
}
process.exit(rc);
