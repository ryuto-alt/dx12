// エンジン(C++)と MCP スキーマのドリフト検出テスト。ネット不要・エンジン起動不要。
//
// 落ちたら直し方は 1 つ: エンジンが受け付けるようになったフィールドを index.ts の
// 該当ツールの inputSchema へ足す。足さない限り、その引数は zod に黙って捨てられ、
// set_* は {applied:true} を返し、AI は「効かない操作」を延々と繰り返す。
//
// このテストが実際に止めた事故:
//   tonemapper / godraysOn / lensflareOn / dofOn / motionBlurOn / autoExposureOn /
//   bloomKnee / bloomRadius / lut* / deband / snap_to_ground の precise。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseEngineMethods, parseFieldMacro, parseStringVector, parseTsTools } from "./schemaDrift.ts";
import { COMPOSITE_TOOLS, GLOBAL_PARAM_KEYS, METHOD_KEY_ALIASES } from "./paramGuard.ts";
import {
  SCULPT_BRUSHES, SCULPT_PRIMITIVES, TERRAIN_BRUSHES, TERRAIN_PRESETS,
} from "./sceneTools.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..");
const CPP = path.join(repoRoot, "src", "core", "Application.cpp");
const POST_H = path.join(repoRoot, "src", "renderer", "PostProcessSettings.h");
const INDEX_TS = path.join(here, "index.ts");

let failed = 0;
const ok = (label: string) => console.log(`  OK  ${label}`);
function check(label: string, cond: boolean, detail?: string) {
  if (cond) ok(label);
  else { failed++; console.log(`  NG  ${label}${detail ? `\n      ${detail}` : ""}`); }
}

if (!fs.existsSync(CPP) || !fs.existsSync(POST_H)) {
  // mcp-server だけを配布したケース。突き合わせる相手が無いので検査しない(落とさない)。
  console.log("SKIP: エンジンのソース(src/core/Application.cpp)が無いのでドリフト検査は省略");
  process.exit(0);
}

const cpp = fs.readFileSync(CPP, "utf8");
const header = fs.readFileSync(POST_H, "utf8");
const indexSrc = fs.readFileSync(INDEX_TS, "utf8");

const engine = parseEngineMethods(cpp);
const tools = parseTsTools(indexSrc);

// index.ts の共通スプレッド部品 → 実キー。増えたらここに足す(足し忘れは下の [0] が気づく)。
const SPREADS: Record<string, string[]> = { "...entityRef": ["entity", "name"] };

/**
 * 同じ if 分岐が 2 つの method を受けているせいで、相手側のキーまで拾ってしまう分。
 * (`method == "a" || method == "b"` の形。パーサはブロックを分けられないので両方に同じ集合が付く)
 * ★ここに書くのは「相手 method のキー」だけ。自分のキーを書くと本物のドリフトを見逃す。
 */
const SHARED_BLOCK_KEYS: Record<string, string[]> = {
  overlap_box: ["radius"],
  overlap_sphere: ["halfExtents"],
  // Application.cpp:6358 `terrain_paint || terrain_autopaint`
  terrain_paint: [
    "rockSlopeStart", "rockSlopeEnd", "dirtSlopeStart", "dirtSlopeEnd",
    "snowHeightStart", "snowHeightEnd", "noiseStrength",
  ],
  terrain_autopaint: ["layer", "radius", "strength", "falloff", "point", "points", "worldPos"],
};

console.log("[0] パーサ自体が壊れていないこと");
check("Application.cpp から MCP method を抜ける", engine.size >= 90, `size=${engine.size}`);
check("index.ts からツール定義を抜ける", tools.length >= 100, `tools=${tools.length}`);
check("代表的な method / ツールが取れている",
  engine.has("set_post_process") && engine.has("set_ssao") && engine.has("set_volumetric_fog")
  && tools.some((t) => t.tool === "dx12_set_post_process")
  && tools.some((t) => t.tool === "dx12_screenshot"));
check("スプレッドが全部 SPREADS に載っている",
  tools.every((t) => t.schemaKeys.every((k) => !k.startsWith("...") || k in SPREADS)),
  tools.flatMap((t) => t.schemaKeys.filter((k) => k.startsWith("...") && !(k in SPREADS))).join(","));
{
  // 入れ子の `if (method == "...")` を分岐の頭と誤認しないこと。誤認するとブロックが途中で切れ、
  // 前半で読んでいるキーが丸ごと落ちて【ドリフトを検出できなくなる】(terrain_paint が
  // entity/name しか持たない状態になっていた)。両 method が同じ行・同じ全部入りの集合になるのが正。
  const paint = engine.get("terrain_paint");
  const auto = engine.get("terrain_autopaint");
  check("`a || b` の分岐は 1 ブロックとして両 method に同じ集合が付く",
    paint != null && auto != null && paint.line === auto.line
    && paint.keys.includes("layer") && paint.keys.includes("rockSlopeStart"),
    `paint=${JSON.stringify(paint)} / auto=${JSON.stringify(auto)}`);
}

