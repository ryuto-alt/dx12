// W05-40: W0.5 の MCP 追従(autosave・device_caps のガイド、searchHints、例)の検査。エンジン不要・決定論。
// 実行: node autosaveGuide.test.ts
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
process.env.DX12_MCP_PORT = "1";
process.env.DX12_MCP_CONNECT_BACKOFF_MS = "0";
await import("./toolset/all.ts");
const { shell } = await import("./toolset/shell.ts");
const { SEARCH_HINTS, SEARCH_HINTS_EN } = await import("./searchHints.ts");

let failed = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${JSON.stringify(detail).slice(0, 600)}` : ""}`); }
}

console.log("[1] guide トピック");
const topics = shell.guideTopics().map((t) => t.id);
check("autosave と device_caps のトピックがある", topics.includes("autosave") && topics.includes("device_caps"), topics);
const auto = fs.readFileSync(path.join(here, "guides", "autosave.md"), "utf8");
check("autosave: blockedByHumanEdits・人に保存を依頼・アイドル終了・taskkill を説明している",
  ["blockedByHumanEdits", "人に保存を依頼", "アイドル終了", "taskkill", "lastBackupId"].every((w) => auto.includes(w)));
const caps = fs.readFileSync(path.join(here, "guides", "device_caps.md"), "utf8");
check("device_caps: fallbackReason を説明している", caps.includes("dlss.fallbackReason") && caps.includes("get_device_caps"));

console.log("[2] searchHints");
const jp = Object.values(SEARCH_HINTS).join(" ").toLowerCase();
const en = Object.values(SEARCH_HINTS_EN).join(" ").toLowerCase();
for (const w of ["caps", "agility", "sm6.9", "autosave"]) check(`「${w}」が searchHints にある`, jp.includes(w) || en.includes(w));
check("get_device_caps に hint がある", !!SEARCH_HINTS.get_device_caps && !!SEARCH_HINTS_EN.get_device_caps);

console.log("[3] 例とツール説明");
const doc = (n: string) => shell.catalog.docs.find((d) => d.id === n || d.aliases?.includes(n));
const es = doc("dx12_editor_state");
check("editor_state に例が 1 件以上ある", (es?.examples?.length ?? 0) >= 1);
check("editor_state / save_scene の説明に autosave.blockedByHumanEdits がある",
  (es?.coreDescription ?? "").includes("blockedByHumanEdits") && (doc("dx12_save_scene")?.coreDescription ?? "").includes("blockedByHumanEdits"));
const up = doc("dx12_get_upscaler");
check("get_upscaler に例が 1 件以上ある", (up?.examples?.length ?? 0) >= 1);
check("tool_search で『人の未保存の変更 自動保存』が save_scene / editor_state を上位に出す",
  shell.index.search("人の未保存の変更 自動保存", { limit: 5 }).hits.some((h) => /save_scene|editor_state/.test(h.name)));

console.log("[4] autosave ガイドの記述(終了経路・save_scene)");
check("autosave: engine_stop / engine_refresh / アイドル停止が taskkill と書いてある",
  ["engine_stop", "engine_refresh", "アイドル停止", "taskkill"].every((w) => auto.includes(w)));
check("autosave: wroteHumanEdits と復旧用の自動保存を説明している", auto.includes("wroteHumanEdits") && auto.includes("復旧用の自動保存"));
const cs = (n: string) => doc(n)?.coreDescription ?? "";
check("engine_stop / engine_refresh の説明に autosave.blockedByHumanEdits がある",
  cs("dx12_engine_stop").includes("blockedByHumanEdits") && cs("dx12_engine_refresh").includes("blockedByHumanEdits"));
check("save_scene の説明に wroteHumanEdits がある", cs("dx12_save_scene").includes("wroteHumanEdits"));
check("device_caps ガイドは未確定のキー名を断定しない(暫定の注記がある)", caps.includes("暫定") && !caps.includes("serSupported"));

console.log("[5] manifest だけにある get_device_caps(合成マニフェスト)");
const { buildCatalog } = await import("./catalog.ts");
const { SearchIndex } = await import("./search.ts");
const { TOOL_REGISTRY } = await import("./toolRuntime.ts");
const synth: any = {
  protocol: 1, manifestHash: "synthetic", count: 1, categories: [], fetchedAt: 0, source: "snapshot",
  methods: new Map([["get_device_caps", { name: "get_device_caps", category: "render", summary: "GPU の機能を読む", effect: "read" }]]),
};
const cat2 = buildCatalog(TOOL_REGISTRY, synth);
const dc = cat2.docs.find((d: any) => d.id === "get_device_caps");
check("manifest のみの get_device_caps に EXTRA_EXAMPLES の例が付く", (dc?.examples?.length ?? 0) >= 1, dc?.examples);
const idx2 = new SearchIndex(cat2);
for (const q of ["caps", "agility", "SM6.9"]) {
  const hits = idx2.search(q, { limit: 5 }).hits;
  check(`tool_search「${q}」で get_device_caps が上位 5 件に出る`, hits.some((h: any) => /get_device_caps/.test(h.name)), hits.map((h: any) => h.name));
}

console.log(failed ? `\n${failed} 件失敗` : "\nすべて成功");
process.exit(failed ? 1 : 0);
