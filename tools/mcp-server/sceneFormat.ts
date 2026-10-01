// シーンファイル形式 v2 の補完・省略・整形（純関数。エンジンは触らない）。
// 仕様の正本は docs/SCENE_FORMAT_DESIGN.md §3。C++ 側の実装は src/scene/SceneFormatV2.{h,cpp}。
//
//   inflateScene … version >= 2 のシーンを読むときに、既定値表で省略されたフィールドを足す（補完後は v1 保存と同じ形）
//   convertToV2  … 完全形（または補完済み）のシーンを v2 へ（最短 float・既定値の省略・parent index 廃止）
//   dumpSceneV2  … ルート設定は整形・entities は 1 行 1 体
//
// ★既定値表は src/scene/scene_defaults_v2.json が唯一の正本。配布（publish.ps1）はこのフォルダだけを写すので、
//   同じ内容のコピーを ./scene_defaults_v2.json に置く。sceneFormat.test.ts がバイト一致を見張る（食い違えば赤）。
//   コピーの更新: node scripts/sync_scene_defaults.mjs
//
// ★v2 の省略形の意味: `"rigidBody":{}` は【静的コライダー】（motionType 0 / mass 0 / friction 0.8 / restitution 0 / 重力なし）。
//   v1 や構造体の既定（動的）ではない。動的な剛体は motionType / mass などを書く。transform は rotation 0 / scale 1 が省略値で、position は常に書く。
//
// ★JS の数値は float / 整数の区別が無い。C++ 側が "1.0" と書く所を、ここは "1" と書く（JSON として等価で、
//   エンジンは同じ値に読む）。キー順は C++（nlohmann の辞書順）に合わせて再帰的にソートする。

import fs from "node:fs";

export const SCENE_VERSION_V2 = 2;

type Json = null | boolean | number | string | Json[] | { [k: string]: Json };
type JsonObject = { [k: string]: Json };

const isObj = (v: unknown): v is JsonObject => typeof v === "object" && v !== null && !Array.isArray(v);

/** float32 で正確に表せる number なら、float32 として往復する最短の十進表記の number に置き換える。それ以外は触らない。 */
export function f32Shortest(v: number): number {
  if (!Number.isFinite(v) || Math.fround(v) !== v) return v;
  for (let p = 1; p <= 9; p++) {
    const s = Number(v.toPrecision(p));
    if (Math.fround(s) === v) return Object.is(v, -0) ? -0 : s;
  }
  return v;
}

/** 木を再帰で複製しつつ number を f32Shortest で置き換える（キーはソートして返す＝nlohmann の辞書順）。 */
export function normalizeFloats<T extends Json>(x: T): T {
  if (typeof x === "number") return f32Shortest(x) as T;
  if (Array.isArray(x)) return x.map((c) => normalizeFloats(c)) as T;
  if (isObj(x)) {
    const out: JsonObject = {};
    for (const k of Object.keys(x).sort()) out[k] = normalizeFloats(x[k]);
    return out as T;
  }
  return x;
}

/** 厳密な等価。-0 と 0 は別物（float32 のビットが違う）。配列は丸ごと比較。 */
function exactEqual(a: unknown, b: unknown): boolean {
  if (typeof a === "number" && typeof b === "number") return Object.is(a, b);
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((v, i) => exactEqual(v, b[i]));
  if (isObj(a) && isObj(b)) {
    const ka = Object.keys(a);
    return ka.length === Object.keys(b).length && ka.every((k) => k in b && exactEqual(a[k], b[k]));
  }
  return a === b;
}

let tableCache: JsonObject | null = null;
/** 凍結の既定値表（正規化済み）。 */
export function defaultsV2(): JsonObject {
  if (!tableCache) {
    const raw = JSON.parse(fs.readFileSync(new URL("./scene_defaults_v2.json", import.meta.url), "utf8")) as JsonObject;
    tableCache = normalizeFloats(raw);
  }
  return tableCache;
}

/** ルートの version が 2 以上か（無い / 数値でないときは v1）。 */
export function isV2(root: unknown): boolean {
  return isObj(root) && typeof root.version === "number" && Number.isInteger(root.version) && root.version >= SCENE_VERSION_V2;
}

