// 項目ブリーフの生成。EXEC_*.md(200〜350KB)から 1 項目ぶんだけを抜き出し、8KB 以下の 1 ファイルにする。
// エージェントが毎回 EXEC 全体を読み直さずに済むようにするための道具(docs/AGENT_KIT.md「ブリーフ」の節)。
//
//   node tools/briefs/make_brief.mjs <EXEC_*.md> <項目ID> [--out brief.md] [--max 8192] [--lane A]
//   node tools/briefs/make_brief.mjs <EXEC_*.md> --list                 # 項目 ID と見出しの一覧
//   node tools/briefs/make_brief.mjs <EXEC_*.md> --all --out-dir briefs/  # 全項目を <ID>.md に書く
//
// 入るもの(優先度の高い順。8KB を超えるときは本文の中ほどから削る):
//   1. 見出しと出典(ファイルの行範囲。省略したときの「続きはここ」)
//   2. 受け入れ(項目の「受け入れ」の段落。2.8KB を超えるときだけ、切った旨の注記つきで切る)
//   3. 所有(その項目のレーンの行/列と、凍結ファイルの行。所有表から)
//   4. §0 の要約(文書の表題と冒頭の箇条書き。最大 1KB)
//   5. 項目の本文(受け入れの段落を除いたもの。残りの予算の全部)
// 項目 = `#`〜`######` の見出しで、最初の語が ID と一致するもの(例 `##### A9 GG-5 …（A、L=4、…）`)。
// 見出しの `（<レーン>、<S|M|L>=<点>、` からレーンを読む。--lane で上書きできる。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const enc = new TextEncoder();
export const byteLen = (s) => enc.encode(s).length;

/** 文字列を、バイト数が max 以下になるまで、行の境目(なければ文字)で後ろから削る。 */
export function truncBytes(s, max) {
  if (byteLen(s) <= max) return s;
  const lines = s.split("\n");
  let out = "";
  for (const l of lines) {
    const next = out ? `${out}\n${l}` : l;
    if (byteLen(next) > max) break;
    out = next;
  }
  if (!out) { // 1 行目だけで超える: 文字単位で削る
    let n = s.length;
    while (n > 0 && byteLen(s.slice(0, n)) > max) n--;
    out = s.slice(0, n);
  }
  return out;
}

