// W1-NB7: mcp_feature_coverage の雛形と、W1 で足した MCP method の TS 側(例・searchHints・E_OUT_OF_RANGE の形)の検査。エンジン不要・決定論。
// 実行: node mcpCoverage.test.ts
//  - 新しい method(manifest.snapshot.json にあって mcpCoverage.baseline.json に無いもの)に例か searchHints(日本語・英語)が無ければ赤。
//  - ベースラインは減らす方向にだけ動く(解消済みの項目が残っていても赤)。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
process.env.DX12_MCP_PORT = "1";
process.env.DX12_MCP_CONNECT_BACKOFF_MS = "0";
await import("./toolset/all.ts");
const { buildCatalog } = await import("./catalog.ts");
const { TOOL_REGISTRY } = await import("./toolRuntime.ts");
const { loadSnapshot } = await import("./manifest.ts");
const { SEARCH_HINTS, SEARCH_HINTS_EN } = await import("./searchHints.ts");
const { computeCoverageGaps, judgeAgainstBaseline } = await import("./mcpCoverage.ts");
const { outOfRangeDetailsKind, ERROR_CODES } = await import("./errors.ts");
const { SearchIndex } = await import("./search.ts");

let failed = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${JSON.stringify(detail).slice(0, 600)}` : ""}`); }
}

const manifest = loadSnapshot()!;
check("manifest.snapshot.json を読めた", !!manifest && manifest.methods.size > 200, manifest?.methods.size);
const catalog = buildCatalog(TOOL_REGISTRY, manifest);
const gaps = computeCoverageGaps(manifest, catalog.docs as any, SEARCH_HINTS, SEARCH_HINTS_EN);
const baseline = JSON.parse(fs.readFileSync(path.join(here, "mcpCoverage.baseline.json"), "utf8")) as { noExample: string[]; noHints: string[] };
const existing = new Set(manifest.methods.keys());

console.log("[1] 全 method にカタログの項目がある");
check("カタログに出てこない method が無い", gaps.noDoc.length === 0, gaps.noDoc);

console.log("[2] 新しい method は例と searchHints(日本語・英語)が要る(ベースライン方式)");
for (const [kind, list, base] of [["例が無い", gaps.noExample, baseline.noExample], ["searchHints(日英)が無い", gaps.noHints, baseline.noHints]] as const) {
  const v = judgeAgainstBaseline(kind, list, base, existing);
  check(`${kind}: ベースライン外の method が無い(足したなら catalog.ts の EXTRA_EXAMPLES / searchHints.ts に足す)`, v.newGaps.length === 0, v.newGaps);
  check(`${kind}: ベースラインに解消済み・消えた method が残っていない(mcpCoverage.baseline.json から消す)`, v.staleBaseline.length === 0, v.staleBaseline);
}

console.log("[3] 雛形が本当に赤にする(合成: 例も searchHints も無い新 method)");
{
  const fake = { name: "zz_new_method", category: "render", summary: "合成", effect: "read" } as any;
  const m2: any = { ...manifest, methods: new Map([...manifest.methods, ["zz_new_method", fake]]) };
  const cat2 = buildCatalog(TOOL_REGISTRY, m2);
  const g2 = computeCoverageGaps(m2, cat2.docs as any, SEARCH_HINTS, SEARCH_HINTS_EN);
  const ex = new Set(existing).add("zz_new_method");
  check("例が無い新 method は赤になる", judgeAgainstBaseline("x", g2.noExample, baseline.noExample, ex).newGaps.join() === "x: zz_new_method", g2.noExample.slice(-3));
  check("searchHints が無い新 method は赤になる", judgeAgainstBaseline("x", g2.noHints, baseline.noHints, ex).newGaps.join() === "x: zz_new_method", g2.noHints.slice(-3));
  const g3 = computeCoverageGaps(m2, [...(cat2.docs as any[]).map((d) => d.method === "zz_new_method" ? { ...d, examples: [{ args: {} }] } : d)], { ...SEARCH_HINTS, zz_new_method: "新しい" }, { ...SEARCH_HINTS_EN, zz_new_method: "new" });
  check("例と日英の searchHints を足せば緑に戻る", !g3.noExample.includes("zz_new_method") && !g3.noHints.includes("zz_new_method"));
  const g4 = computeCoverageGaps(m2, cat2.docs as any, { ...SEARCH_HINTS, zz_new_method: "新しい" }, SEARCH_HINTS_EN);
  check("日本語だけでは足りない(英語の searchHints も要る)", g4.noHints.includes("zz_new_method"));
}

