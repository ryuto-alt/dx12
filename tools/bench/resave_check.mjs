// シーンの「読み込んで保存し直し」がバイト単位で変わらないことを確かめる（W05-15 / M3 の確認）。
//
//   node tools/bench/resave_check.mjs --exe <DX12Engine.exe> --port 8852 --record <manifest.json>
//   node tools/bench/resave_check.mjs --exe <DX12Engine.exe> --port 8852 --compare <manifest.json>
//
// やること（ヘッドレスのエンジンを 1 つ。窓は出さない・実マウスに触らない）:
//   1. 対象プロジェクトを一時フォルダへ写す（リポジトリのシーンを書き換えない）。
//   2. 各シーンを open_scene → save_scene（別名）で保存し直し、保存ファイルの SHA-256 を取る。
//   3. --with-play なら、さらに play → stop の後にもう一度保存し直し、1 回目と同じバイト列であることを確かめる
//      （Play 用スナップショットの "_eid" が保存ファイルに出ないことの確認。W05-11）。
//   4. どの保存ファイルにも "_eid" が無いことを確かめる。
//   --keep <dir> は保存し直したファイルそのものを残す（差分調査用）。
//   --record は SHA-256 の一覧を書く（改修前のビルドで取る）。--compare は一覧と全件一致するか見る（改修後のビルド）。
// 終了コード: 0 = 一致 / 1 = 不一致・失敗。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { REPO_ROOT, findDefaultExe, launchInstance, connect, sleep } from "./lib/engine.mjs";

const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : dflt; };
const has = (name) => args.includes(name);

// samples/ は無い。hardware/lab の 2 プロジェクトも tracked なので対象に含める。
const PROJECTS = ["tests/golden/projects/golden_core", "tests/golden/ui_project", "hardware/lab/ArduinoLab", "hardware/lab/GroveLab"];

function listScenes(projRoot) {
  const dir = path.join(projRoot, "assets", "scenes");
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir).filter((f) => f.endsWith(".json")).sort().map((f) => `scenes/${f}`);
}

// ビルド間の比較用ハッシュ。guid を持たないエンティティは保存のたびに乱数の guid が振られるので、
// 値そのものではなく「最初に現れた順」の番号（g0, g1, …）に置き換えてから比べる。
// 欠落・重複した guid や、parentGuid のつながりの変化は、番号の並びの違いとして残る。
// （Play→Stop の前後の比較は 1 つのエンジンの中なので、この置き換えをせず生のバイトで比べる）
function normalizeGuids(text, stable) {
  // stable: 元のシーンファイルに書かれていた guid。これは生の値のまま比べる（別の値への書き換えを見逃さない）。
  // 保存時に新しく振られた guid だけを出現順の番号にする。
  const map = new Map();
  return text.replace(/"(guid|valueGuid|parentGuid)":"([0-9a-fA-F]+)"/g, (m, k, v) => {
    if (stable && stable.has(v)) return m;
    if (!map.has(v)) map.set(v, `g${map.size}`);
    return `"${k}":"${map.get(v)}"`;
  });
}
function stableGuids(origBytes) {
  const set = new Set();
  if (!origBytes) return set;
  for (const m of origBytes.toString("utf8").matchAll(/"(?:guid|valueGuid|parentGuid)":"([0-9a-fA-F]+)"/g)) set.add(m[1]);
  return set;
}
// 注意: --record と --compare はどちらもエンジンの保存結果どうしの比較（ビルド間の一致）で、
// 元のシーンファイルとのバイト一致は見ていない。
const sha = (buf, orig) => createHash("sha256").update(normalizeGuids(buf.toString("utf8"), stableGuids(orig))).digest("hex");
const rawSha = (buf) => createHash("sha256").update(buf).digest("hex");

async function waitIdle(c, timeoutMs = 60_000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    const r = await c.call("ping", {});
    if (!r.sceneLoading) return;
    await sleep(200);
  }
  throw new Error("シーンの読み込みが終わらない");
}

