// jev/qualityGate.ts の単体テスト(ネット不要。エンジン呼び出しと fetch を差し替える)。
// 守りたいのは:
//   1) 合否の規則: ルールの error は blocking / Jev の keep は blocking から外れて keep に判断が残る /
//      uncertain は合否に影響しない / warning は blocking にしない(UI の strict だけ例外)
//   2) 1 往復: bundle:"one" は全検査の質問が 1 リクエスト、"perDomain" は検査ごと、
//      facts がぶつかる plan(落ちたプレイテスト 2 本)だけ別リクエスト
//   3) ルールだけでも同じ形(judge:false / Brief なし / 鍵なし)
//   4) 検査を足す口(GATE_CHECKS の形の検査を渡すと束ねに参加する)、Playing 中は配置を飛ばす、知らない検査は弾く

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { GATE_CHECKS, runQualityGate, type GateCheck } from "./qualityGate.ts";
import type { FetchLike } from "./client.ts";

let failed = 0;
function check(label: string, cond: boolean, detail?: string) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail ? `\n      ${detail}` : ""}`); }
}

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "dx12-jev-gate-"));
const BRIEF = { genre: "一人称ホラー", mood: ["暗い"], avoid: ["明るく均一な照明"], ui: "光って動くボタン" };

// ── 偽エンジン ──
const LAYOUT = { pass: false, checked: 5, errors: 2, warnings: 1, fixed: 0, issues: [
  { kind: "OVERLAP", level: "error", text: "ENV_Book_07 が ENV_Bookshelf_01 に体積比 100% めり込んでいる。", fixed: false,
    entityId: 22, name: "ENV_Book_07", otherEntityId: 21, otherName: "ENV_Bookshelf_01" },
  { kind: "Z_FIGHT", level: "error", text: "LVL_Floor と ENV_Rug の面が Y 軸で 0.00mm しか離れていない", fixed: false, entityId: 25, name: "ENV_Rug" },
  { kind: "NO_COLLIDER", level: "warning", text: "ENV_Grass_03: 一辺 3.0m あるのに当たり判定が無い", fixed: false, entityId: 23, name: "ENV_Grass_03" },
] };
const UI_TREE = { canvases: [{ entityId: 1, name: "Canvas", uiCanvas: { refWidth: 1920, refHeight: 1080 }, children: [
  { entityId: 10, name: "UI_Start", resolvedRect: [840, 500, 240, 64], uiRect: { visible: true }, components: ["uiImage", "uiButton"],
    uiButton: { interactable: true, onClickEvent: "go" }, uiImage: { color: [0.2, 0.2, 0.2, 1], gradientScrollSpeed: 0.8 } },
  { entityId: 11, name: "UI_Tiny", resolvedRect: [1700, 1000, 40, 30], uiRect: { visible: true }, components: ["uiButton"],
    uiButton: { interactable: true, onClickEvent: "x" } },
] }] };
function makeCall(o: { mode?: string; ui?: boolean; validatePass?: boolean; diagError?: boolean } = {}) {
  const calls: { method: string; params: any }[] = [];
  const call = async (method: string, params: any) => {
    calls.push({ method, params });
    switch (method) {
      case "get_mode": return { mode: o.mode ?? "Editor" };
      case "validate_scene": return o.validatePass === false
        ? { pass: false, exitCode: 1, scenePath: "scenes/a.json", report: "FAIL\n[warn] x\n[ERROR] unresolved entity reference: \"Boss\" (trigger action target of WinZone)\n" }
        : { pass: true, exitCode: 0, scenePath: "scenes/a.json", report: "PASS" };
      case "diagnose": return { summary: { errors: o.diagError ? 1 : 0, warnings: 1 }, checks: [
        { id: "lighting", issues: [{ level: 1, text: "影を落とすライトが無い" }, ...(o.diagError ? [{ level: 2, text: "ポイントライトが上限超過" }] : [])] }] };
      case "validate_layout": return LAYOUT;
      case "get_bounds": return { size: params.entity === 21 ? [2, 2.2, 0.4] : [0.3, 0.3, 0.1], center: [0, 0, 0] };
      case "list_entities": return { entities: [
        { entityId: 2, name: "ENV" }, { entityId: 21, name: "ENV_Bookshelf_01" }, { entityId: 22, name: "ENV_Book_07" },
        { entityId: 23, name: "ENV_Grass_03" }, { entityId: 25, name: "ENV_Rug" },
        { entityId: 30, name: "Floor", componentTypes: ["meshRenderer"] }] };
      case "get_hierarchy": return { roots: [{ entityId: 2, children: [{ entityId: 21 }, { entityId: 22 }, { entityId: 23 }, { entityId: 25 }] }, { entityId: 30 }] };
      case "get_scene_settings": return { skybox: { envMapPath: "__procedural_sky__", iblIntensity: 1, drawSkybox: false } };
      case "list_lights": return { lights: [{ type: "Point", intensity: 0.8, castShadows: true }] };
      case "get_volumetric_fog": return { enabled: false, density: 0 };
      case "get_post_process": return { vignetteOn: true };
      case "get_ssao": return { enabled: true };
      case "get_contact_shadow": return { enabled: true };
      case "get_entity": return { material: { roughness: 0.8, metallic: 0 } };
      case "ui_tree": return o.ui === false ? { canvases: [] } : UI_TREE;
      default: throw new Error(`偽エンジンは ${method} を知らない`);
    }
  };
  return { call, calls };
}

// ── 偽 Jev: 本棚の本(layout ref A)・光沢(BUSY_GLOSS)・フォグ無し(NO_FOG)だけ「意図どおり」 ──
function makeJev(o: { lowConfUi?: boolean } = {}) {
  const reqs: any[] = [];
  const fetch: FetchLike = async (_u, init) => {
    const b = JSON.parse(init.body);
    reqs.push(b);
    const answers: Record<string, unknown> = {};
    for (const [k, q] of Object.entries<any>(b.questions)) {
      const t = JSON.stringify(q.instructions);
      if (q.type === "noul") {
        const keep = /flagged issue A /.test(t) || /BUSY_GLOSS/.test(t) || /: NO_FOG =/.test(t);
        answers[k] = { type: "noul", noul: o.lowConfUi && /on the current screen|polish checklist/.test(t) ? 0.66 : keep ? 0.93 : 0.05 };
      } else if (q.type === "choice") {
        const keys = Object.keys(q.criteria);
        const c = keys.includes("add_fog") ? "enable_ssao" : keys.includes("stuck_geometry") ? "stuck_geometry" : keys[0];
        answers[k] = { type: "choice", choice: c, confidence: 0.8, probabilities: {} };
      } else answers[k] = { type: "score", score: 3.4, confidence: 0.9 };
    }
    return { ok: true, status: 200, headers: { get: () => null },
             text: async () => JSON.stringify({ model: "m", answers, usage: { input_tokens: 1500, output_tokens: 30 } }) };
  };
  return { fetch, reqs };
}

const gate = (e: ReturnType<typeof makeCall>, j: ReturnType<typeof makeJev>, opts: any = {}, extra: any = {}) =>
  runQualityGate({ call: e.call, baseDir: TMP, brief: BRIEF, opts: { screenshot: false, ...opts },
                   askOptions: { apiKey: "k", fetch: j.fetch, cache: "off", baseDir: TMP }, ...extra });

console.log("[1] 合否の規則");
{
  const e = makeCall(), j = makeJev();
  const r = await gate(e, j);
  const bcodes = r.blocking.map((b) => `${b.check}:${b.code}`).sort();
  check("ルールの error は blocking(Z_FIGHT / 小さすぎるボタン)、keep した本棚の本は外れる",
    JSON.stringify(bcodes) === JSON.stringify(["layout:Z_FIGHT", "ui:SMALL_HIT_TARGET"]), JSON.stringify(bcodes));
  check("pass は blocking が 0 かどうか", r.pass === false);
  const kb = r.keep.find((k) => k.name === "ENV_Book_07");
  check("keep に判断結果と確信度が残る(本棚の本)", !!kb && kb.judge.question === "layout.intended" && kb.judge.value === 0.93
    && kb.judge.confidence === 0.93 && kb.judge.source === "jev" && !!kb.why, JSON.stringify(kb));
  check("UI の好み(BUSY_GLOSS)と polish の NO_FOG も keep", r.keep.some((k) => k.check === "ui" && k.code === "BUSY_GLOSS")
    && r.keep.some((k) => k.check === "polish" && k.code === "NO_FOG"), JSON.stringify(r.keep.map((k) => `${k.check}:${k.code}`)));
  check("NO_COLLIDER(warning)は blocking にならない", !r.blocking.some((b) => b.code === "NO_COLLIDER"));
  check("polish の指摘は blocking にしない", !r.blocking.some((b) => b.check === "polish"));
  check("suggestions に次の一手(polish の nextFix はそのまま撃てる形)",
    r.suggestions.some((s) => s.check === "polish" && s.tool === "dx12_set_ssao" && !!s.args), JSON.stringify(r.suggestions));
  check("keep にした物があるので layout の fix:\"safe\" は勧めない…のは BURIED/FLOATING のときだけ(今回は OK)",
    r.suggestions.some((s) => s.tool === "dx12_validate_layout"), JSON.stringify(r.suggestions.filter((s) => s.check === "layout")));
  check("checks[] に各検査の要約と judge", ["scene", "layout", "polish", "ui"].every((id) => r.checks.find((c) => c.id === id)?.ran)
    && !!r.checks.find((c) => c.id === "layout")?.judge, JSON.stringify(r.checks.map((c) => [c.id, c.ran, c.skipped])));
  check("playtests は指定しなければ走らない", !r.checks.some((c) => c.id === "playtests"));
  check("cost は 1 リクエストぶん", r.cost.requests === 1 && r.cost.tokens === 1500, JSON.stringify(r.cost));
  check("next に何をすればいいかが書いてある", /blocking 2 件/.test(r.next) && /keep/.test(r.next), r.next);

  const ju = makeJev({ lowConfUi: true });
  const ru = await gate(makeCall(), ju);
  check("uncertain は列挙するだけで合否は変えない / 見るためのツール呼び出し付き",
    ru.uncertain.some((u) => u.check === "ui" && u.look.tool === "dx12_ui_screenshot") && ru.pass === r.pass
    && JSON.stringify(ru.blocking.map((b) => b.code).sort()) === JSON.stringify(r.blocking.map((b) => b.code).sort()), JSON.stringify(ru.uncertain));

  // blocking が無く uncertain だけあるとき: pass は true のまま(uncertain は合否に入れない)
  const jOnly = makeJev({ lowConfUi: true });
  const ro = await gate(makeCall(), jOnly, { checks: ["polish", "ui"] });
  const roNoTiny = ro.blocking.filter((b) => b.code !== "SMALL_HIT_TARGET");
  check("uncertain だけなら合否は変わらない(ui の小さすぎるボタン以外に blocking が無い状態で uncertain がある)",
    roNoTiny.length === 0 && ro.uncertain.length > 0 && ro.pass === (ro.blocking.length === 0), JSON.stringify({ b: ro.blocking.map((x) => x.code), u: ro.uncertain.length }));
  const rp0 = await gate(makeCall(), makeJev({ lowConfUi: true }), { checks: ["polish"] });
  check("blocking 0 + uncertain あり → pass:true", rp0.blocking.length === 0 && rp0.pass === true, JSON.stringify({ u: rp0.uncertain.length }));

  const rs = await gate(makeCall(), makeJev(), { strictness: "strict", judge: false });
  check("UI の strict は warning も blocking(ルールだけのとき BUSY_GLOSS が入る)",
    rs.blocking.some((b) => b.check === "ui" && b.code === "BUSY_GLOSS" && b.level === "warning"), JSON.stringify(rs.blocking.map((b) => `${b.code}:${b.level}`)));
  const rsj = await gate(makeCall(), makeJev(), { strictness: "strict" });
  check("strict でも Jev が keep にした warning は blocking から外れる",
    !rsj.blocking.some((b) => b.code === "BUSY_GLOSS") && rsj.keep.some((k) => k.code === "BUSY_GLOSS"));

  const rv = await gate(makeCall({ validatePass: false, diagError: true }), makeJev());
  check("参照切れと診断のエラーは blocking(文から [ERROR] を剥がす)",
    rv.blocking.some((b) => b.code === "SCENE_REFERENCE" && /^unresolved entity reference/.test(b.text))
    && rv.blocking.some((b) => b.code === "DIAG_LIGHTING"), JSON.stringify(rv.blocking.filter((b) => b.check === "scene")));
  const diag = e.calls.find((c) => c.method === "diagnose");
  check("diagnose は既定で重い検査(textures / models)を外す", !!diag && !/textures|models/.test(diag.params.only) && diag.params.only.length > 0, diag?.params.only);
}

console.log("[2] 1 往復に束ねる");
{
  const j = makeJev();
  await gate(makeCall(), j);
  const qs = Object.values<any>(j.reqs[0].questions).length;
  check("bundle:one = 全検査の質問が 1 リクエスト(state は brief + look/findings/ui/layout)", j.reqs.length === 1 && qs >= 6
    && JSON.stringify(Object.keys(j.reqs[0].state.facts).sort()) === '["findings","layout","look","ui"]', JSON.stringify(Object.keys(j.reqs[0]?.state?.facts ?? {})));
  const jp = makeJev();
  await gate(makeCall(), jp, { bundle: "perDomain" });
  check("bundle:perDomain = 検査ごとの state(polish / ui / layout の 3 リクエスト)", jp.reqs.length === 3,
    JSON.stringify(jp.reqs.map((r: any) => Object.keys(r.state.facts))));

  // 落ちたプレイテストが 2 本 = facts.play が 2 つ → 束ねられないので別リクエスト
  const dir = path.join(TMP, ".dx12", "playtests");
  fs.mkdirSync(dir, { recursive: true });
  for (const n of ["run_a", "run_b"]) {
    fs.writeFileSync(path.join(dir, `${n}.json`), JSON.stringify({ version: 1, name: n, scene: "scenes/a.json", recordedAt: "x", durationSec: 5,
      steps: [{ t: 0, down: "W" }], look: [], reference: [{ t: 0, pos: [0, 1, 0] }, { t: 5, pos: [0, 1, 20] }], endTolerance: 1, pathTolerance: 2, expect: [] }));
  }
  const replay = async () => ({
    verdict: { pass: false, endDistance: 20, maxDeviation: 20, maxDeviationAt: 5, reasons: ["終点が記録から 20m ずれた"] },
    trace: Array.from({ length: 51 }, (_, i) => ({ t: i / 10, pos: [0, 1, 0] })),
  });
  const jr = makeJev();
  const rr = await gate(makeCall(), jr, { playtests: true }, { replay });
  check("落ちたプレイテストは blocking(再生の合否はルールのまま)", rr.blocking.filter((b) => b.code === "PLAYTEST_FAILED").length === 2);
  check("facts.play がぶつかる 2 本は別リクエスト(計 2 リクエスト)", jr.reqs.length === 2 && rr.judge.bundles === 2, `${jr.reqs.length} req`);
  check("落ちた原因が suggestions に入る", rr.suggestions.some((s) => s.check === "playtests" && /引っかかる/.test(s.text)), JSON.stringify(rr.suggestions));
  check("cost は束ねた全リクエストの合計", rr.cost.requests === 2 && rr.cost.tokens === 3000);
  const jn = makeJev();
  const rn = await gate(makeCall(), jn, { playtests: ["run_b"] }, { replay });
  check("名前で絞れる", rn.blocking.filter((b) => b.code === "PLAYTEST_FAILED").length === 1 && jn.reqs.length === 1);
}

console.log("[3] ルールだけでも同じ形");
{
  const j = makeJev();
  const r = await gate(makeCall(), j, { judge: false });
  check("judge:false → Jev に出ない・keep は無い・ルールの error は全部 blocking", j.reqs.length === 0 && r.keep.length === 0
    && r.blocking.some((b) => b.name === "ENV_Book_07") && r.judge.used === false && r.cost.requests === 0);
  const jb = makeJev();
  const rb = await runQualityGate({ call: makeCall().call, baseDir: TMP, brief: null, opts: { screenshot: false },
                                    askOptions: { apiKey: "k", fetch: jb.fetch, cache: "off", baseDir: TMP } });
  check("Brief なし → Jev に出ず briefMissing、次の一手に dx12_brief", jb.reqs.length === 0 && rb.judge.briefMissing === true
    && /dx12_brief/.test(rb.next) && rb.judge.source === "rules");
  check("Brief なしでも NO_COLLIDER はグループ規約で keep(ENV の草)", rb.keep.some((k) => k.code === "NO_COLLIDER" && k.judge.source === "rules"
    && k.judge.confidence === null), JSON.stringify(rb.keep));
  const saved = process.env.TYPESAFE_API_KEY;
  delete process.env.TYPESAFE_API_KEY;
  const jk = makeJev();
  const rk = await runQualityGate({ call: makeCall().call, baseDir: TMP, brief: BRIEF, opts: { screenshot: false },
                                    askOptions: { fetch: jk.fetch, cache: "off", baseDir: TMP } });
  if (saved !== undefined) process.env.TYPESAFE_API_KEY = saved;
  check("鍵なし → Jev に出ずルールで同じ形", jk.reqs.length === 0 && rk.judge.source === "rules" && Array.isArray(rk.blocking));
}

console.log("[4] 検査を足す口 / 状態による分岐");
{
  const extra: GateCheck = {
    id: "custom", title: "足した検査", enabled: () => true,
    run: async () => ({
      items: [{ check: "custom", code: "X_FLAGGED", level: "error", blocking: true, text: "足した検査のエラー" }],
      judges: [{
        plan: { facts: { custom: { note: "足した検査の事実" } }, refs: ["look.brief_fit"] },
        interpret: (res) => ({ judge: { got: res.length }, keep: [{ index: 0, question: "look.brief_fit", value: 0.9, threshold: null, source: "jev", why: "足した検査の判断" }],
                               uncertain: [], suggestions: [{ check: "custom", text: "足した検査の次の一手" }] }),
      }],
    }),
  };
  const j = makeJev();
  const r = await gate(makeCall(), j, {}, { checks: [...GATE_CHECKS, extra] });
  check("足した検査も走り、質問は同じ 1 リクエストに束ねる", j.reqs.length === 1 && "custom" in j.reqs[0].state.facts === false
    && r.checks.some((c) => c.id === "custom" && c.ran), JSON.stringify(Object.keys(j.reqs[0]?.state?.facts ?? {})));
  check("足した検査の keep も blocking から外れる", r.keep.some((k) => k.code === "X_FLAGGED") && !r.blocking.some((b) => b.code === "X_FLAGGED"));
  check("足した検査の次の一手も並ぶ", r.suggestions.some((s) => s.check === "custom"));

  const jp = makeJev();
  const rp = await gate(makeCall({ mode: "Playing" }), jp);
  const lay = rp.checks.find((c) => c.id === "layout");
  check("Playing 中は配置検査を飛ばす(理由付き)", !!lay && lay.ran === false && /dx12_stop/.test(lay.skipped ?? ""));
  const rn = await gate(makeCall({ ui: false }), makeJev());
  check("UI が無ければ ui は飛ばす", rn.checks.find((c) => c.id === "ui")?.ran === false);
  let threw = "";
  try { await gate(makeCall(), makeJev(), { checks: ["nope"] }); } catch (e: any) { threw = e.message; }
  check("知らない検査は有効な id つきで弾く", /nope/.test(threw) && /layout/.test(threw), threw);
  const only = await gate(makeCall(), makeJev(), { checks: ["layout"] });
  check("checks で絞れる", only.checks.length === 1 && only.checks[0].id === "layout");
  const broken: GateCheck = { id: "boom", title: "壊れた検査", enabled: () => true, run: async () => { throw new Error("爆発"); } };
  const rb = await gate(makeCall(), makeJev(), {}, { checks: [broken, ...GATE_CHECKS] });
  check("検査が例外を投げてもゲートは止まらない(skipped に理由)", rb.checks[0].ran === false && /爆発/.test(rb.checks[0].skipped ?? ""));
}

fs.rmSync(TMP, { recursive: true, force: true });
console.log(failed === 0 ? "\nOK: jev/qualityGate テストすべて通過" : `\nNG: ${failed} 件失敗`);
process.exit(failed === 0 ? 0 : 1);
