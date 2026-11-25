// アンカーの lint（W05-31）。使い方: node tools/check_anchors.mjs [--self-test] [--strict] [--quiet]
//
// アンカーの書式（1 行に単独で書く。前後にコメント記号だけ許す）:
//   ==== lane-A begin ====   ...   ==== lane-A end ====          （A〜D の 4 組）
//   ==== lane-A:<名前> begin ====  ...  ==== lane-A:<名前> end ====   （名前付き。複数可）
//   コメント記号: C++ は //、CMake は #、Markdown は <!-- ... -->
// 検査:
//   - begin / end が同じレーン・同じ名前で対になる（入れ子・取り残しは赤）
//   - 名前の無いブロックを 1 つでも持つファイルは、A〜D の 4 つがそれぞれちょうど 1 回ある
//   - 名前付きブロックは (レーン, 名前) ごとに 1 回だけ
//   - 空のブロックは許す
// 旧書式（==== wave<N>-lane<X> ==== begin/end）:
//   ルート CMakeLists.txt の DXC / シェーダ区画の 18 組は許可リストで通す（M3 で W05-16 が消したら空にする）。
//   それ以外のファイルの旧書式は W05-01 が新書式へ移すまでの暫定で、件数を報告して通す。
//   --strict を付けると、許可リスト以外の旧書式はすべて赤になる（W05-01 のマージ後に使う）。
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join, extname } from "node:path";
import { fileURLToPath } from "node:url";

// 旧書式を許すファイル（常に通す）。M3 で W05-16 が区画を消したら、この配列を空にする。
export const LEGACY_ALLOWLIST = ["CMakeLists.txt"];

