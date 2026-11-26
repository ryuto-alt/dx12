//   node --test tools/bench
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { resolvePerfTestProject } from "./perftestProject.mjs";

test("DX12_PERFTEST_PROJECT が最優先", () => {
  assert.equal(resolvePerfTestProject({ DX12_PERFTEST_PROJECT: "/p/PerfTest", DX12_GOLDEN_PROJECTS_ROOT: "/r" }), "/p/PerfTest");
});
test("親フォルダ指定から PerfTest を作る", () => {
  assert.equal(resolvePerfTestProject({ DX12_GOLDEN_PROJECTS_ROOT: "/r" }), path.join("/r", "PerfTest"));
});
test("未設定は null", () => {
  assert.equal(resolvePerfTestProject({}), null);
});
test("対象 2 ファイルに個人パスが残っていない", () => {
  const here = path.dirname(fileURLToPath(import.meta.url));
  for (const rel of ["../scale_ladder.mjs", "../../check_splash_sync.py"]) {
    const t = fs.readFileSync(path.join(here, rel), "utf8");
    assert.ok(!/C:[\/]+Users[\/]+[A-Za-z0-9_.-]+/i.test(t), `${rel} に個人パス`);
  }
});
