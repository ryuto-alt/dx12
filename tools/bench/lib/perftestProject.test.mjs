//   node --test tools/bench
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { resolvePerfTestProject } from "./perftestProject.mjs";

// 円記号(\)・スラッシュ(/) の両方を捉える（Windows の個人フォルダ）
const BS = String.fromCharCode(92); // 円記号
const PERSONAL_PATH = new RegExp(`[A-Za-z]:[${BS}${BS}/]+Users[${BS}${BS}/]+[A-Za-z0-9_.-]+`, "i");
const HERE = path.dirname(fileURLToPath(import.meta.url));
const SPLASH_PY = path.join(HERE, "../../check_splash_sync.py");

function runSplash(env) {
  for (const py of ["python", "python3", "py"]) {
    const r = spawnSync(py, [SPLASH_PY], { env, encoding: "utf8" });
    if (!r.error) return r;
  }
  return null; // python が無い環境
}

test("DX12_PERFTEST_PROJECT が最優先", () => {
  assert.equal(resolvePerfTestProject({ DX12_PERFTEST_PROJECT: "/p/PerfTest", DX12_GOLDEN_PROJECTS_ROOT: "/r" }), "/p/PerfTest");
});
test("親フォルダ指定から PerfTest を作る", () => {
  assert.equal(resolvePerfTestProject({ DX12_GOLDEN_PROJECTS_ROOT: "/r" }), path.join("/r", "PerfTest"));
});
test("未設定は null", () => {
  assert.equal(resolvePerfTestProject({}), null);
});
test("個人パス検出は円記号・スラッシュの両方を捉える", () => {
  assert.ok(PERSONAL_PATH.test(String.raw`C:\Users\someone\Documents\x`));
  assert.ok(PERSONAL_PATH.test("C:/Users/someone/x"));
  assert.ok(!PERSONAL_PATH.test("no path here"));
});
test("対象 2 ファイルに個人パスが残っていない", () => {
  for (const rel of ["../scale_ladder.mjs", "../../check_splash_sync.py"]) {
    const t = fs.readFileSync(path.join(HERE, rel), "utf8");
    assert.ok(!PERSONAL_PATH.test(t), `${rel} に個人パス`);
  }
});
test("check_splash_sync.py: 引数も環境変数も無ければ使い方を出して終了コード 2", () => {
  const env = { ...process.env };
  delete env.DX12_SPLASH_SYNC_DIR;
  const r = runSplash(env);
  if (!r) return;
  assert.equal(r.status, 2);
  assert.match(r.stdout, /DX12_SPLASH_SYNC_DIR/);
});
test("check_splash_sync.py: 環境変数の指定先を見る（wav 無しなら終了コード 2）", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "splashsync-"));
  try {
    const r = runSplash({ ...process.env, DX12_SPLASH_SYNC_DIR: dir });
    if (!r) return;
    assert.equal(r.status, 2);
    assert.match(r.stdout, /sync_\*\.wav/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
