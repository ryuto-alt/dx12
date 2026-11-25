// W05-38: ドッグフードの道具の単体テスト（エンジン不要・LLM 不要）。実行: node tools/dogfood/collect.test.mjs
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { validateChecklist, validateResults, deriveBugs, nextBugNumber, makeBugId, collect } from "./collect.mjs";

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

// [6] 手順に実マウス・フォーカスの要求が無い（実ファイルの全文）
const allText = fs.readFileSync(path.join(here, "checklists", "w05.json"), "utf8") + fs.readFileSync(path.join(here, "checklists", "w1.json"), "utf8");
check("チェックリスト全文に実マウス・フォーカスの語が無い", !/SendInput|mouse_event|SetCursorPos|SetForegroundWindow|computer-use|PrintWindow|実マウス/.test(allText));

console.log(failed ? `\nFAILED: ${failed}` : "\nall OK");
process.exitCode = failed ? 1 : 0;
