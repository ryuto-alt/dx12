/**
 * 判断段を足したツールの e2e テスト。ネット不要・エディタ不要。
 *
 * index.ts を子プロセスで起動し、偽エンジン(TCP)と偽 Jev(HTTP)に繋ぐ(部品は jev/testHarness.ts)。
 * 担保すること:
 *   [ui] dx12_ui_audit: 1 往復で brief_fit + 好みの指摘コードが届き、judge が付く。既存フィールドは壊れない。
 *        judge:false で Jev に出ない。Brief が無ければ judge.source="rules" + briefMissing。
 *   [layout] dx12_validate_layout: 聞く種類(OVERLAP 等)だけが 1 往復で届き、明らかな欠陥(Z_FIGHT)は聞かない。
 *        大きさはその物だけ get_bounds で測る。エンジンの結果(pass / errors / issues)は壊れない。judge:false で止まる。
 *
 * 実行: node jev/judgeTools.test.ts
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { McpStdio, payload, startFakeEngine, startFakeJev } from "./testHarness.ts";

const FAKE_KEY = "apikey_e2e_judge_tools_fake_value_77";
let passed = 0;
const pass = (label: string) => { passed++; console.log(`  OK  ${label}`); };

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "dx12-jev-judge-tools-"));
const PROJ = path.join(TMP, "proj");
fs.mkdirSync(path.join(PROJ, "assets"), { recursive: true });
fs.writeFileSync(path.join(PROJ, "brief.json"), JSON.stringify({
  genre: "ソシャゲのガチャ画面", mood: ["にぎやか", "きらきら"], avoid: ["地味な画面"], ui: "光って動くボタン",
}));

// ── 偽エンジンの UI: 中央揃えのボタン 6 個(うち 1 個は光沢が速い)+ 小さすぎるボタン 1 個 ──
const button = (id: number, name: string, rect: number[], img: any = {}) => ({
  entityId: id, name, resolvedRect: rect, uiRect: { visible: true }, components: ["uiImage", "uiButton"],
  uiButton: { interactable: true, onClickEvent: "go" },
  uiImage: { color: [0.2, 0.2, 0.2, 1], gradientDir: 0, outlineWidth: 0, shadowAlpha: 0, shape: 0, ...img },
});
const UI_TREE = { canvases: [{ entityId: 1, name: "Canvas", uiCanvas: { refWidth: 1920, refHeight: 1080 }, children: [
  ...Array.from({ length: 6 }, (_, i) => button(10 + i, `UI_Gacha_0${i + 1}`, [840, 200 + i * 100, 240, 64],
    i === 0 ? { gradientScrollSpeed: 0.8 } : {})),
  { entityId: 30, name: "UI_Tiny", resolvedRect: [1700, 1000, 40, 30], uiRect: { visible: true }, components: ["uiButton"],
    uiButton: { interactable: true, onClickEvent: "x" } },
] }] };

// ── 偽エンジンの配置: 本棚の中の本(OVERLAP・意図どおり)/ 壁に埋まったスライム(OVERLAP)/ 床と絨毯のちらつき(Z_FIGHT) ──
const LAYOUT_REPORT = { pass: false, checked: 6, errors: 2, warnings: 1, fixed: 0, sceneGeneration: 3, issues: [
  { kind: "OVERLAP", level: "error", text: "ENV_Book_07 が ENV_Bookshelf_01 に体積比 100% めり込んでいる。どちらかをずらすか片方を消すこと",
    fixed: false, entityId: 22, name: "ENV_Book_07", otherEntityId: 21, otherName: "ENV_Bookshelf_01" },
  { kind: "OVERLAP", level: "warning", text: "GP_Enemy_Slime_01 が LVL_Wall_02 に体積比 45% めり込んでいる。どちらかをずらすか片方を消すこと",
    fixed: false, entityId: 30, name: "GP_Enemy_Slime_01", otherEntityId: 11, otherName: "LVL_Wall_02" },
  { kind: "Z_FIGHT", level: "error", text: "LVL_Floor と ENV_Rug の面が Y 軸で 0.00mm しか離れていない", fixed: false,
    entityId: 25, name: "ENV_Rug", otherEntityId: 10, otherName: "LVL_Floor" },
] };
const ENTS = [
  { entityId: 1, name: "LVL" }, { entityId: 2, name: "ENV" }, { entityId: 3, name: "GAMEPLAY" },
  { entityId: 10, name: "LVL_Floor", parent: 1 }, { entityId: 11, name: "LVL_Wall_02", parent: 1 },
  { entityId: 21, name: "ENV_Bookshelf_01", parent: 2 }, { entityId: 22, name: "ENV_Book_07", parent: 2 },
  { entityId: 25, name: "ENV_Rug", parent: 2 }, { entityId: 30, name: "GP_Enemy_Slime_01", parent: 3 },
];
const hierNode = (id: number): any => ({ entityId: id, children: ENTS.filter((e) => e.parent === id).map((e) => hierNode(e.entityId)) });

function engineHandler(method: string, params: any): any {
  switch (method) {
    case "ping": return { pong: true, mode: "Editor", protocolVersion: 4, baseDir: PROJ, assetsDir: path.join(PROJ, "assets"), cwd: TMP };
    case "ui_tree": return UI_TREE;
    case "validate_layout": return LAYOUT_REPORT;
    case "get_bounds": return { size: params.entity === 21 ? [2, 2.2, 0.4] : [0.5, 0.5, 0.5], center: [0, 0, 0], min: [0, 0, 0], max: [1, 1, 1] };
    case "list_entities": return { entities: ENTS.map((e) => ({ entityId: e.entityId, name: e.name, componentTypes: [] })) };
    case "get_hierarchy": return { roots: ENTS.filter((e) => e.parent === undefined).map((e) => hierNode(e.entityId)) };
    default: return undefined;
  }
}

const engine = await startFakeEngine(engineHandler);
// 光沢(BUSY_GLOSS)だけ「意図どおり」と答える偽 Jev
const jev = await startFakeJev({
  noul: (text) => (text.includes("BUSY_GLOSS") ? 0.94 : 0.08),
  score: () => ({ score: 3.3, confidence: 0.85 }),
});
const mcp = new McpStdio({ enginePort: engine.port, jevUrl: jev.url, key: FAKE_KEY });
await mcp.init();

try {
  console.log("[ui] dx12_ui_audit の判断段");
  {
    const before = jev.reqs.length;
    const r = payload(await mcp.call("dx12_ui_audit", { strictness: "strict", screen: "title" }));
    assert.equal(typeof r.score, "number");
    assert.equal(typeof r.grade, "string");
    assert.equal(r.pass, false);
    assert.ok(Array.isArray(r.issues) && r.metrics, "既存の issues / metrics は従来どおり");
    const codes = new Set(r.issues.map((i: any) => i.code));
    assert.ok(codes.has("BUSY_GLOSS") && codes.has("CENTERED_MONOTONY") && codes.has("SMALL_HIT_TARGET"), [...codes].join(","));
    pass("pass / score / grade / issues / metrics は従来どおり");

    assert.equal(jev.reqs.length - before, 1, "判断段は 1 往復");
    const req = jev.reqs[jev.reqs.length - 1];
    assert.equal(req.auth, `Bearer ${FAKE_KEY}`);
    assert.equal(req.state.brief.genre, "ソシャゲのガチャ画面", "brief.json が state に入る");
    assert.deepEqual(Object.keys(req.state.facts), ["ui"]);
    assert.equal(req.state.facts.ui.screen, "タイトル画面");
    assert.ok(!/[0-9]/.test(JSON.stringify({ ...req.state.facts.ui, issues: [] })), "ui の語に数字が無い");
    const askedCodes = Object.values<any>(req.questions).map((q) => (JSON.stringify(q.instructions).match(/screen: ([A-Z_]+)/) ?? [])[1]).filter(Boolean);
    assert.ok(!askedCodes.includes("SMALL_HIT_TARGET"), "機能の欠陥は聞かない");
    assert.equal(Object.keys(req.questions).length, 1 + askedCodes.length);
    pass("1 リクエストで brief_fit + 好みの指摘コードだけ。state は Brief + 数値を含まない言葉");

    const j = r.judge;
    assert.equal(j.source, "jev");
    assert.equal(j.briefFit.value, 3.3);
    assert.deepEqual(j.findings.filter((f: any) => f.keep).map((f: any) => f.code), ["BUSY_GLOSS"]);
    assert.ok(j.notAsked.includes("SMALL_HIT_TARGET"));
    assert.ok(j.scoreExcludingKept > r.score);
    assert.equal(j.passExcludingKept, false, "エラー(小さすぎるボタン)は keep でも消えない");
    assert.equal(j.cost.requests, 1);
    pass("judge: {source:jev, briefFit, findings[keep], notAsked, scoreExcludingKept, cost}");

    const before2 = jev.reqs.length;
    const off = payload(await mcp.call("dx12_ui_audit", { judge: false }));
    assert.equal(off.judge, undefined);
    assert.equal(jev.reqs.length, before2);
    pass("judge:false で判断段を止める(Jev に出ない)");

    fs.renameSync(path.join(PROJ, "brief.json"), path.join(PROJ, "brief.json.bak"));
    try {
      const nb = payload(await mcp.call("dx12_ui_audit", {}));
      assert.equal(jev.reqs.length, before2, "Brief が無ければ聞かない");
      assert.equal(nb.judge.source, "rules");
      assert.equal(nb.judge.briefMissing, true);
      assert.equal(nb.judge.scoreExcludingKept, nb.score);
      pass("Brief が無い → judge.source:rules + briefMissing(従来どおり全部直す)");
    } finally {
      fs.renameSync(path.join(PROJ, "brief.json.bak"), path.join(PROJ, "brief.json"));
    }

    const unknown = await mcp.call("dx12_ui_audit", { judgee: false });
    assert.equal(unknown.result.isError, true, "未知の引数は近い正解つきのエラー");
    pass("未知の引数は弾く(regRaw の流儀)");
  }

  console.log("[layout] dx12_validate_layout の判断段");
  {
    // 本棚の本(ref A)だけ「意図どおり」と答える
    jev.cfg.answers = { noul: (text) => (text.includes("flagged issue A ") ? 0.93 : 0.05) };
    const before = jev.reqs.length;
    const boundsBefore = engine.received.filter((r) => r.method === "get_bounds").length;
    const r = payload(await mcp.call("dx12_validate_layout", {}));
    assert.equal(r.pass, false);
    assert.equal(r.errors, 2);
    assert.equal(r.issues.length, 3, "エンジンの issues は従来どおり");
    pass("pass / errors / issues はエンジンの返り値のまま");

    assert.equal(jev.reqs.length - before, 1, "判断段は 1 往復");
    const req = jev.reqs[jev.reqs.length - 1];
    assert.deepEqual(Object.keys(req.state.facts), ["layout"]);
    assert.equal(Object.keys(req.questions).length, 2, "OVERLAP 2 件だけ聞く(Z_FIGHT は聞かない)");
    assert.ok(!/[0-9]/.test(JSON.stringify(req.state.facts.layout)), "layout の語に数字が無い");
    const measured = engine.received.filter((x) => x.method === "get_bounds").slice(boundsBefore).map((x) => x.params.entity).sort((a: number, b: number) => a - b);
    assert.deepEqual(measured, [11, 21, 22, 30], "大きさは聞く指摘に出てくる物だけ測る");
    pass("1 リクエストで聞く種類の指摘だけ。state は Brief + 数値を含まない言葉");

    const j = r.judge;
    assert.equal(j.source, "jev");
    assert.deepEqual(j.findings.filter((f: any) => f.keep).map((f: any) => f.name), ["ENV_Book_07"]);
    assert.deepEqual(j.notAsked, ["Z_FIGHT"]);
    assert.equal(j.errorsExcludingKept, 1, "本棚の本(エラー)は keep で外れ、Z_FIGHT は残る");
    assert.equal(j.passExcludingKept, false);
    pass("judge: {source:jev, findings[keep], notAsked:[Z_FIGHT], errorsExcludingKept, passExcludingKept}");

    const before2 = jev.reqs.length;
    const off = payload(await mcp.call("dx12_validate_layout", { judge: false, fix: "none" }));
    assert.equal(off.judge, undefined);
    assert.equal(jev.reqs.length, before2);
    const sent = engine.received.filter((x) => x.method === "validate_layout").pop()!;
    assert.equal(sent.params.judge, undefined, "judge はエンジンへ送らない");
    assert.equal(sent.params.fix, "none");
    pass("judge:false で止まる。judge はエンジンへ渡さない");
  }
} catch (e) {
  console.log(`  NG  ${(e as Error).message}`);
  mcp.kill(); engine.server.close(); jev.server.close();
  process.exit(1);
}

mcp.kill();
engine.server.close();
jev.server.close();
fs.rmSync(TMP, { recursive: true, force: true });
console.log(`\nOK: 判断段ツール e2e テスト ${passed} 項目すべて通過`);
process.exit(0);