/** エンティティ 1 体から表と完全一致するフィールドを消す（全部消えたコンポーネントは {} のまま残す）。ej を書き換える。 */
export function stripDefaults(ej: JsonObject, table: JsonObject = defaultsV2()): JsonObject {
  for (const [k, v] of Object.entries(ej)) {
    const t = table[k];
    if (!isObj(v) || !isObj(t)) continue;
    for (const [f, d] of Object.entries(t)) if (f in v && exactEqual(v[f], d)) delete v[f];
  }
  return ej;
}

/** 表にあって欠けているフィールドを足す。ej を書き換える。 */
export function inflateDefaults(ej: JsonObject, table: JsonObject = defaultsV2()): JsonObject {
  for (const [k, v] of Object.entries(ej)) {
    const t = table[k];
    if (!isObj(v) || !isObj(t)) continue;
    for (const [f, d] of Object.entries(t)) if (!(f in v)) v[f] = structuredClone(d);
  }
  return ej;
}

/**
 * ディスクから読んだシーン JSON を使う前に必ず通す。version >= 2 なら全エンティティを補完した【複製】を返す。
 * v1（version 無し / 1）や配列でないルートはそのまま返す（複製しない）。
 */
export function inflateScene<T>(root: T): T {
  if (!isV2(root)) return root;
  const r = structuredClone(root) as JsonObject;   // JSON 往復だと -0 の符号が落ちる
  if (Array.isArray(r.entities)) for (const e of r.entities) if (isObj(e)) inflateDefaults(e);
  return r as unknown as T;
}

/**
 * 完全形（各コンポーネントが全フィールドを持つ、エンジンが SaveToString で書く形）を v2 へ変換した複製を返す。
 * ★手書きの「部分的な v1」を渡してはいけない（v1 で欠けたフィールドは構造体の既定、v2 では表の既定になり意味が変わる）。
 *   v2 で書かれたもの（省略形を含む）を整形し直す用途は安全。
 */
export function convertToV2(root: JsonObject): JsonObject {
  const r = normalizeFloats(root) as JsonObject;
  r.version = SCENE_VERSION_V2;
  if (Array.isArray(r.entities)) {
    const table = defaultsV2();
    for (const e of r.entities) {
      if (!isObj(e)) continue;
      if ("parentGuid" in e) delete e.parent;   // index は挿入・削除で全体がずれ差分が広がる。guid が正
      stripDefaults(e, table);
    }
  }
  return r;
}

// JSON.stringify は -0 を "0" と書いて符号を落とす（float32 のビットが変わる）。-0 だけ印を付けて "-0.0" へ戻す。
const NEG0 = "\u0000NEG0\u0000";
function stringify(value: unknown, indent?: number): string {
  const s = JSON.stringify(value, (_k, v) => (Object.is(v, -0) ? NEG0 : v), indent);
  const quoted = JSON.stringify(NEG0);   // 出力に現れる形（NUL は \u0000 にエスケープされる）
  return s.includes(quoted) ? s.split(quoted).join("-0.0") : s;
}

/** v2 の整形: version → 他のルート設定（辞書順・2 スペース整形）→ entities（1 行 1 体）。末尾改行あり・LF のみ。 */
export function dumpSceneV2(root: JsonObject): string {
  const ents = root.entities;
  if (!Array.isArray(ents)) return JSON.stringify(root, null, 2) + "\n";
  const sorted = normalizeFloats(root) as JsonObject;   // キーを辞書順に揃える（数値は変えない: 既に正規化済みなら不変）
  const parts: string[] = [];
  const emit = (k: string) => {
    const body = stringify(sorted[k], 2).replace(/\n/g, "\n  ");
    parts.push(`  ${JSON.stringify(k)}: ${body}`);
  };
  if ("version" in sorted) emit("version");
  for (const k of Object.keys(sorted)) if (k !== "version" && k !== "entities") emit(k);
  const lines = (sorted.entities as Json[]).map((e) => stringify(e));
  const entities = lines.length === 0 ? "[]" : `[\n${lines.join(",\n")}\n  ]`;
  return `{\n${parts.length ? parts.join(",\n") + ",\n" : ""}  "entities": ${entities}\n}\n`;
}
