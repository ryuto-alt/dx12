/**
 * BUG-13(W05-41): dx12_vfx_preview がトランザクションで包まれ、最後に必ず rollback することの e2e テスト。
 * 偽エンジンは undoDepth / 保存回数 / トランザクションの開閉を持つ。ネット不要・エディタ不要。
 *
 * 実行: node vfxPreviewTx.test.ts
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import { PNG } from "pngjs";
import { McpStdio, payload, startFakeEngine } from "./jev/testHarness.ts";

let passed = 0;
const pass = (label: string) => { passed++; console.log(`  OK  ${label}`); };

const st = { open: false, undoDepth: 3, saves: 0, failScreenshot: false, noTx: false, pending: 0, rbFail: false, foreignAt: 0, beginErr: "", beginTimeoutOpen: false, names: [] as string[] };
const png = (() => { const p = new PNG({ width: 8, height: 8 }); p.data.fill(60); return PNG.sync.write(p); })();
const err = (code: number, message: string) => Object.assign(new Error(message), { code });

function handler(method: string, params: any): any {
  switch (method) {
    case "get_entity": return { entityId: 5, name: "Fire", transform: { position: [0, 0, 0] }, particleEmitter: { layers: [{ looping: true, lifetime: 1 }] } };
    case "transaction_begin":
      if (st.noTx) throw err(1, "unknown method: transaction_begin");
      if (st.beginErr) { if (st.beginTimeoutOpen) { st.open = true; st.pending = 0; } throw err(3, st.beginErr); }
      if (st.open) throw err(3, "transaction already open");
      st.open = true; st.pending = 0; return { open: true };
    case "transaction_rollback":
      if (!st.open) throw err(3, "no open transaction");
      if (st.rbFail) { st.open = false; throw err(3, "no open transaction (closed by Play)"); }
      { const calls = st.pending + st.foreignAt; st.open = false; st.pending = 0; return { rolledBack: true, calls }; }
    case "transaction_status":
      return { open: st.open, label: st.open ? "vfx_preview" : undefined, callNames: [...Array(st.pending).fill("set_transform"), ...Array(st.foreignAt).fill("other_client_write"), ...st.names], humanEditsDuringTransaction: 0 };
    case "transaction_commit":
      st.open = false; st.undoDepth += 1; return { committed: true };
    case "set_transform": case "set_component":
      if (st.open) st.pending++; else st.undoDepth++;
      return { ok: true };
    case "set_editor_camera": case "step_frames": return { ok: true };
    case "screenshot_final":
      if (st.failScreenshot) throw err(9, "engine error: screenshot failed");
      fs.writeFileSync(params.path, png); return { path: params.path };
    case "save_scene": st.saves++; return { saved: true };
    default: return undefined;
  }
}

const engine = await startFakeEngine(handler);
const mcp = new McpStdio({ enginePort: engine.port, jevUrl: "http://127.0.0.1:9/none", key: "" });
await mcp.init();
const seq = (n: number) => engine.received.slice(n).map((x) => x.method);

try {
  console.log("[1] 成功経路");
  {
    const n = engine.received.length, depth = st.undoDepth;
    payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    const m = seq(n);
    assert.equal(m[1], "transaction_begin", "get_entity の直後に begin");
    assert.equal(m[m.length - 1], "transaction_rollback", "最後は rollback");
    assert.ok(m.includes("set_transform"), "退避の set_transform が走っている");
    assert.equal(m.filter((x) => x === "transaction_commit").length, 0, "commit しない");
    assert.equal(st.undoDepth, depth, "undoDepth 不変");
    assert.equal(st.open, false);
    // 注意: st.saves は明示の save_scene しか数えない。自動保存(D16/G9)は実エンジンでしか確認できない。
    assert.equal(st.saves, 0, "明示の save_scene は呼ばない");
    pass("begin → 撮影 → rollback。undoDepth 不変・保存なし");
  }
  console.log("[2] エンジンのエラー(失敗経路)でも rollback");
  {
    st.failScreenshot = true;
    const n = engine.received.length, depth = st.undoDepth;
    const r = await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000);
    assert.equal(r.result.isError, true);
    const m = seq(n);
    assert.equal(m[m.length - 1], "transaction_rollback");
    assert.equal(st.open, false);
    assert.equal(st.undoDepth, depth);
    st.failScreenshot = false;
    pass("screenshot 失敗でも rollback が走り undoDepth 不変");
  }
  console.log("[3] トランザクションの無い古いエンジン");
  {
    st.noTx = true;
    const n = engine.received.length;
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(!seq(n).includes("transaction_rollback"));
    assert.ok(out.notes?.some((x: string) => /トランザクション/.test(x)));
    st.noTx = false;
    pass("begin に失敗したら rollback せず、従来どおり個別復元(note に理由)");
  }
  console.log("[4] rollback が他の書き込みも破棄したら警告");
  {
    st.foreignAt = 2;
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(out.notes?.some((x: string) => /破棄された可能性/.test(x)), "calls 不一致の警告");
    st.foreignAt = 0;
    pass("rollback の calls が自分の書き込みより多ければ notes に警告");
  }
  console.log("[5] rollback 失敗は結果に載せる");
  {
    st.rbFail = true;
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(out.notes?.some((x: string) => /rollback に失敗/.test(x)));
    st.rbFail = false;
    pass("rollback の失敗が notes に出る");
  }
  console.log("[6] 他人のトランザクションが開いている");
  {
    st.open = true;
    const n = engine.received.length;
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(!seq(n).includes("transaction_rollback"), "他人のトランザクションは rollback しない");
    assert.ok(out.notes?.some((x: string) => /既に開いている/.test(x)), "別トランザクションの note");
    st.open = false;
    pass("他人のトランザクションが開いていれば、そう note して rollback しない");
  }
  console.log("[7] fire:true で他者の書き込み(callNames)を検出");
  {
    st.names = ["other_client_write"];
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(out.notes?.some((x: string) => /破棄された可能性/.test(x) && /other_client_write/.test(x)), "callNames の混入を警告");
    st.names = [];
    pass("callNames に自分以外の呼び出しがあれば警告");
  }
  console.log("[8] Play 中の begin 失敗は別トランザクション扱いにしない");
  {
    st.beginErr = "cannot begin a transaction while Playing";
    const n = engine.received.length;
    const out = payload(await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000));
    assert.ok(out.notes?.some((x: string) => /開けなかった/.test(x)) && !out.notes?.some((x: string) => /既に開いている/.test(x)));
    assert.ok(!seq(n).includes("transaction_rollback"));
    st.beginErr = "";
    pass("分類不能な begin 失敗は別トランザクションと誤認しない");
  }
  console.log("[9] begin が失敗応答だが実際は開いた(タイムアウト相当)");
  {
    st.beginErr = "timeout"; st.beginTimeoutOpen = true;
    const n = engine.received.length;
    await mcp.call("dx12_vfx_preview", { name: "Fire", frames: 2, seconds: 0.2 }, 60000);
    assert.ok(seq(n).includes("transaction_rollback"), "自分のものとして rollback");
    assert.equal(st.open, false, "開きっぱなしにしない");
    st.beginErr = ""; st.beginTimeoutOpen = false;
    pass("実際は開いていた begin を adopt して rollback する");
  }
} catch (e) {
  console.log(`  NG  ${(e as Error).message}`);
  mcp.kill(); engine.server.close();
  process.exit(1);
}
mcp.kill(); engine.server.close();
console.log(`\nOK: vfx_preview トランザクション e2e テスト ${passed} 項目すべて通過`);
process.exit(0);