console.log("[4] W1 の method に例・searchHints があり、例の引数がマニフェストの引数名に収まる");
const W1_METHODS = ["vfx_place", "vfx_seek", "vfx_stats", "freeze_time", "frame_step", "get_quality_preset", "set_quality_preset", "new_scene", "screenshot", "screenshot_final"];
const docOf = (n: string) => (catalog.docs as any[]).find((d) => d.method === n);
for (const n of W1_METHODS) {
  const d = docOf(n);
  const names = new Set<string>((manifest.methods.get(n)?.params ?? []).map((p: any) => p.name));
  const badKeys = (d?.examples ?? []).flatMap((e: any) => Object.keys(e.args ?? {}).filter((k) => !names.has(k)));
  check(`${n}: 例が 1 件以上・引数名がマニフェストに実在・searchHints(日英)あり`,
    (d?.examples?.length ?? 0) >= 1 && badKeys.length === 0 && !!SEARCH_HINTS[n] && !!SEARCH_HINTS_EN[n], { n, badKeys, ex: d?.examples?.length });
}
check("new_scene の例が profile:classic を示す", docOf("new_scene")?.examples?.some((e: any) => e.args?.profile === "classic"));
check("set_quality_preset の例の注記が『C1 時点』のまま古くなっていない", !JSON.stringify(docOf("set_quality_preset")?.examples).includes("C1 時点"));
{
  const ex = docOf("vfx_place")?.examples ?? [];
  check("vfx_place は preset と asset を同時に渡す例を持たない(排他)", ex.every((e: any) => !(e.args?.preset && e.args?.asset)));
  const seek = docOf("vfx_seek")?.examples?.[0]?.args;
  const tp = manifest.methods.get("vfx_seek")?.params.find((p: any) => p.name === "t");
  check("vfx_seek の例の t がマニフェストの範囲内", typeof seek?.t === "number" && seek.t >= Number(tp?.min) && seek.t <= Number(tp?.max), { seek, tp });
}
const idx = new SearchIndex(catalog);
for (const [q, want] of [["焚き火 エフェクト 置く", "vfx_place"], ["place vfx effect campfire", "vfx_place"], ["VFX 時刻 固定 決定論", "vfx_seek"], ["freeze time deterministic", "freeze_time"], ["コマ送り", "frame_step"], ["unofx stats live particles vram", "vfx_stats"]] as const) {
  const hits = idx.search(q, { limit: 8 }).hits.map((h: any) => h.name);
  check(`tool_search「${q}」が ${want} を上位 8 件に出す`, hits.some((n: string) => n === want || n === "dx12_" + want), hits);
}
check("dx12_vfx_apply(旧レシピ)から vfx_place へ誘導する(next)", (docOf("vfx_apply") ?? (catalog.docs as any[]).find((d) => d.id === "dx12_vfx_apply"))?.next?.some((x: any) => x.tool === "dx12_vfx_place"));

console.log("[5] E_OUT_OF_RANGE の details の形(エンジンの 3 つの出どころ)");
check("範囲外(数値): {param,value,range:{min,max}}", outOfRangeDetailsKind({ param: "microShadowK", value: 5, range: { min: 0, max: 2 } }) === "range");
check("範囲外(片側だけの range)も range", outOfRangeDetailsKind({ param: "x", value: -1, range: { min: 0 } }) === "range");
check("添字: {field,value,max}(layer:7 / max:2)", outOfRangeDetailsKind({ field: "layer", value: 7, max: 2 }) === "index");
check("添字: 余計なキーがあれば不正", outOfRangeDetailsKind({ field: "layer", value: 7, max: 2, extra: 1 }) === null);
check("長さ: {param,length,range:{min,max}}(editor_notify)", outOfRangeDetailsKind({ param: "message", length: 401, range: { min: 1, max: 400 } }) === "length");
check("知らない形は null", outOfRangeDetailsKind({ foo: 1 }) === null && outOfRangeDetailsKind(null) === null && outOfRangeDetailsKind([1]) === null);
check("コード表の説明が details の形へ案内する", ERROR_CODES.E_OUT_OF_RANGE.meaning.includes("OUT_OF_RANGE_DETAILS"));

console.log(failed ? `\n${failed} 件失敗` : "\nすべて成功");
process.exit(failed ? 1 : 0);
