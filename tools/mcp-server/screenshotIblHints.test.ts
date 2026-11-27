// FU-E-36: dx12_screenshot / dx12_screenshot_final の waitIblRebake に例と検索語があることの検査。エンジン不要・決定論。
// 実行: node screenshotIblHints.test.ts
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

const doc = (n: string) => shell.catalog.docs.find((d) => d.id === n || d.aliases?.includes(n));
for (const n of ["dx12_screenshot", "dx12_screenshot_final"]) {
  const ex = doc(n)?.examples ?? [];
  check(`${n} に waitIblRebake:true の例がある`, ex.some((e: any) => e.args?.waitIblRebake === true), ex);
}
for (const k of ["screenshot", "screenshot_final"]) {
  check(`searchHints(日本語) ${k} に IBL 焼き直しの語がある`, /IBL/.test(SEARCH_HINTS[k] ?? "") && /焼き直し/.test(SEARCH_HINTS[k] ?? ""));
}
check("searchHints(英語) screenshot_final に IBL rebake の語がある", /IBL/i.test(SEARCH_HINTS_EN.screenshot_final ?? "") && /rebake/i.test(SEARCH_HINTS_EN.screenshot_final ?? ""));
const hits = shell.index.search("IBL 焼き直し 待つ 撮る", { limit: 8 }).hits.map((h: any) => h.name);
check("tool_search『IBL 焼き直し 待つ 撮る』が screenshot 系を上位に出す", hits.some((n: string) => /screenshot/.test(n)), hits);

console.log(failed ? `\n${failed} 件失敗` : "\nすべて成功");
process.exit(failed ? 1 : 0);
