// ゴールデンの設定と基準の入出力（純粋なファイル操作。GPU もエンジンも要らない・単体テストあり）。
//
// W05-32（W0-9）: 複数のレーンが同じファイルの末尾を書き換えて衝突しないよう、1 件 1 ファイルに分けた。
//   設定: tests/golden/entries/<id>.json          … 1 シーンの定義（name は <id> と一致）
//   基準: tests/golden/baseline/<id>.json         … 1 シーンのネイティブ解像度ハッシュなどの記録
//   共通: tests/golden/golden.json                … $comment / defaults / order（読み込み順の固定。省略可）。scenes は空（互換）
// 読み込み順: golden.json の order に載っている ID をその順に、残りをファイル名の辞書順に並べる。
//   order は既存 22 件の実行順を分割の前後で変えないためのもの。新しいゴールデンは entries に足すだけでよい（order へは書かない）。
// ID の重複（entries どうし、または互換の scenes との重複）はエラー。
import fs from "node:fs";
import path from "node:path";

const ID_RE = /^[A-Za-z0-9][A-Za-z0-9_.-]*$/;
const byName = (a, b) => (a < b ? -1 : a > b ? 1 : 0);   // ロケールに依存しない辞書順

function listJson(dir) {
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir).filter((f) => f.endsWith(".json")).sort(byName);
}

/** entries/*.json と golden.json から、読み込み順に並べたシーン定義（defaults 適用前）と defaults を返す。 */
export function loadGoldenEntries(goldenDir) {
  const cfgFile = path.join(goldenDir, "golden.json");
  const cfg = fs.existsSync(cfgFile) ? JSON.parse(fs.readFileSync(cfgFile, "utf8")) : {};
  const map = new Map();
  const add = (s, origin) => {
    if (!s || typeof s.name !== "string" || !ID_RE.test(s.name)) throw new Error(`${origin}: name（ID）が不正`);
    if (map.has(s.name)) throw new Error(`ゴールデン ID の重複: ${s.name}（${origin}）`);
    map.set(s.name, s);
  };
  for (const s of cfg.scenes ?? []) add(s, "golden.json の scenes");
  for (const f of listJson(path.join(goldenDir, "entries"))) {
    const s = JSON.parse(fs.readFileSync(path.join(goldenDir, "entries", f), "utf8"));
    if (s.name !== path.basename(f, ".json")) throw new Error(`entries/${f}: name "${s.name}" がファイル名と一致しない`);
    add(s, `entries/${f}`);
  }
  const ordered = [];
  for (const id of cfg.order ?? []) {
    if (!map.has(id)) throw new Error(`golden.json の order に載っている ID が entries に無い: ${id}`);
    if (!ordered.includes(id)) ordered.push(id);
  }
  const rest = [...map.keys()].filter((id) => !ordered.includes(id)).sort(byName);
  return { defaults: cfg.defaults ?? {}, scenes: [...ordered, ...rest].map((id) => map.get(id)) };
}

/** baseline/<id>.json をすべて読み、{ scenes: { <id>: 記録 } } を返す（辞書順。ファイル名と対応する ID だけ）。 */
export function loadBaseline(goldenDir) {
  const dir = path.join(goldenDir, "baseline");
  const scenes = {};
  for (const f of listJson(dir)) scenes[path.basename(f, ".json")] = JSON.parse(fs.readFileSync(path.join(dir, f), "utf8"));
  return { scenes };
}

/** 指定した ID の基準だけを書く（他の ID のファイルには触れない。内容が同じなら書き直さない）。 */
export function writeBaselineEntries(goldenDir, baseline, ids) {
  const dir = path.join(goldenDir, "baseline");
  fs.mkdirSync(dir, { recursive: true });
  const written = [];
  for (const id of ids) {
    if (!ID_RE.test(id)) throw new Error(`baseline の ID が不正: ${id}`);
    const rec = baseline.scenes[id];
    if (!rec) continue;
    const file = path.join(dir, `${id}.json`);
    const text = JSON.stringify(rec, null, 2) + "\n";
    if (fs.existsSync(file) && fs.readFileSync(file, "utf8") === text) continue;
    fs.writeFileSync(file, text);
    written.push(id);
  }
  return written;
}
