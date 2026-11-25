/**
 * W05-42: fleet のエンジンのコピー(materializeBin)に D3D12\ (Agility SDK)が含まれることのテスト。
 * 偽のビルドディレクトリを一時に作る。ネット・エンジン不要。
 *
 * 実行: node fleetMaterialize.test.ts
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ENGINE_EXE, materializeBin, statBuild } from "./fleet/instance.ts";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "fleet-mat-"));
try {
  const src = path.join(tmp, "src");
  fs.mkdirSync(path.join(src, "D3D12", "sub"), { recursive: true });
  fs.mkdirSync(path.join(src, "shaders"), { recursive: true });
  fs.writeFileSync(path.join(src, ENGINE_EXE), "exe");
  fs.writeFileSync(path.join(src, "D3D12", "D3D12Core.dll"), "core");
  fs.writeFileSync(path.join(src, "D3D12", "sub", "nested.bin"), "nested");
  const build = statBuild(src, "test");
  assert.ok(build, "statBuild");
  const dst = path.join(tmp, "inst", "bin");
  const r = materializeBin(build!, dst);
  assert.equal(fs.readFileSync(path.join(dst, "D3D12", "D3D12Core.dll"), "utf8"), "core");
  assert.equal(fs.readFileSync(path.join(dst, "D3D12", "sub", "nested.bin"), "utf8"), "nested");
  assert.ok(r.linked.some((f) => f.endsWith("D3D12Core.dll")) || r.copiedBytes > 0);
  console.log("  OK  bin/D3D12/D3D12Core.dll がある(再帰)");

  // D3D12 の無いビルド(M2 前)でも従来どおり動く
  fs.rmSync(path.join(src, "D3D12"), { recursive: true });
  materializeBin(statBuild(src, "test")!, dst);
  assert.ok(!fs.existsSync(path.join(dst, "D3D12")));
  console.log("  OK  D3D12 が無いビルドでも動く");
  console.log("2 passed");
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