const keysOf = (t: { schemaKeys: string[] }) =>
  new Set(t.schemaKeys.flatMap((k) => (k.startsWith("...") ? (SPREADS[k] ?? []) : [k])));

console.log("\n[1] dx12_<X> ツールは engine の method X と対応している");
{
  const orphans = tools
    .filter((t) => !COMPOSITE_TOOLS.has(t.tool))
    .filter((t) => !engine.has(t.tool.replace(/^dx12_/, "")))
    .map((t) => `${t.tool} (index.ts:${t.line})`);
  check("1:1 のはずのツールに対応する engine method がある", orphans.length === 0,
    `対応先が無い: ${orphans.join(", ")}\n      → 合成ツールなら paramGuard.ts の COMPOSITE_TOOLS に足す`);
}

console.log("\n[2] エンジンが受け付けるフィールド ⊇ TS スキーマ(渡せないフィールドが無い)");
{
  const drifted: string[] = [];
  for (const t of tools) {
    if (COMPOSITE_TOOLS.has(t.tool)) continue;
    const method = t.tool.replace(/^dx12_/, "");
    const em = engine.get(method);
    if (!em) continue;   // [1] が報告済み
    const declared = keysOf(t);
    const allowed = new Set<string>([
      ...GLOBAL_PARAM_KEYS,
      ...(METHOD_KEY_ALIASES[method] ?? []),
      ...(SHARED_BLOCK_KEYS[method] ?? []),
    ]);
    const missing = em.keys.filter((k) => !declared.has(k) && !allowed.has(k));
    if (missing.length > 0) {
      drifted.push(`${t.tool}: エンジンにあるのに TS スキーマに無い → ${missing.join(", ")}`
        + `  (Application.cpp:${em.line} / index.ts:${t.line})`);
    }
  }
  check("全ツールで取りこぼしゼロ", drifted.length === 0, drifted.join("\n      "));
}

console.log("\n[3] PostProcessSettings.h の名前表(X マクロ)と突き合わせ");
{
  const postFields = parseFieldMacro(header, "DX12E_POST_FIELDS");
  const ssaoFields = parseFieldMacro(header, "DX12E_SSAO_FIELDS");
  check("DX12E_POST_FIELDS を読めている", postFields.length >= 80, `${postFields.length} fields`);
  check("DX12E_SSAO_FIELDS を読めている", ssaoFields.length === 7, ssaoFields.join(","));

  const pp = tools.find((t) => t.tool === "dx12_set_post_process")!;
  const ppKeys = keysOf(pp);
  const missPp = postFields.filter((f) => !ppKeys.has(f));
  check("dx12_set_post_process が名前表の全フィールドを宣言している", missPp.length === 0,
    `足りない: ${missPp.join(", ")}`);

  const ssao = tools.find((t) => t.tool === "dx12_set_ssao")!;
  const ssaoKeys = keysOf(ssao);
  const missSsao = ssaoFields.filter((f) => !ssaoKeys.has(f));
  check("dx12_set_ssao が名前表の全フィールドを宣言している", missSsao.length === 0,
    `足りない: ${missSsao.join(", ")}`);

  // 名前表 ⊆ engine ハンドラ (C++ 側の取りこぼし検出。ここが落ちたらエンジンの set_post_process が
  // フィールドを 1 つ読み忘れている＝MCP からは永遠に触れない)
  const em = engine.get("set_post_process")!;
  const notHandled = postFields.filter((f) => !em.keys.includes(f));
  check("engine の set_post_process が名前表の全フィールドを読んでいる", notHandled.length === 0,
    `Application.cpp が読んでいない: ${notHandled.join(", ")}`);
}

console.log("\n[4] {applied:true} を鵜呑みにせず get_* で読み返している");
{
  // エンジンが {"applied", true} しか返さない method は、それだけでは「本当に入ったか」が
  // 分からない(未知フィールドを無視しても applied:true が返る)。get_* の対がある場合は
  // 必ず読み返してから返すこと。
  const notVerified: string[] = [];
  for (const [method, em] of engine) {
    if (!em.returnsAppliedTrue) continue;
    const pair = method.startsWith("set_") ? `get_${method.slice(4)}` : null;
    if (!pair || !engine.has(pair)) continue;
    const t = tools.find((x) => x.tool === `dx12_${method}`);
    if (!t) continue;
    if (!t.methods.includes(pair)) notVerified.push(`${t.tool}(index.ts:${t.line})`);
  }
  check("applied:true を返す設定系は適用後に get_* で読み返す", notVerified.length === 0,
    `鵜呑みにしている: ${notVerified.join(", ")}`);
}