/** 見出し行を全部拾う。 */
export function parseHeadings(text) {
  const lines = text.split(/\r?\n/);
  const heads = [];
  let inFence = false;
  lines.forEach((l, i) => {
    if (/^```/.test(l)) { inFence = !inFence; return; }
    if (inFence) return;
    const m = /^(#{1,6})\s+(.*)$/.exec(l);
    if (m) heads.push({ line: i, level: m[1].length, title: m[2].trim(), id: (/^[A-Za-z][A-Za-z0-9._-]*/.exec(m[2].trim())?.[0] ?? m[2].trim().split(/\s+/)[0] ?? "") });
  });
  return { lines, heads };
}

/** 見出しからレーンを読む。例 `…（A、L=4、依存 A1）` -> "A"、`…（L1、S=1、…）` -> "L1" */
export function laneOf(title) {
  if (!/依存|[SML]\s*=\s*\d/.test(title)) return null;
  const m = /（(?:\*\*)?([A-Za-z][A-Za-z0-9-]*)(?:\*\*)?(?:（[^）]*）)?、/.exec(title);
  return m ? m[1] : null;
}

/** 項目の一覧(ID と見出し)。ID は「最初の語が英数字で始まり、見出しにレーンと規模がある」もの。 */
export function listItems(text) {
  const { heads } = parseHeadings(text);
  return heads.filter((h) => laneOf(h.title) && /^[A-Za-z][A-Za-z0-9._-]*$/.test(h.id)).map((h) => ({ id: h.id, title: h.title, line: h.line + 1, lane: laneOf(h.title) }));
}

/** 項目の範囲(見出しの行〜次の同じか上位の見出しの直前)。 */
export function findItem(text, id) {
  const { lines, heads } = parseHeadings(text);
  const idx = heads.findIndex((h) => h.id.toLowerCase() === id.toLowerCase() && laneOf(h.title));
  const i = idx >= 0 ? idx : heads.findIndex((h) => h.id.toLowerCase() === id.toLowerCase());
  if (i < 0) return null;
  const h = heads[i];
  let end = lines.length;
  for (let j = i + 1; j < heads.length; j++) { if (heads[j].level <= h.level) { end = heads[j].line; break; } }
  while (end > h.line + 1 && /^(---)?\s*$/.test(lines[end - 1])) end--;
  return { id: h.id, title: h.title, lane: laneOf(h.title), startLine: h.line, endLine: end, lines: lines.slice(h.line, end) };
}

/** 項目本文から「受け入れ」の段落を分ける(`- **受け入れ**:` から次の `- **` までと、その続きの行)。 */
export function splitAcceptance(itemLines) {
  const acc = [], rest = [];
  let inAcc = false;
  for (const l of itemLines) {
    if (/^\s*[-*]\s+\*\*受け入れ/.test(l)) { inAcc = true; acc.push(l); continue; }
    if (inAcc && (/^\s*[-*]\s+\*\*/.test(l) || /^#{1,6}\s/.test(l))) inAcc = false;
    (inAcc ? acc : rest).push(l);
  }
  return { acceptance: acc.join("\n").trim(), body: rest.join("\n").trim() };
}

const cells = (row) => row.replace(/^\s*\|/, "").replace(/\|\s*$/, "").split("|").map((c) => c.trim());

/** 所有表(見出しに「所有」を含む節)から、そのレーンに関係する行/列と、凍結の行を拾う。 */
export function extractOwnership(text, lane, cap = 1800) {
  const { lines, heads } = parseHeadings(text);
  const out = [];
  const secs = heads.map((h, i) => ({ h, end: (heads.slice(i + 1).find((x) => x.level <= h.level)?.line ?? lines.length) })).filter((s) => /所有/.test(s.h.title));
  const lt = lane ? lane.toLowerCase() : null;
  const norm = (c) => c.replace(/[*`]/g, "").trim().toLowerCase();
  for (const s of secs) {
    let header = null;
    for (let k = s.h.line + 1; k < s.end; k++) {
      const l = lines[k];
      if (/凍結/.test(l) && /^\s*[-*]/.test(l)) out.push(l.trim());
      if (!/^\s*\|/.test(l)) { header = null; continue; }
      const c = cells(l);
      if (c.every((x) => /^:?-{2,}:?$/.test(x))) continue;
      if (!header) { header = c; continue; }                        // 表の 1 行目 = 見出し行
      if (!lt) continue;
      const col = header.findIndex((x) => norm(x) === lt);
      if (col >= 0 && col < c.length) { out.push(`[${header[col]}] ${c[col]}`); continue; }  // レーンが列の表
      const last = norm(c[c.length - 1] ?? "");
      if (last === lt || last.split(/[、,/・ ]+/).includes(lt) || last === `lane-${lt}`) out.push(c.slice(0, -1).join(" | "));    // レーンが最後の列の表
    }
  }
  const joined = [...new Set(out)].map((x) => (byteLen(x) > 700 ? truncBytes(x, 700) + "…" : x)).join("\n");
  return truncBytes(joined, cap);
}

/** 文書の表題と冒頭の箇条書き(最初の `---` か `## ` の前)。 */
export function extractSummary(text, cap = 1000) {
  const lines = text.split(/\r?\n/);
  const out = [];
  for (const l of lines) { if (/^---\s*$/.test(l) || /^##\s/.test(l)) break; out.push(l.length > 360 ? l.slice(0, 360) + "…" : l); }
  return truncBytes(out.join("\n").trim(), cap);
}

/**
 * ブリーフ本文を作る。戻り値: { text, bytes, truncated, item }。見つからなければ null。
 * options: { maxBytes=8192, lane, source="EXEC.md" }
 */
export function makeBrief(text, id, options = {}) {
  const { maxBytes = 8192, source = "EXEC.md" } = options;
  const item = findItem(text, id);
  if (!item) return null;
  const lane = options.lane ?? item.lane;
  const { acceptance, body } = splitAcceptance(item.lines.slice(1));
  const summary = extractSummary(text, 1000);
  const owner = extractOwnership(text, lane, 1800);
  const where = `${source}:${item.startLine + 1}-${item.endLine}`;
  const head = `# ブリーフ: ${item.title}\n\n出典: ${where}(足りないときだけ、この範囲を読む)。レーン: ${lane ?? "不明"}\n`;
  const accOut = byteLen(acceptance) > 2800 ? `${truncBytes(acceptance, 2800)}\n…(受け入れは ${byteLen(acceptance)} バイトあり、ここには約 2800 バイト。続きは ${where})` : acceptance;
  const accBlock = acceptance ? `\n## 受け入れ\n${accOut}\n` : "\n## 受け入れ\n(この項目に「受け入れ」の段落が無い。本文の「設計」と「レビュー観点」を見る)\n";
  const ownBlock = owner ? `\n## 所有と凍結(レーン ${lane ?? "?"})\n${owner}\n` : "";
  const sumBlock = summary ? `\n## 文書の要約(§0)\n${summary}\n` : "";
  const fixed = head + accBlock + ownBlock + sumBlock;
  const bodyHead = "\n## 項目の本文\n";
  let room = maxBytes - byteLen(fixed) - byteLen(bodyHead) - 200;   // 200 = 省略の注記ぶん
  let truncated = false;
  let bodyOut = body;
  if (room < 400) {  // 固定部が大きすぎる: 要約と所有を縮める
    const sum2 = truncBytes(summary, 300), own2 = truncBytes(owner, 900);
    const fixed2 = head + accBlock + (own2 ? `\n## 所有と凍結(レーン ${lane ?? "?"})\n${own2}\n` : "") + (sum2 ? `\n## 文書の要約(§0)\n${sum2}\n` : "");
    room = maxBytes - byteLen(fixed2) - byteLen(bodyHead) - 200;
    if (room < 200) room = 200;
    var fixedUse = fixed2;
  } else { var fixedUse = fixed; }
  if (byteLen(bodyOut) > room) {
    truncated = true;
    const keepHead = Math.floor(room * 0.7), keepTail = room - keepHead;
    const h = truncBytes(bodyOut, keepHead);
    // 末尾も残す(「レビュー観点」や禁止事項が末尾にあることが多い)
    const tailLines = bodyOut.split("\n").reverse();
    let t = "";
    for (const l of tailLines) { const n = t ? `${l}\n${t}` : l; if (byteLen(n) > keepTail) break; t = n; }
    bodyOut = `${h}\n\n…(中略。本文は ${byteLen(body)} バイト、ここには約 ${room} バイト。続きは ${where})…\n\n${t}`;
  }
  let out = fixedUse + bodyHead + bodyOut + "\n";
  if (byteLen(out) > maxBytes) out = truncBytes(out, maxBytes - 40) + "\n…(上限で切った)\n";
  return { text: out, bytes: byteLen(out), truncated, item: { id: item.id, title: item.title, lane } };
}

function parseCli(argv) {
  const a = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const x = argv[i];
    if (x === "--out") a.out = argv[++i];
    else if (x === "--out-dir") a.outDir = argv[++i];
    else if (x === "--max") a.max = Number(argv[++i]);
    else if (x === "--lane") a.lane = argv[++i];
    else if (x === "--list") a.list = true;
    else if (x === "--all") a.all = true;
    else a._.push(x);
  }
  return a;
}

export async function main(argv) {
  const a = parseCli(argv);
  const file = a._[0];
  if (!file || (!a.list && !a.all && !a._[1])) {
    console.error("usage: node tools/briefs/make_brief.mjs <EXEC_*.md> <ID> [--out f] [--max 8192] [--lane X] | --list | --all --out-dir <dir>");
    return 2;
  }
  const text = fs.readFileSync(file, "utf8");
  const source = path.basename(file);
  if (a.list) { for (const it of listItems(text)) console.log(`${it.id}\t${it.lane}\t${source}:${it.line}\t${it.title}`); return 0; }
  const maxBytes = a.max && a.max > 0 ? a.max : 8192;
  if (a.all) {
    if (!a.outDir) { console.error("--all には --out-dir が要る"); return 2; }
    fs.mkdirSync(a.outDir, { recursive: true });
    let n = 0, big = 0;
    for (const it of listItems(text)) {
      const b = makeBrief(text, it.id, { maxBytes, source, lane: a.lane });
      if (!b) continue;
      fs.writeFileSync(path.join(a.outDir, `${it.id.replace(/[^\w.-]/g, "_")}.md`), b.text);
      n++; if (b.truncated) big++;
    }
    console.log(`${n} briefs -> ${a.outDir} (${big} truncated)`);
    return 0;
  }
  const b = makeBrief(text, a._[1], { maxBytes, source, lane: a.lane });
  if (!b) {
    console.error(`項目 ${a._[1]} が見つからない。--list で一覧: ${listItems(text).slice(0, 12).map((i) => i.id).join(", ")} …`);
    return 1;
  }
  if (a.out) { fs.mkdirSync(path.dirname(path.resolve(a.out)), { recursive: true }); fs.writeFileSync(a.out, b.text); console.log(`${a.out}: ${b.bytes} bytes${b.truncated ? " (本文を省略)" : ""}`); }
  else process.stdout.write(b.text);
  return 0;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main(process.argv.slice(2)).then((c) => process.exit(c), (e) => { console.error(e); process.exit(1); });
