// temporal_metrics.mjs の単体テスト（エンジンも GPU も要らない）。
//   node --test tools/bench/temporal_metrics.test.mjs
// Python（numpy）が無い環境では、Python を呼ぶテストだけ SKIP にする。
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { REPO_ROOT } from "./lib/engine.mjs";
import { loadGoldenEntries } from "./lib/goldenStore.mjs";
import { parseArgs, findPython, driveState, phaseManifest, loadScenarios, analyzeManifest, compareResults, energyCheck } from "./temporal_metrics.mjs";

const python = findPython(null);
const hasNumpy = (() => { try { return spawnSync(python, ["-I", "-c", "import numpy"], { encoding: "utf8" }).status === 0; } catch { return false; } })();

function writePfm(file, w, h, fn) {
  const buf = Buffer.alloc(w * h * 12);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const v = fn(x, y);
    const o = ((h - 1 - y) * w + x) * 12;            // PFM は下から上
    buf.writeFloatLE(v, o); buf.writeFloatLE(v, o + 4); buf.writeFloatLE(v, o + 8);
  }
  fs.writeFileSync(file, Buffer.concat([Buffer.from(`PF\n${w} ${h}\n-1.0\n`, "latin1"), buf]));
  return file;
}

test("parseArgs: シーン・phase・repeat・settings", () => {
  const a = parseArgs(["--scene", "taa_torture,dof_chart", "--phase", "flicker,ghost", "--repeat", "2", "--settings", '{"taa":{"enabled":false}}', "--keep-frames"]);
  assert.deepEqual(a.scenes, ["taa_torture", "dof_chart"]);
  assert.deepEqual(a.phases, ["flicker", "ghost"]);
  assert.equal(a.repeat, 2); assert.equal(a.keepFrames, true);
  assert.deepEqual(a.settings, { taa: { enabled: false } });
  assert.throws(() => parseArgs(["--repeat", "9"]));
  assert.throws(() => parseArgs(["--nope"]));
  assert.throws(() => parseArgs(["--scene"]));
});

test("findPython: 明示 > 環境変数 > tools/parity の venv > python", () => {
  assert.equal(findPython("X.exe", {}, () => true), "X.exe");
  assert.equal(findPython(null, { UNO_BENCH_PYTHON: "Y.exe" }, () => true), "Y.exe");
  assert.match(findPython(null, {}, (p) => p.endsWith("python.exe")).split(path.sep).join("/"), /tools\/parity\/\.venv\/Scripts\/python\.exe$/);
  assert.equal(findPython(null, {}, () => false), "python");
});

test("driveState: 回転とカメラのパンは k に比例", () => {
  const d = { rotationPerFrame: [0, 0, 25], cameraPanPerFrame: [0.04, 0, 0] };
  const s = driveState(d, 4, { position: [0, 1, -5], target: [0, 1, 2] });
  assert.deepEqual(s.rotation, [0, 0, 100]);
  assert.deepEqual(s.camera.position.map((v) => +v.toFixed(6)), [0.16, 1, -5]);
  assert.deepEqual(s.camera.target.map((v) => +v.toFixed(6)), [0.16, 1, 2]);
  assert.equal(driveState({}, 3, null).camera, null);
});

test("phaseManifest: ghost は visibleFrames-1 がピーク、ssim は refCount の既定 4", () => {
  assert.equal(phaseManifest("ghost", { visibleFrames: 12, region: [0, 0, 1, 1] }, ["a"]).peakIndex, 11);
  assert.equal(phaseManifest("ssim", {}, ["a"]).refCount, 4);
  assert.equal(phaseManifest("flicker", {}, ["a"]).region, null);
});

test("手順のシーンは tests/golden/entries に有り、動かす対象は生成したシーンに存在する", () => {
  const cfg = loadScenarios();
  const entries = loadGoldenEntries(path.join(REPO_ROOT, "tests", "golden")).scenes;
  for (const [name, sc] of Object.entries(cfg.scenes)) {
    const e = entries.find((x) => x.name === name);
    assert.ok(e, `entries に ${name} が無い`);
    const scene = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, e.project, "assets", e.scene), "utf8"));
    const names = new Set(scene.entities.map((x) => x.name));
    const need = [];
    for (const p of Object.values(sc.phases ?? {})) { if (p.entity) need.push(p.entity); if (p.drive?.entity) need.push(p.drive.entity); for (const h of p.hide ?? []) need.push(h); }
    if (sc.energy?.drive?.entity) need.push(sc.energy.drive.entity);
    if (sc.energy?.centerEntity) need.push(sc.energy.centerEntity);
    for (const n of need) assert.ok(names.has(n), `${name}: エンティティ ${n} がシーンに無い`);
    for (const p of Object.values(sc.phases ?? {})) if (p.region) assert.ok(p.region.length === 4 && p.region.every((v) => v >= 0 && v <= 1), `${name}: region`);
  }
});

test("Python: 合成の PFM から指標が出て、2 回の差が 1% 以内になる", { skip: !hasNumpy && "numpy が無い" }, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "tm-test-"));
  try {
    const W = 64, H = 48;
    const orb = (lvl) => (x, y) => 0.1 + (x >= 28 && x < 36 && y >= 20 && y < 28 ? lvl : 0);
    const files = [];
    for (let i = 0; i < 6; i++) files.push(writePfm(path.join(dir, `g${i}.pfm`), W, H, orb(10)));
    for (let i = 0; i < 10; i++) files.push(writePfm(path.join(dir, `g${6 + i}.pfm`), W, H, orb(0)));
    const still = Array.from({ length: 6 }, (_, i) => writePfm(path.join(dir, `f${i}.pfm`), W, H, () => 0.5));
    const man = { exposure: 1, phases: {
      flicker: phaseManifest("flicker", { region: [0, 0, 1, 1] }, still),
      ghost: phaseManifest("ghost", { visibleFrames: 6, region: [0.3, 0.3, 0.7, 0.7] }, files),
      ssim: phaseManifest("ssim", { refCount: 2 }, still),
    } };
    const a = analyzeManifest(python, man, dir);
    const b = analyzeManifest(python, man, dir);
    assert.ok(a.flicker.meanStd < 1e-9);
    assert.ok(a.ghost.meanRatio < 1e-6 && a.ghost.peakEnergy > 100);
    assert.ok(a.ssim.min > 0.999);
    const c = compareResults(python, a, b, dir);
    assert.equal(c.within1pct, true); assert.equal(c.maxRelDiff, 0);
    const e = energyCheck(python, still[0], writePfm(path.join(dir, "b.pfm"), W, H, () => 0.5 * 1.1), {});
    assert.ok(Math.abs(e.total.ratio - 1.1) < 1e-4);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});