console.log("\n[5] 列挙値(sceneTools.ts の定数 ⇔ engine の表)");
{
  // 列挙は足し忘れても「黙って捨てられる」わけではない(zod が弾く / engine が有効値を返す)が、
  // エンジンに増えたブラシ等が MCP から永遠に選べない状態になるのは同じなので一緒に見る。
  const cases: [string, string, readonly string[]][] = [
    ["kTerrainPresets", "TERRAIN_PRESETS", TERRAIN_PRESETS],
    ["kTerrainBrushes", "TERRAIN_BRUSHES", TERRAIN_BRUSHES],
    ["kSculptBrushes", "SCULPT_BRUSHES", SCULPT_BRUSHES],
    ["kSculptPrims", "SCULPT_PRIMITIVES", SCULPT_PRIMITIVES],
  ];
  for (const [cppVar, tsName, tsValues] of cases) {
    const engineValues = parseStringVector(cpp, cppVar);
    if (engineValues == null) {
      check(`${cppVar} を Application.cpp から読める`, false, "変数名が変わった? schemaDrift.test.ts を更新すること");
      continue;
    }
    check(`${tsName} が ${cppVar} と一致(順序込み)`,
      JSON.stringify(engineValues) === JSON.stringify([...tsValues]),
      `engine=[${engineValues.join(",")}] / ts=[${tsValues.join(",")}]`);
  }
}

console.log("\n[6] SHARED_BLOCK_KEYS が本物のドリフトを隠していないこと");
{
  // ここに自分のキーを書くと [2] が素通りしてしまう。載せていいのは「相方 method のキー」だけ。
  const leaks: string[] = [];
  for (const [method, keys] of Object.entries(SHARED_BLOCK_KEYS)) {
    const t = tools.find((x) => x.tool === `dx12_${method}`);
    if (!t) { leaks.push(`dx12_${method} が index.ts に無い`); continue; }
    const declared = keysOf(t);
    const own = keys.filter((k) => declared.has(k));
    if (own.length > 0) leaks.push(`${method}: 自分で宣言しているキーを除外している → ${own.join(", ")}`);
  }
  check("除外リストは相方 method のキーだけ", leaks.length === 0, leaks.join("\n      "));
}

console.log("\n[7] 今回追加したツールが宣言を取りこぼしていないこと");
{
  // [2] は「エンジン ⊇ TS」しか見ない(共有ブロックのぶんは除外される)ので、
  // 地形ペイント 2 本については「TS が実際に何を宣言しているか」も名指しで固定する。
  const want: Record<string, string[]> = {
    dx12_terrain_paint: ["entity", "name", "layer", "point", "points", "worldPos", "radius", "strength", "falloff"],
    dx12_terrain_autopaint: ["entity", "name", "rockSlopeStart", "rockSlopeEnd", "dirtSlopeStart",
      "dirtSlopeEnd", "snowHeightStart", "snowHeightEnd", "noiseStrength"],
  };
  for (const [tool, keys] of Object.entries(want)) {
    const t = tools.find((x) => x.tool === tool);
    if (!t) { check(`${tool} が登録されている`, false); continue; }
    const declared = keysOf(t);
    const missing = keys.filter((k) => !declared.has(k));
    check(`${tool} が engine の引数を全部宣言している`, missing.length === 0, `足りない: ${missing.join(", ")}`);
  }
  const mat = tools.find((t) => t.tool === "dx12_material_apply");
  check("dx12_material_apply が登録されている & 合成ツール扱い",
    mat != null && COMPOSITE_TOOLS.has("dx12_material_apply"));
  check("dx12_material_apply は既存 method だけで組んである(新しい C++ method を要求しない)",
    mat != null && mat.methods.every((m) => engine.has(m)) && mat.methods.length > 0,
    `methods=${mat?.methods.join(",")}`);
  check("dx12_material_apply が set_texture / set_pbr / get_entity を使う",
    mat != null && ["set_texture", "set_pbr", "get_entity"].every((m) => mat.methods.includes(m)),
    `methods=${mat?.methods.join(",")}`);
}

console.log(failed === 0
  ? "\nOK: schemaDrift テストすべて通過"
  : `\nNG: ${failed} 件失敗`);
process.exit(failed === 0 ? 0 : 1);
