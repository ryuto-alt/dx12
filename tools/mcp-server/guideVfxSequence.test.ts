// W1-NB15: guide の vfx / sequence トピックの検査。エンジン不要・決定論。
// 実行: node guideVfxSequence.test.ts
// 守ること: ① 2 トピックが一覧にある ② 本文が 6 KB 以内 ③ 案内する dx12_* のツール名が全部実在する
//          ④ 例の dx12_sequence の呼び出しが planSequenceCall を通る ⑤ プリセット名が assets に実在する ⑥ vfx_place / vfx_seek が searchHints に居る。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
process.env.DX12_MCP_PORT = "1";
process.env.DX12_MCP_CONNECT_BACKOFF_MS = "0";
await import("./toolset/all.ts");
const { shell } = await import("./toolset/shell.ts");
const { planSequenceCall } = await import("./sequenceOps.ts");

let failed = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${JSON.stringify(detail).slice(0, 600)}` : ""}`); }
}

const topics = shell.guideTopics();
console.log("[1] 一覧と大きさ");
for (const id of ["vfx", "sequence"]) {
  const t = topics.find((x) => x.id === id);
  check(`${id}: トピック一覧にある(title / summary つき)`, !!t && t.title.length > 0 && t.summary.length > 0, t);
  const text = (shell.guide({ topic: id }).content[0] as any).text as string;
  check(`${id}: 本文が Markdown で 6 KB 以内`, text.startsWith("#") && Buffer.byteLength(text) <= 6144, Buffer.byteLength(text));
}

const bodies: Record<string, string> = {
  vfx: fs.readFileSync(path.join(here, "guides", "vfx.md"), "utf8"),
  sequence: fs.readFileSync(path.join(here, "guides", "sequence.md"), "utf8"),
};

console.log("[2] 案内するツール名が実在する");
const doc = (n: string) => shell.catalog.docs.find((d) => d.id === n || d.aliases?.includes(n));
for (const [id, body] of Object.entries(bodies)) {
  const names = [...new Set([...body.matchAll(/\bdx12_[a-z0-9_]+/g)].map((m) => m[0]))];
  const missing = names.filter((n) => !doc(n));
  check(`${id}: dx12_* ${names.length} 個が全部カタログにある`, missing.length === 0, missing);
}
for (const t of ["vfx_place", "vfx_seek", "vfx_stats", "sequence"]) check(`${t} がカタログにある`, !!doc("dx12_" + t) || !!doc(t));

console.log("[3] 内容");
check("vfx: place / seek / stats / プリセット / Play 中は断られる / resume を説明している",
  ["dx12_vfx_place", "dx12_vfx_seek", "dx12_vfx_stats", "p_campfire", "p_torch", "p_embers_drift", "p_sparks", "dx12_stop", "resume"].every((w) => bodies.vfx.includes(w)));
const presetDir = path.join(here, "..", "..", "assets", "engine", "vfx2", "presets");
for (const p of ["p_campfire", "p_torch", "p_embers_drift", "p_sparks"]) check(`プリセット ${p}.uvfx が assets にある`, fs.existsSync(path.join(presetDir, p + ".uvfx")));
check("sequence: 全 op・scrub は Editor 限定・.dxseq・autoplay を説明している",
  ["list", "load", "get", "edit", "eval", "scrub", "save", "play", "stop", "autoplay", ".dxseq", "Editor 限定", "docs/DXSEQ_FORMAT.md"].every((w) => bodies.sequence.includes(w)));
check("sequence: DXSEQ_FORMAT.md が実在する", fs.existsSync(path.join(here, "..", "..", "docs", "DXSEQ_FORMAT.md")));

console.log("[4] sequence の例が実際に通る");
const calls = [...bodies.sequence.matchAll(/^dx12_sequence (\{.*\})/gm)].map((m) => m[1].replace(/\s+#.*$/, ""));
check("sequence: 例の呼び出しが 8 件以上ある", calls.length >= 8, calls.length);
for (const c of calls) {
  let args: any;
  try { args = JSON.parse(c.replace(/([{,]\s*)([A-Za-z_]+):/g, '$1"$2":')); } catch (e) { check(`JSON として読める: ${c.slice(0, 60)}`, false, String(e)); continue; }
  const plan = planSequenceCall(args);
  check(`planSequenceCall を通る: ${c.slice(0, 70)}`, plan.ok === true, (plan as any).body);
}

console.log("[5] 検索");
const { SEARCH_HINTS } = await import("./searchHints.ts");
check("vfx_place / vfx_seek に searchHints がある", !!SEARCH_HINTS.vfx_place && !!SEARCH_HINTS.vfx_seek);
check("dx12_guide の一覧が id 昇順で vfx / sequence を含む", JSON.stringify(topics.map((t) => t.id)) === JSON.stringify([...topics.map((t) => t.id)].sort()));

console.log(failed ? `\n${failed} 件失敗` : "\nすべて成功");
process.exit(failed ? 1 : 0);
