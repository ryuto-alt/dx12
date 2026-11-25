// ゴールデンの引数解析とシーンの絞り込み（純関数。GPU もエンジンも要らない・単体テストあり）。
import path from "node:path";
import { findDefaultExe } from "./engine.mjs";

export function parseArgs(argv) {
  const a = { update: false, repeat: 1, only: null, exe: findDefaultExe(), strict: true, group: null, uiOnly: false, noUi: false, forceTheme: null };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === "--update") a.update = true;
    else if (k === "--repeat") a.repeat = Math.max(1, Number(argv[++i]) || 1);
    else if (k === "--only") a.only = new Set(String(argv[++i]).split(",").map((s) => s.trim()).filter(Boolean));
    else if (k === "--exe") a.exe = path.resolve(argv[++i]);
    else if (k === "--strict") a.strict = true;
    else if (k === "--no-strict") a.strict = false;
    else if (k === "--group") a.group = String(argv[++i]);
    else if (k === "--ui-only") a.uiOnly = true;
    else if (k === "--no-ui") a.noUi = true;
    else if (k === "--force-theme") a.forceTheme = String(argv[++i]);
    else if (k === "--help" || k === "-h") { a.help = true; }
    else throw new Error(`不明な引数: ${k}`);
  }
  if (a.update && a.forceTheme) throw new Error("--update と --force-theme は同時に使えない（基準をテーマ違いで上書きしてしまう）");
  return a;
}

/**
 * 回すシーンを決める。
 *  - requiresProjectsRoot:true は hasRoot（DX12_GOLDEN_PROJECTS_ROOT）があるときだけ。無ければ対象から外す（SKIP の見かけ合格にしない）。
 *  - group 指定なし → group の無いシーンだけ。group 指定あり → その group のシーンだけ。
 *  - only があれば名前で絞る（上の絞り込みの後なので、対象外のシーンを --only で名指ししても回らない）。
 * 戻り値: { scenes, gated: 対象外にした名前の配列 }
 */
export function selectScenes(all, { hasRoot = false, group = null, only = null } = {}) {
  const gated = all.filter((s) => s.requiresProjectsRoot && !hasRoot).map((s) => s.name);
  let scenes = all.filter((s) => !gated.includes(s.name));
  scenes = scenes.filter((s) => (group ? s.group === group : !s.group));
  if (only) scenes = scenes.filter((s) => only.has(s.name));
  return { scenes, gated };
}