const NEW_RE = /^(?:\/\/|#|<!--)?\s*={4} lane-([A-D])(?::([A-Za-z0-9_.-]+))? (begin|end) ={4}\s*(?:-->)?\s*$/;
const BAD_RE = /^(?:\/\/|#|<!--)?\s*={4} lane-\S*\s+(?:begin|end)\b/;
const LEGACY_RE = /={4} wave\d+-lane[A-Za-z]\b.*?={4} (?:begin|end)\b/;
const LANES = ["A", "B", "C", "D"];

const SKIP_EXT = new Set([
  ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".dds", ".hdr", ".exr", ".tga", ".wav", ".ogg", ".mp3",
  ".ttf", ".otf", ".woff", ".woff2", ".dll", ".exe", ".lib", ".pdb", ".zip", ".7z", ".bin", ".pdf", ".fbx",
  ".glb", ".gltf", ".obj", ".uasset", ".spv", ".dxil", ".cso", ".rc", ".lock",
]);

/** 1 ファイルの本文を検査し、{errors, legacy} を返す（errors は文字列の配列、legacy は旧書式の行数） */
export function checkText(text, name, { strict = false, legacyAllowed = false } = {}) {
  const errors = [];
  let legacy = 0;
  const lines = text.split(/\r?\n/);
  const open = new Map(); // key -> begin の行番号
  const seenBlocks = new Map(); // key -> begin の出現回数
  let unnamed = 0;
  lines.forEach((raw, i) => {
    const ln = i + 1;
    const trimmed = raw.trim();
    const m = NEW_RE.exec(trimmed);
    if (!m) {
      if (BAD_RE.test(trimmed)) errors.push(`${name}:${ln}: アンカーの書式が不正です: ${trimmed}`);
      if (LEGACY_RE.test(raw) && !legacyAllowed) {
        legacy++;
        if (strict) errors.push(`${name}:${ln}: 旧書式のアンカーです（--strict）: ${trimmed}`);
      }
      return;
    }
    const [, lane, label, kind] = m;
    const key = label ? `${lane}:${label}` : lane;
    if (kind === "begin") {
      if (open.has(key)) errors.push(`${name}:${ln}: lane-${key} の begin が入れ子/重複です（前の begin は ${open.get(key)} 行）`);
      else open.set(key, ln);
      const c = (seenBlocks.get(key) ?? 0) + 1;
      seenBlocks.set(key, c);
      if (c === 2) errors.push(`${name}:${ln}: lane-${key} のブロックが重複しています`);
      if (!label) unnamed++;
    } else if (!open.has(key)) {
      errors.push(`${name}:${ln}: lane-${key} の end に対応する begin がありません`);
    } else {
      open.delete(key);
    }
  });
  for (const [key, ln] of open) errors.push(`${name}:${ln}: lane-${key} の begin に対応する end がありません`);
  if (unnamed > 0) {
    for (const l of LANES) {
      if (!seenBlocks.has(l)) errors.push(`${name}: lane-${l} のブロックがありません（A〜D の 4 つが必要）`);
    }
  }
  return { errors, legacy };
}

function trackedFiles(repoRoot) {
  const out = execFileSync("git", ["-C", repoRoot, "ls-files", "-z"], { maxBuffer: 256 * 1024 * 1024 });
  return out.toString("utf8").split("\0").filter(Boolean);
}

function selfTest() {
  const t = (cond, msg) => {
    if (!cond) throw new Error("self-test 失敗: " + msg);
  };
  const full = (c) => LANES.map((l) => `${c} ==== lane-${l} begin ====\n${c} ==== lane-${l} end ====`).join("\n");
  const named = (l, n) => `// ==== lane-${l}:${n} begin ====\n// ==== lane-${l}:${n} end ====`;
  t(checkText(full("//"), "a").errors.length === 0, "空のブロックは許す");
  t(checkText(full("#"), "a").errors.length === 0, "# 形式");
  t(checkText(LANES.map((l) => `<!-- ==== lane-${l} begin ==== -->\n<!-- ==== lane-${l} end ==== -->`).join("\n"), "a").errors.length === 0, "markdown 形式");
  t(checkText(full("//").replace("lane-C begin", "lane-D begin"), "a").errors.length > 0, "C が無く D が重複");
  t(checkText(full("//") + "\n" + full("//"), "a").errors.length > 0, "名前の無いブロックの重複");
  t(checkText(full("//").replace("// ==== lane-B end ====", ""), "a").errors.length > 0, "end 欠落");
  t(checkText(full("//") + "\n" + named("A", "x") + "\n" + named("B", "x"), "a").errors.length === 0, "名前付きは複数可");
  t(checkText(full("//") + "\n" + named("A", "x") + "\n" + named("A", "x"), "a").errors.length > 0, "名前付きの重複");
  t(checkText("// ==== lane-E begin ====", "a").errors.length > 0, "不正なレーン");
  t(checkText("// ==== lane-a begin ====", "a").errors.length > 0, "小文字のレーン");
  t(checkText("// ==== wave1-laneA ==== begin\n// ==== wave1-laneA ==== end", "a").legacy === 2, "旧書式を数える");
  t(checkText("// ==== wave1-laneA (登録) ==== begin", "a").legacy === 1, "旧書式（登録）");
  t(checkText("// ==== wave1-laneA ==== begin", "a", { strict: true }).errors.length > 0, "--strict");
  t(checkText("// ==== wave1-laneA ==== begin", "a", { strict: true, legacyAllowed: true }).errors.length === 0, "許可リスト");
  t(checkText("説明: `==== lane-A begin ====` と書く", "a").errors.length === 0, "文中の引用は対象外");
  t(checkText("", "a").errors.length === 0, "アンカーの無いファイル");
  console.log("self-test OK");
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes("--self-test")) {
    selfTest();
    return 0;
  }
  const strict = args.includes("--strict");
  const quiet = args.includes("--quiet");
  const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
  const files = trackedFiles(repoRoot);
  const errors = [];
  let legacyFiles = 0;
  let legacyLines = 0;
  let anchored = 0;
  for (const f of files) {
    if (SKIP_EXT.has(extname(f).toLowerCase())) continue;
    let text;
    try {
      text = readFileSync(join(repoRoot, f), "utf8");
    } catch {
      continue; // 消えたファイルなど
    }
    if (!text.includes("====")) continue;
    const r = checkText(text, f, { strict, legacyAllowed: LEGACY_ALLOWLIST.includes(f) });
    errors.push(...r.errors);
    if (r.legacy > 0) {
      legacyFiles++;
      legacyLines += r.legacy;
    }
    if (text.includes("==== lane-")) anchored++;
  }
  if (errors.length > 0) {
    console.error(errors.join("\n"));
    console.error(`\ncheck_anchors: ${errors.length} 件の問題`);
    return 1;
  }
  if (!quiet) {
    console.log(
      `check_anchors OK: ${files.length} ファイルを検査（新書式 ${anchored} ファイル、旧書式の暫定 ${legacyFiles} ファイル/${legacyLines} 行、許可リスト: ${LEGACY_ALLOWLIST.join(",")}）`,
    );
  }
  return 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main();
