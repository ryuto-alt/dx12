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
check("tool_search で『自動保存』『caps』が上位に出る",
  shell.index.search("人の未保存の変更 自動保存", { limit: 5 }).hits.some((h) => /save_scene|editor_state/.test(h.name)));

console.log(failed ? `\n${failed} 件失敗` : "\nすべて成功");
process.exit(failed ? 1 : 0);
