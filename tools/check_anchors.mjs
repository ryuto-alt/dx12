// アンカーの lint（W05-31）。使い方: node tools/check_anchors.mjs [--self-test] [--strict] [--quiet]
//
// アンカーの書式（1 行に単独で書く。前後にコメント記号だけ許す）:
//   例: `==== lane-A begin ====` ... `==== lane-A end ====`（A〜D の 4 組）
//   例: `==== lane-A:<名前> begin ====` ... `==== lane-A:<名前> end ====`（名前付き。複数可）
//   コメント記号: C++ は //、CMake は #、Markdown は <!-- ... -->
//   ※このヘッダ自身がアンカーに見えないよう、書式の例は必ず文章の途中に（バッククォートで）書く。
// 検査:
//   - begin / end が同じレーン・同じ名前で対になる（入れ子・交差・取り残しは赤。名前付きブロックも他のブロックの中には置けない）
//   - 名前の無いブロックを 1 つでも持つファイルは、A〜D の 4 つがそれぞれちょうど 1 回ある
//   - 名前付きブロックは (レーン, 名前) ごとに 1 回だけ
//   - 空のブロックは許す
// 旧書式（==== wave<N>-lane<X> ==== begin/end）:
//   ルート CMakeLists.txt の DXC / シェーダ区画の 36 行 = 18 組は許可リストで通す（M3 で W05-16 が消したら空にする）。
//   それ以外のファイルの旧書式は W05-01 が新書式へ移すまでの暫定で、件数を報告して通す。
//   --strict を付けると、許可リスト以外の旧書式はすべて赤になる（W05-01 のマージ後に使う）。
//   追跡条件: W05-01 のマージ後（M0）に、ctest の AnchorLint を --strict で登録するか、この既定を strict に切り替える。
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join, extname } from "node:path";
import { fileURLToPath } from "node:url";

// 旧書式を許すファイル（常に通す）。M3 で W05-16 が区画を消したら、この配列を空にする。
export const LEGACY_ALLOWLIST = [];
// 許可リストのファイルごとの旧書式の行数（期待値）。食い違えば赤にする（取りこぼしや、区画が消えて古くなった項目の検出）。
export const LEGACY_EXPECTED_LINES = {};

