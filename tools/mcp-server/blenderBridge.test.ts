/**
 * blenderBridge.ts（Blender 連携）の自己テスト。純関数だけを対象にする（Blender 不要）。
 *
 * 検証対象:
 *   [1-3] parseCodeResult    — execute_code の stdout から JSON を取り出す
 *   [4-6] buildExportScript  — 踏んだ罠が本当にスクリプトへ埋まっているか（回帰防止）
 *   [7-9] planImageRenames   — tmp 名だけ直し、人が付けた名前と埋め込みは触らない
 *   [10]  modelBrief         — 用途に応じた注意が出る / 単色マテリアル禁止が必ず入る
 *   [11]  blenderCandidatePaths — 新しい版から順に返す
 *
 * 実行: node blenderBridge.test.ts
 */

import assert from "node:assert/strict";
import {
  blenderCandidatePaths, buildExportScript, modelBrief, parseCodeResult, planImageRenames,
} from "./blenderBridge.ts";

let passed = 0;
function pass(label: string): void {
  passed++;
  console.log(`  OK  ${label}`);
}

// ─── [1-3] parseCodeResult ──────────────────────────────────────────────────
console.log("\n[1-3] parseCodeResult（stdout から JSON を拾う）");
{
  const r = parseCodeResult({ status: "success", result: { executed: true, result: '{"a":1}\n' } });
  assert.deepStrictEqual(r.json, { a: 1 });
  pass("print した JSON を取り出す");

  const multi = parseCodeResult({
    result: { result: "何かのログ\n{\"first\":1}\n{\"exported\":[\"Cube\"]}\n" },
  });
  assert.deepStrictEqual(multi.json, { exported: ["Cube"] }, "最後の JSON を採る");
  pass("print が複数あっても最後の JSON を採る");

  const none = parseCodeResult({ result: { result: "ただのログだけ\n" } });
  assert.equal(none.json, undefined);
  assert.ok(none.stdout.includes("ただのログ"), "stdout はそのまま返す");
  pass("JSON が無ければ stdout だけ返す（例外にしない）");
}

// ─── [4-6] buildExportScript ────────────────────────────────────────────────
console.log("\n[4-6] buildExportScript（罠が埋まっているか）");
{
  const code = buildExportScript({ objectNames: ["Rock"], outPath: "C:/tmp/rock.glb" });

  assert.ok(code.includes("use_selection=True"),
    "use_selection=False は .blend 内の全シーンを書き出してしまう");
  pass("use_selection=True で出す（全シーン書き出しの事故を防ぐ）");

  assert.ok(/for sc in bpy\.data\.scenes[\s\S]*select_set\(False/.test(code),
    "全シーンの全 view_layer で deselect してから対象を選ぶ");
  pass("書き出し前に全シーンで選択解除する");

  assert.ok(code.includes("shape_key_clear"), "シェイプキー削除が入っている");
  assert.ok(code.includes("TEX_IMAGE"), "画像テクスチャ有無の検査が入っている");
  pass("シェイプキー削除と単色マテリアル検出が入っている");

  // パスは Python 側で使えるよう / に正規化される
  const win = buildExportScript({ objectNames: [], outPath: "C:\\a\\b.glb" });
  assert.ok(win.includes('"C:/a/b.glb"'));
  pass("Windows のパス区切りを / に直して埋める");
}

// ─── [7-9] planImageRenames ─────────────────────────────────────────────────
console.log("\n[7-9] planImageRenames（tmp 名の直し）");
{
  const plan = planImageRenames(
    { images: [{ uri: "tmp1a2b3c.jpg" }, { uri: "brick_diff.png" }, { uri: "data:image/png;base64,AAA" }] },
    "rock",
  );
  assert.equal(plan.length, 1, "tmp 名の 1 枚だけが対象");
  pass("tmp 名だけを直す");

  assert.ok(!plan.some((p) => p.from === "brick_diff.png"));
  pass("人が付けた名前は触らない");

  assert.ok(!plan.some((p) => p.from.startsWith("data:")));
  pass("埋め込み画像（data:）は触らない");

  const dup = planImageRenames({ images: [{ uri: "tmpAAA.png" }, { uri: "tmpBBB.png" }] }, "wall");
  assert.equal(new Set(dup.map((p) => p.to)).size, 2, "新しい名前が衝突しない");
  pass("複数の tmp があっても名前が衝突しない");
}

// ─── [10] modelBrief ────────────────────────────────────────────────────────
console.log("\n[10] modelBrief（規約）");
{
  const b = modelBrief("prop");
  assert.ok(b.materials.some((m) => m.includes("単色マテリアルは禁止")),
    "エンジンが baseColorFactor を読まない件は必ず出す");
  assert.ok(b.rules.some((r) => r.includes("メートル")));
  const lvl = modelBrief("level 床");
  assert.ok(lvl.rules.some((r) => r.includes("rigidBody")), "床なら当たり判定の注意が増える");
  pass("用途に応じた注意が出て、単色マテリアル禁止は必ず入る");
}

// ─── [11] blenderCandidatePaths ─────────────────────────────────────────────
console.log("\n[11] blenderCandidatePaths");
{
  const p = blenderCandidatePaths();
  assert.ok(p[0].includes("Blender 5.2"), "新しい版が先頭");
  assert.ok(p.every((x) => x.endsWith("blender.exe")));
  pass("新しい版から順に候補を返す");
}

console.log(`\nOK: ${passed} 件すべて成功`);