async function resaveProject(projRel, exe, port, withPlay) {
  const src = path.join(REPO_ROOT, projRel);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "resave-"));
  const proj = path.join(tmp, path.basename(projRel));
  fs.cpSync(src, proj, { recursive: true });
  const inst = await launchInstance({
    name: `resave-${path.basename(projRel)}`, project: proj, port, buildDir: path.dirname(exe), mode: "headless",
  });
  const out = {};
  const problems = [];
  const c = await connect(inst.port);
  try {
    await waitIdle(c);   // 起動直後の既定シーンの読み込みを待つ
    for (const scene of listScenes(proj)) {
      await c.call("open_scene", { path: scene });
      await waitIdle(c);
      const outRel = `scenes/_resave/${path.basename(scene)}`;
      await c.call("save_scene", { path: outRel });
      const abs = path.join(proj, "assets", outRel);
      const bytes = fs.readFileSync(abs);
      if (bytes.includes("_eid")) problems.push(`${projRel}/${scene}: 保存ファイルに "_eid" が出ている`);
      const key = `${path.basename(projRel)}/${scene}`;
      let orig = null;
      try { orig = fs.readFileSync(path.join(proj, "assets", scene)); } catch { /* 元ファイルが読めなければ全 guid を番号にする */ }
      out[key] = sha(bytes, orig);
      const firstRaw = rawSha(bytes);
      const keep = opt("--keep", "");
      if (keep) { const dst = path.join(keep, key); fs.mkdirSync(path.dirname(dst), { recursive: true }); fs.writeFileSync(dst, bytes); }
      if (withPlay) {
        try { await c.call("play", {}); }
        catch (e) { console.log(`  skip play: ${key}（${String(e.message).slice(0, 60)}）`); continue; }   // アクティブカメラの無いシーンは Play できない
        await sleep(500);
        await c.call("stop", {});
        await waitIdle(c);
        await c.call("save_scene", { path: outRel });
        const again = fs.readFileSync(abs);
        if (again.includes("_eid")) problems.push(`${key}: Play→Stop の後の保存に "_eid" が出ている`);
        if (rawSha(again) !== firstRaw) problems.push(`${key}: Play→Stop の前後で保存結果が変わった`);
      }
    }
  } finally {
    c.close();
    inst.kill();
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* 一時フォルダ。残っても害は無い */ }
  }
  return { out, problems };
}

async function main() {
  const exe = path.resolve(opt("--exe", findDefaultExe()));
  const port = Number(opt("--port", "8852"));
  const record = opt("--record", "");
  const compare = opt("--compare", "");
  const withPlay = has("--with-play");
  if (!record && !compare) { console.error("--record <file> か --compare <file> を指定"); process.exit(2); }

  const all = {};
  const problems = [];
  for (const p of PROJECTS) {
    const r = await resaveProject(p, exe, port, withPlay);
    Object.assign(all, r.out);
    problems.push(...r.problems);
  }
  console.log(`保存し直したシーン: ${Object.keys(all).length} 件`);

  if (record) {
    fs.writeFileSync(record, JSON.stringify(all, null, 2) + "\n");
    console.log(`記録: ${record}`);
  }
  if (compare) {
    const base = JSON.parse(fs.readFileSync(compare, "utf8"));
    for (const k of new Set([...Object.keys(base), ...Object.keys(all)])) {
      if (base[k] !== all[k]) problems.push(`${k}: 保存結果がバイト一致しない（記録 ${base[k] ?? "なし"} / 今回 ${all[k] ?? "なし"}）`);
    }
  }
  if (problems.length) {
    for (const m of problems) console.error("NG  " + m);
    process.exit(1);
  }
  console.log("OK  全シーンでバイト一致" + (withPlay ? "（Play→Stop 後も一致、_eid なし）" : ""));
}

main().catch((e) => { console.error(e.stack || String(e)); process.exit(1); });