const NEW_RE = /^(?:\/\/|#|<!--)?\s*={4} lane-([A-D])(?::([A-Za-z0-9_.-]+))? (begin|end) ={4}\s*(?:-->)?\s*$/;
const BAD_RE = /^(?:\/\/|#|<!--|\/\*|--|;|REM\b)?\s*={4}\s*lane-\S*\s+(?:begin|end)\b/i;
const LEGACY_RE = /^\s*(?:\/\/|#|<!--|\/\*|--|;|REM\b)?\s*={4} wave\d+-lane[A-Za-z]\b.*?={4} (?:begin|end)\b/;
// 名前に空白が入った begin/end（`lane-A:foo bar begin`）。NEW_RE に合わないので別に拾う。
const BAD_NAME_RE = /^(?:\/\/|#|<!--|\/\*|--|;|REM\b)?\s*={4} lane-[A-Da-d]:\S*\s+\S.*?\s(?:begin|end) ={4}/;
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
  const stack = []; // 開いているブロック（{key, ln}）。入れ子・交差は赤
  const seenBlocks = new Map(); // key -> begin の出現回数
  let unnamed = 0;
  let fence = null; // Markdown のコードフェンス内（開きの記号 ``` か ~~~）は説明用の例なので対象外
  let fenceLn = 0;
  const isMd = /\.(md|markdown)$/i.test(name);
  lines.forEach((raw, i) => {
    const ln = i + 1;
    const trimmed = raw.trim();
    if (isMd) {
      const fm = /^(```|~~~)/.exec(trimmed);
      if (fm && !fence) {
        fence = fm[1];
        fenceLn = ln;
        return;
      }
      if (fm && fm[1] === fence) {
        fence = null;
        return;
      }
    }
    if (fence) return;
    const m = NEW_RE.exec(trimmed);
    if (!m) {
      if (BAD_RE.test(trimmed) || BAD_NAME_RE.test(trimmed)) errors.push(`${name}:${ln}: アンカーの書式が不正です: ${trimmed}`);
      if (LEGACY_RE.test(raw) && !legacyAllowed) {
        legacy++;
        if (strict) errors.push(`${name}:${ln}: 旧書式のアンカーです（--strict）: ${trimmed}`);
      }
      return;
    }
    const [, lane, label, kind] = m;
    const key = label ? `${lane}:${label}` : lane;
    if (kind === "begin") {
      if (stack.length > 0) {
        const top = stack[stack.length - 1];
        errors.push(`${name}:${ln}: lane-${key} の begin が lane-${top.key}（${top.ln} 行）の中にあります。入れ子は不可`);
      }
      stack.push({ key, ln });
      const c = (seenBlocks.get(key) ?? 0) + 1;
      seenBlocks.set(key, c);
      if (c === 2) errors.push(`${name}:${ln}: lane-${key} のブロックが重複しています`);
      if (!label) unnamed++;
    } else if (stack.length === 0 || stack[stack.length - 1].key !== key) {
      const top = stack[stack.length - 1];
      errors.push(`${name}:${ln}: lane-${key} の end が対応する begin と合いません（直近の begin は ${top ? `lane-${top.key} ${top.ln} 行` : "なし"}）`);
      const idx = stack.map((b) => b.key).lastIndexOf(key);
      if (idx >= 0) stack.splice(idx, 1);
    } else {
      stack.pop();
    }
  });
  if (fence) errors.push(`${name}:${fenceLn}: Markdown のコードフェンスが閉じていません`);
  for (const { key, ln } of stack) errors.push(`${name}:${ln}: lane-${key} の begin に対応する end がありません`);
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
  t(checkText("/* ==== lane-A begin ==== */", "a").errors.length > 0, "/* */ 形式は不正として赤");
  t(checkText("-- ==== lane-A begin ====", "a").errors.length > 0, "-- 形式は不正として赤");
  t(checkText("// ==== Lane-A begin ====", "a").errors.length > 0, "大文字の Lane は赤");
  t(checkText("// ====  lane-A begin ====", "a").errors.length > 0, "空白が 2 つは赤");
  t(checkText("```\n// ==== lane-A begin ====\n```", "a.md").errors.length === 0, "Markdown のフェンス内は対象外");
  t(checkText("```\n// ==== lane-A begin ====\n```", "a.cpp").errors.length > 0, "Markdown 以外ではフェンスを見ない");
  t(checkText(readFileSync(fileURLToPath(import.meta.url), "utf8"), "self").errors.length === 0, "このファイル自身が緑");
  t(checkText("//   ==== lane-A begin ====   ...   ==== lane-A end ====", "a").errors.length > 0, "行頭アンカー風の説明行は赤（ヘッダに書かない）");
  const A = (k, kind) => `// ==== lane-${k} ${kind} ====`;
  const J = (...a) => a.join("\n");
  t(checkText(J(A("A", "begin"), A("B", "begin"), A("A", "end"), A("B", "end")), "a").errors.length > 0, "交差は赤");
  t(checkText(J(A("A", "begin"), A("B", "begin"), A("B", "end"), A("A", "end")), "a").errors.length > 0, "レーン間の入れ子は赤");
  t(checkText(J(A("A", "begin"), A("A:x", "begin"), A("A:x", "end"), A("A", "end")), "a").errors.length > 0, "名前付きの入れ子も赤");
  t(checkText(J(A("A:x", "begin"), A("A:x", "end"), A("B:y", "begin"), A("B:y", "end")), "a").errors.length === 0, "並びは緑");
  t(checkText("// ==== lane-A:foo bar begin ====", "a").errors.length > 0, "名前の空白は赤");
  t(checkText(J("```", "foo"), "a.md").errors.length > 0, "閉じていないフェンスは赤");
  t(checkText(J("```", "~~~", A("A", "begin"), "```"), "a.md").errors.length === 0, "``` の中の ~~~ は閉じ扱いにしない");
  t(checkText(J("~~~", "```", "~~~", A("A", "begin")), "a.md").errors.length > 0, "~~~ で閉じた後は検査する");
  t(checkText("説明 ==== wave1-laneA ==== begin", "a", { strict: true }).errors.length === 0, "旧書式は行頭のみ（文中は対象外）");
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
  const allowedSeen = new Set();
  for (const f of files) {
    if (SKIP_EXT.has(extname(f).toLowerCase())) continue;
    let text;
    try {
      text = readFileSync(join(repoRoot, f), "utf8");
    } catch {
      continue; // 消えたファイルなど
    }
    if (!text.includes("====")) continue;
    const allowed = LEGACY_ALLOWLIST.includes(f);
    const r = checkText(text, f, { strict, legacyAllowed: allowed });
    errors.push(...r.errors);
    if (allowed) {
      const n = text.split(/\r?\n/).filter((l) => LEGACY_RE.test(l)).length;
      const want = LEGACY_EXPECTED_LINES[f];
      allowedSeen.add(f);
      if (n === 0) errors.push(`${f}: 許可リストに載っているが旧書式が 0 行です（許可リストから外すこと）`);
      else if (want !== undefined && n !== want) errors.push(`${f}: 旧書式が ${n} 行（期待 ${want}）。許可リストの外へ増えたか、消えた可能性があります`);
    }
    if (r.legacy > 0) {
      legacyFiles++;
      legacyLines += r.legacy;
    }
    if (text.includes("==== lane-")) anchored++;
  }
  for (const f of LEGACY_ALLOWLIST) {
    if (!allowedSeen.has(f)) errors.push(`${f}: 許可リストに載っているがファイルがありません（許可リストから外すこと）`);
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

// junction / シンボリックリンク / subst 経由でも確実に走らせるため、argv[1] との比較はしない（import 先は無い）。
process.exitCode = main();
