// W05-38: ドッグフードの道具の単体テスト（エンジン不要・LLM 不要）。実行: node tools/dogfood/collect.test.mjs
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import {
  validateChecklist, validateResults, deriveBugs, nextBugNumber, makeBugId, collect, evaluateGreen, exitCodeOf,
  SEVERITIES, STATUSES, BUG_STATUSES, EVIDENCE_KINDS, CHECK_REQUIRED, RESULT_REQUIRED, BUG_REQUIRED, STEP_KINDS, FILE_BASES,
} from "./collect.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
let failed = 0;
function check(label, cond, detail) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? "\n      " + JSON.stringify(detail).slice(0, 500) : ""}`); }
}
const load = (w) => JSON.parse(fs.readFileSync(path.join(here, "checklists", `${w}.json`), "utf8"));

// [1] 実チェックリストの検査と全 ID の存在（EXEC 1.5 と 2.9）
const w05 = load("w05"), w1 = load("w1");
check("w05.json が検証を通る", validateChecklist(w05).length === 0, validateChecklist(w05));
check("w1.json が検証を通る", validateChecklist(w1).length === 0, validateChecklist(w1));
const want05 = ["D1", "D1b", ...Array.from({ length: 15 }, (_, i) => `D${i + 2}`)];
check("w05 に D1, D1b, D2〜D16 が全部ある", want05.every((id) => w05.checks.some((c) => c.id === id)) && w05.checks.length === want05.length, w05.checks.map((c) => c.id));
const want1 = Array.from({ length: 20 }, (_, i) => `G${i + 1}`);
check("w1 に G1〜G20 が全部ある", want1.every((id) => w1.checks.some((c) => c.id === id)) && w1.checks.length === 20);
const dg05a = w05.checks.filter((c) => c.gate?.includes("dg05a")).map((c) => c.id).sort().join(",");
check("DG05-a の組が D1, D1b, D2〜D8, D16", dg05a === ["D1", "D1b", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D16"].sort().join(","), dg05a);
check("w05 の全チェックが steps/expect/evidence/severity を持つ", w05.checks.every((c) => c.steps.length && c.expect && c.evidence.length && c.severityIfFail));

// [2] 検証が悪い入力を落とす
const mk = (over) => ({ wave: "t", version: 1, checks: [{ id: "D1", area: "a", steps: [{ kind: "note", text: "x" }], expect: "e", evidence: ["json"], severityIfFail: "major", ...over }] });
check("正常な最小形は通る", validateChecklist(mk({})).length === 0);
check("重大度の不正を落とす", validateChecklist(mk({ severityIfFail: "fatal" })).length > 0);
check("steps が空を落とす", validateChecklist(mk({ steps: [] })).length > 0);
check("未知の kind を落とす", validateChecklist(mk({ steps: [{ kind: "click", x: 1 }] })).length > 0);
check("実マウスの語を含む手順を落とす", validateChecklist(mk({ steps: [{ kind: "note", text: "SetCursorPos で動かす" }] })).length > 0);
check("不明なキーを落とす", validateChecklist(mk({ steps: [{ kind: "wait", ms: 1, extra: 1 }] })).length > 0);
check("id の形の不正を落とす", validateChecklist(mk({ id: "bad" })).length > 0);
check("evidence の不正を落とす", validateChecklist(mk({ evidence: ["video"] })).length > 0);
check("id の重複を落とす", validateChecklist({ ...mk({}), checks: [mk({}).checks[0], mk({}).checks[0]] }).length > 0);

// [3] 結果の検証
const cl = mk({});
const runner = { model: "m", promptVersion: "1" };
check("fail に observed が無いと落ちる", validateResults({ wave: "t", runner, results: [{ id: "D1", status: "fail" }] }, cl).length > 0);
check("未知の ID を落とす", validateResults({ wave: "t", runner, results: [{ id: "Z9", status: "pass" }] }, cl).length > 0);
check("runner が無いと落ちる", validateResults({ wave: "t", results: [] }, cl).length > 0);
check("正常な結果は通る", validateResults({ wave: "t", runner, results: [{ id: "D1", status: "pass" }] }, cl).length === 0);

// [4] 不具合の採番
check("採番は DF-W05-01 の形", makeBugId("w05", 1) === "DF-W05-01" && makeBugId("w1", 12) === "DF-W1-12");
check("次の番号は最大 + 1", nextBugNumber([{ id: "DF-W05-03" }, { id: "DF-W05-01" }, { id: "DF-W1-09" }], "w05") === 4);
const cl2 = { wave: "w05", version: 1, checks: [
  { id: "D1", area: "data-loss", title: "t1", steps: [{ kind: "note", text: "x" }], expect: "e1", evidence: ["json"], severityIfFail: "blocker", target: ["W05-02"] },
  { id: "D2", area: "x", title: "t2", steps: [{ kind: "note", text: "x" }], expect: "e2", evidence: ["json"], severityIfFail: "minor" }] };
const res1 = { wave: "w05", runner, results: [{ id: "D1", status: "fail", observed: "壊れた" }, { id: "D2", status: "pass" }] };
let bugs = deriveBugs(cl2, res1, [], "T");
check("fail から DF-W05-01 を作る", bugs.length === 1 && bugs[0].id === "DF-W05-01" && bugs[0].severity === "blocker" && bugs[0].actual === "壊れた" && bugs[0].target[0] === "W05-02", bugs);
bugs = deriveBugs(cl2, res1, bugs, "T2");
check("同じ失敗の再実行で不具合が増えない", bugs.length === 1);
bugs = deriveBugs(cl2, { wave: "w05", runner, results: [{ id: "D1", status: "pass" }] }, bugs, "T3");
check("直ったら verified になる", bugs[0].status === "verified");
bugs = deriveBugs(cl2, { wave: "w05", runner, results: [{ id: "D1", status: "fail", observed: "再発" }, { id: "D2", status: "fail", observed: "b" }] }, bugs, "T4");
check("再発は新しい番号（DF-W05-02, 03）", bugs.map((b) => b.id).join() === "DF-W05-01,DF-W05-02,DF-W05-03", bugs.map((b) => b.id));

// [5] collect: 証拠のコピー、root 外の拒否、結果のマージ
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "dogfood_"));
try {
  const root = path.join(tmp, "run"), out = path.join(tmp, "out");
  fs.mkdirSync(root, { recursive: true });
  fs.writeFileSync(path.join(root, "a.png"), "png");
  fs.writeFileSync(path.join(root, "a.json"), "{}");
  fs.writeFileSync(path.join(tmp, "secret.txt"), "x");
  fs.writeFileSync(path.join(tmp, "cl.json"), JSON.stringify(cl2));
  const r = { wave: "w05", runner, results: [
    { id: "D1", status: "fail", observed: "x", evidence: [{ kind: "json", path: "a.json" }] },
    { id: "D2", status: "pass", evidence: [{ kind: "json", path: "../secret.txt" }] }] };
  fs.writeFileSync(path.join(tmp, "res.json"), JSON.stringify(r));
  const s = collect({ checklistPath: path.join(tmp, "cl.json"), resultsPath: path.join(tmp, "res.json"), evidenceRoot: root, outDir: out, now: "T" });
  check("証拠がコピーされる", fs.existsSync(path.join(out, "json", "D1_a.json")));
  check("root 外の証拠は拒否され missing に出る", s.missingEvidence.some((m) => m.includes("root の外")) && !fs.existsSync(path.join(out, "json", "D2_secret.txt")), s);
  check("bugs.json に DF-W05-01、openBlockerMajor = 1", JSON.parse(fs.readFileSync(path.join(out, "bugs.json"), "utf8"))[0].id === "DF-W05-01" && s.openBlockerMajor === 1);
  // 再実行（D1 だけ pass）→ results がマージされ D2 が残る
  fs.writeFileSync(path.join(tmp, "res2.json"), JSON.stringify({ wave: "w05", runner, results: [{ id: "D1", status: "pass", evidence: [{ kind: "json", path: "a.json" }] }] }));
  const s2 = collect({ checklistPath: path.join(tmp, "cl.json"), resultsPath: path.join(tmp, "res2.json"), evidenceRoot: root, outDir: out, now: "T2" });
  const merged = JSON.parse(fs.readFileSync(path.join(out, "results.json"), "utf8"));
  check("再実行で結果がマージされる（D1 pass、D2 が残る）", merged.results.find((x) => x.id === "D1").status === "pass" && merged.results.some((x) => x.id === "D2"));
  check("再実行で不具合が verified、open 0", s2.openBugs === 0 && JSON.parse(fs.readFileSync(path.join(out, "bugs.json"), "utf8"))[0].status === "verified", s2);
  // CLI
  const cli = spawnSync(process.execPath, [path.join(here, "collect.mjs"), "validate-checklist"], { encoding: "utf8" });
  check("CLI validate-checklist が 0 で終わる", cli.status === 0, cli.stdout + cli.stderr);
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}

// [7] 不具合は ID + DPI の単位（別の DPI の pass で閉じない）
{
  const cl7 = { wave: "w05", version: 1, checks: [
    { id: "D10", area: "dpi", title: "dpi", steps: [{ kind: "note", text: "x" }], expect: "e", evidence: ["json"], severityIfFail: "blocker", dpi: [1, 1.5, 2] }] };
  const R = (results) => ({ wave: "w05", runner, results });
  let b = deriveBugs(cl7, R([{ id: "D10", dpi: 1.5, status: "fail", observed: "x1.5", evidence: [{ kind: "json", path: "a", collected: "json/a" }] }, { id: "D10", dpi: 1, status: "pass" }]), [], "T");
  check("1.5 で fail・1.0 で pass の同じ実行で、1.5 の不具合が open のまま", b.length === 1 && b[0].status === "open" && b[0].dpi === 1.5, b);
  b = deriveBugs(cl7, R([{ id: "D10", dpi: 1, status: "pass" }]), b, "T2");
  check("1.0 だけの再実行では 1.5 の不具合が閉じない", b[0].status === "open", b);
  b = deriveBugs(cl7, R([{ id: "D10", dpi: 2, status: "fail", observed: "x2" }]), b, "T3");
  check("別の DPI の fail は別の不具合（observed と証拠を落とさない）", b.length === 2 && b[1].dpi === 2 && b[1].actual === "x2" && b[0].evidence[0] === "json/a", b);
  b = deriveBugs(cl7, R([{ id: "D10", dpi: 1.5, status: "pass" }]), b, "T4");
  check("1.5 が pass したときだけ 1.5 の不具合が verified（2.0 は open のまま）", b[0].status === "verified" && b[1].status === "open", b);
  b = deriveBugs(cl7, R([{ id: "D10", dpi: 1.5, status: "fail", observed: "a" }, { id: "D10", dpi: 2, status: "fail", observed: "b" }]), [], "T5");
  check("同じ実行で 2 つの DPI が fail すると不具合が 2 件で、どちらも observed を持つ", b.length === 2 && b.map((x) => x.actual).join() === "a,b", b);
}

// [8] ゲートの判定（全チェックが緑）
{
  const mkc = (id, sev, over = {}) => ({ id, area: "a", steps: [{ kind: "note", text: "x" }], expect: "e", evidence: ["json"], severityIfFail: sev, ...over });
  const cl8 = { wave: "w05", version: 1, checks: [mkc("D1", "blocker", { gate: ["dg05a"] }), mkc("D2", "major", { gate: ["dg05a"] }), mkc("D9", "minor"), mkc("D10", "blocker", { dpi: [1, 2] })] };
  const g0 = evaluateGreen(cl8, [{ id: "D1", status: "blocked" }], "dg05a");
  check("blocked の blocker は緑でなく、結果の無い D2 は notRun", g0.blocked.join() === "D1" && g0.notRun.join() === "D2" && g0.notGreenBlockerMajor.join() === "D1,D2", g0);
  const g1 = evaluateGreen(cl8, [{ id: "D1", status: "pass" }, { id: "D2", status: "pass" }], "dg05a");
  check("gate の対象が pass なら緑（gate 外の D9 / D10 は見ない）", g1.notGreenBlockerMajor.length === 0 && g1.scope.join() === "D1,D2", g1);
  const g2 = evaluateGreen(cl8, [{ id: "D1", status: "pass" }, { id: "D2", status: "skip" }, { id: "D10", dpi: 1, status: "pass" }], undefined);
  check("全体では skip の major、DPI 2 の未実施、minor の未実施が出る", g2.skip.join() === "D2" && g2.notRun.includes("D10@2") && g2.notGreenBlockerMajor.join() === "D2,D10@2" && g2.notGreenMinor.join() === "D9", g2);
  const t8 = fs.mkdtempSync(path.join(os.tmpdir(), "dogfood_g_"));
  try {
    const run = (name, results, gate) => {
      fs.writeFileSync(path.join(t8, `${name}.json`), JSON.stringify({ wave: "w05", runner, results }));
      return collect({ checklistPath: path.join(t8, "cl.json"), resultsPath: path.join(t8, `${name}.json`), evidenceRoot: t8, outDir: path.join(t8, `out_${name}`), gate });
    };
    fs.writeFileSync(path.join(t8, "cl.json"), JSON.stringify(cl8));
    fs.writeFileSync(path.join(t8, "e.json"), "{}");
    const ev = [{ kind: "json", path: "e.json" }];
    const s = run("r1", [{ id: "D1", status: "blocked" }], "dg05a");
    check("blocked だけの結果は pass=0 fail=0 blocked=1 で終了コード 1", s.pass === 0 && s.fail === 0 && s.blocked === 1 && s.skip === 0 && exitCodeOf(s) === 1, s);
    const s2 = run("r2", [{ id: "D1", status: "pass" }, { id: "D2", status: "pass", evidence: ev }], "dg05a");
    check("pass でも必要な証拠（json）が無ければ終了コード 1", s2.missingEvidence.some((m) => m.startsWith("D1")) && exitCodeOf(s2) === 1, s2);
    const s3 = run("r3", [{ id: "D1", status: "fail", observed: "x" }], "dg05a");
    check("fail で証拠が無いと missingEvidence に出る", s3.missingEvidence.some((m) => m.includes("D1: 必要な証拠 json")), s3);
    const s4 = run("r4", [{ id: "D1", status: "pass", evidence: ev }, { id: "D2", status: "pass", evidence: ev }], "dg05a");
    check("gate の全部が pass で証拠もあれば終了コード 0", exitCodeOf(s4) === 0, s4);
  } finally { fs.rmSync(t8, { recursive: true, force: true }); }
}

// [9] 証拠のファイル名が衝突しない
{
  const t9 = fs.mkdtempSync(path.join(os.tmpdir(), "dogfood_e_"));
  try {
    for (const d of ["before", "after"]) { fs.mkdirSync(path.join(t9, "D1", d), { recursive: true }); fs.writeFileSync(path.join(t9, "D1", d, "editor_state.json"), d); }
    const cl9 = { wave: "w05", version: 1, checks: [{ id: "D1", area: "a", steps: [{ kind: "note", text: "x" }], expect: "e", evidence: ["json"], severityIfFail: "major" }] };
    fs.writeFileSync(path.join(t9, "cl.json"), JSON.stringify(cl9));
    fs.writeFileSync(path.join(t9, "res.json"), JSON.stringify({ wave: "w05", runner, results: [{ id: "D1", status: "fail", observed: "x", evidence: [{ kind: "json", path: "D1/before/editor_state.json" }, { kind: "json", path: "D1/after/editor_state.json" }] }] }));
    collect({ checklistPath: path.join(t9, "cl.json"), resultsPath: path.join(t9, "res.json"), evidenceRoot: t9, outDir: path.join(t9, "out"), now: "T" });
    const outJson = fs.readdirSync(path.join(t9, "out", "json"));
    const contents = outJson.map((f) => fs.readFileSync(path.join(t9, "out", "json", f), "utf8")).sort().join();
    check("同じ basename の before / after が両方残る", outJson.length === 2 && contents === "after,before", outJson);
    const bug = JSON.parse(fs.readFileSync(path.join(t9, "out", "bugs.json"), "utf8"))[0];
    check("bugs.json の証拠は収集後のパス", bug.evidence.length === 2 && bug.evidence.every((p) => fs.existsSync(path.join(t9, "out", p))), bug);
  } finally { fs.rmSync(t9, { recursive: true, force: true }); }
}

// [10] 検証器: hash の前提、pattern、engine 引数
{
  const stepsOf = (steps) => validateChecklist(mk({ steps }));
  check("record_hash 無しの hash_unchanged を落とす", stepsOf([{ kind: "file", path: "scenes/main.json", check: "hash_unchanged" }]).length > 0);
  check("別の path の record_hash では足りない", stepsOf([{ kind: "file", path: "a.json", check: "record_hash" }, { kind: "file", path: "b.json", check: "hash_changed" }]).length > 0);
  check("record_hash の後なら hash_changed が通る", stepsOf([{ kind: "file", path: "a.json", check: "record_hash" }, { kind: "file", path: "a.json", check: "hash_changed" }]).length === 0);
  check("contains に pattern が無いと落ちる", stepsOf([{ kind: "file", path: "engine.log", check: "contains", base: "instance" }]).length > 0);
  check("pattern と base 付きの contains は通る", stepsOf([{ kind: "file", path: "engine.log", check: "contains", pattern: "x", base: "instance" }]).length === 0);
  check("不正な base を落とす", stepsOf([{ kind: "file", path: "a", check: "exists", base: "home" }]).length > 0);
  check("engine.args の --env を落とす", stepsOf([{ kind: "engine", action: "start", args: ["--env", "A=1"] }]).length > 0);
  check("engine.env は通り、ANTHROPIC は落ちる", stepsOf([{ kind: "engine", action: "start", env: ["DX12_D3D_DEBUG=1"] }]).length === 0 && stepsOf([{ kind: "engine", action: "start", env: ["ANTHRO" + "PIC_KEY=x"] }]).length > 0);
  check("run は --splash-selftest <dir> が要る", stepsOf([{ kind: "engine", action: "run", args: ["--splash-selftest", "--splash-dump"] }]).length > 0 && stepsOf([{ kind: "engine", action: "run", args: ["--splash-selftest", "{evidence}/s"] }]).length === 0);
  check("idleMin は正の数、freshData は start / restart だけ", stepsOf([{ kind: "engine", action: "restart", idleMin: 0 }]).length > 0 && stepsOf([{ kind: "engine", action: "kill", freshData: true }]).length > 0 && stepsOf([{ kind: "engine", action: "idle_exit", idleMin: 1 }]).length === 0);
  check("DG05-a の blocker に idle_exit を使う D1b と D2 が実在する", ["D1b", "D2"].every((id) => w05.checks.find((c) => c.id === id).steps.some((s) => s.kind === "engine" && s.action === "idle_exit")));
  // D1b は 3 経路のそれぞれの前に新しい人の編集の手順がある
  const d1b = w05.checks.find((c) => c.id === "D1b").steps;
  const humanEditBefore = (i) => i > 0 && d1b.slice(0, i).some((s, j) => s.kind === "note" && /人の編集/.test(s.text) && d1b.slice(j + 1, i).every((x) => !(x.kind === "mcp" && ["open_scene", "new_scene"].includes(x.method) && x.params?.path !== "scenes/main.json") && !(x.kind === "engine" && x.action === "idle_exit")));
  const iOpen = d1b.findIndex((s) => s.kind === "mcp" && s.method === "open_scene" && s.params?.path === "scenes/other.json");
  const iNew = d1b.findIndex((s) => s.kind === "mcp" && s.method === "new_scene");
  const iIdle = d1b.findIndex((s) => s.kind === "engine" && s.action === "idle_exit");
  check("D1b: 3 経路それぞれの直前に、その経路の新しい人の編集の手順がある", [iOpen, iNew, iIdle].every(humanEditBefore), [iOpen, iNew, iIdle]);
}

// [11] bug_schema.json と collect.mjs の定数が食い違わない
{
  const sc = JSON.parse(fs.readFileSync(path.join(here, "bug_schema.json"), "utf8"));
  const sameSet = (a, b) => [...a].sort().join() === [...b].sort().join();
  const D = sc.$defs;
  check("schema: severity", sameSet(D.severity.enum, SEVERITIES));
  check("schema: 結果の status", sameSet(D.results.properties.results.items.properties.status.enum, STATUSES));
  check("schema: 不具合の status", sameSet(D.bug.properties.status.enum, BUG_STATUSES));
  check("schema: 証拠の種類（check / result）", sameSet(D.check.properties.evidence.items.enum, EVIDENCE_KINDS) && sameSet(D.results.properties.results.items.properties.evidence.items.properties.kind.enum, EVIDENCE_KINDS));
  check("schema: 必須キー（check / result / bug）", sameSet(D.check.required, CHECK_REQUIRED) && sameSet(D.results.properties.results.items.required, RESULT_REQUIRED) && sameSet(D.bug.required, BUG_REQUIRED));
  const kinds = D.step.oneOf.map((s) => s.properties.kind.const);
  check("schema: step の種別", sameSet(kinds, Object.keys(STEP_KINDS)), kinds);
  for (const s of D.step.oneOf) {
    const k = s.properties.kind.const, def = STEP_KINDS[k];
    check(`schema: ${k} のキー（必須 / 任意）`, sameSet(s.required, ["kind", ...def.req]) && sameSet(Object.keys(s.properties), ["kind", ...def.req, ...def.opt]), [s.required, Object.keys(s.properties)]);
    if (def.ops) check("schema: input.op", sameSet(s.properties.op.enum, def.ops));
    if (def.checks) check("schema: file.check", sameSet(s.properties.check.enum, def.checks));
    if (def.actions) check("schema: engine.action", sameSet(s.properties.action.enum, def.actions));
    if (k === "file") check("schema: file.base", sameSet(s.properties.base.enum, FILE_BASES));
  }
}

// [12] run.ps1 の終了コード（pwsh がある環境だけ。エンジンは起動しない: 存在しない BuildDir で止まる）
{
  const probe = spawnSync("pwsh", ["-NoProfile", "-Command", "exit 0"], { encoding: "utf8" });
  if (probe.status === 0) {
    const r = spawnSync("pwsh", ["-NoProfile", "-File", path.join(here, "run.ps1"), "-Wave", "w05", "-Action", "start", "-BuildDir", path.join(os.tmpdir(), "dogfood_no_such_build_dir")], { encoding: "utf8" });
    check("run.ps1: ビルド出力が無いと終了コード 4（1 に潰れない）", r.status === 4, [r.status, r.stdout, r.stderr]);
    const dry = spawnSync("pwsh", ["-NoProfile", "-File", path.join(here, "run.ps1"), "-Wave", "w05", "-Action", "restart", "-DryRun", "-IdleExitMin", "0.5"], { encoding: "utf8" });
    check("run.ps1: restart -IdleExitMin が --idle-exit 0.5 になる", dry.status === 0 && /--idle-exit 0\.5/.test(dry.stdout), dry.stdout);
  } else console.log("  --  pwsh が無いので run.ps1 のテストを飛ばす");
}

// [6] 手順に実マウス・フォーカスの要求が無い（実ファイルの全文）
const allText = fs.readFileSync(path.join(here, "checklists", "w05.json"), "utf8") + fs.readFileSync(path.join(here, "checklists", "w1.json"), "utf8");
check("チェックリスト全文に実マウス・フォーカスの語が無い", !/SendInput|mouse_event|SetCursorPos|SetForegroundWindow|computer-use|PrintWindow|実マウス/.test(allText));

console.log(failed ? `\nFAILED: ${failed}` : "\nall OK");
process.exitCode = failed ? 1 : 0;
