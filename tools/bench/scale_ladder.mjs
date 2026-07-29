// スケール梯子 — 「エンティティ数を増やしたとき何が最初に壊れるか」を測る。
//
// なぜ要るか:
//   規模を上げる作業（ジョブシステム、GPU 駆動描画、ストリーミング）はどれも大きい。
//   どれが要るかは憶測ではなく計測で決める。実際このハーネスの初回計測で
//   「ジョブシステムを先に作る」案は棄却された（下の基準値を参照）。
//   同時に、規模方向の性能退行を検出する回帰テストにもなる。
//
// 使い方:
//   1. PerfTest プロジェクト（assets/scenes/stress_*.json がある方）を開いた状態で
//      エンジンを起動しておく。プロジェクトが違えば自動で開き直す。
//   2. node tools/bench/scale_ladder.mjs [シーン名...]
//      省略すると stress_500 → stress_100000 の 7 段を回す。
//
// 注意:
//   MCP ブリッジは単一クライアント（listen backlog 1）なので、接続は 1 本だけ張って
//   使い回すこと。接続を張り直すと後続が connection refused になる。
//   benchmark は既定で FPS 上限と VSync を外す（uncap:true）＝素のスループットを測る。
//
// ------------------------------------------------------------------------------
// 基準値（2026-07-30 / RTX 系 1 台 / 1052x592 / 影 2048^2 x 4 カスケード）
//
//   scene            ents    fps   frame    gpu  shadow   main  draws  tris(M)
//   stress_500        503   68.8   14.53  14.50    8.45   5.95     15     65.9
//   stress_5000      5003   54.4   18.39  18.36   11.30   6.97     16     90.7
//   stress_100000  100003   52.5   19.05  19.02   11.93   7.00     16     94.4
//
//   読み込み: 60ms(500) → 4563ms(100k)、保存: 20ms → 876ms、診断 4 種: 19ms → 1146ms
//   ＝どれもエンティティ数に対して線形。二次で膨らむ箇所は無い。
//
// この数字から読めること:
//   ・エンティティを 200 倍にしても drawCall は 15→16。インスタンシングが効いている。
//   ・全段で GPU 律速（cpu.fenceWaitMs > 0 ＝ CPU は GPU を待っている）。
//     したがって CPU 側を速くしてもフレームは縮まない。ジョブシステムは今は効かない。
//   ・GPU の内訳は毎段 影が約 60%。伸ばすならここが先。
//   ・CPU 側で唯一エンティティ数に比例して伸びるのは buildList（0.04ms → 5.25ms）と
//     listSort（→2.16ms）。影を削って GPU 律速でなくなった「後」に効いてくる順序。
// ------------------------------------------------------------------------------
import net from "node:net";

const PERFTEST_PROJECT = "C:/Users/ryuto/Documents/game/PerfTest";
const DEFAULT_SCENES = ["stress_500", "stress_2000", "stress_5000", "stress_10000",
                        "stress_20000", "stress_50000", "stress_100000"];

let id = 0, buf = "", pending = new Map(), sock = null;

function connect() {
  return new Promise((res, rej) => {
    let port = 8787;
    const tryPort = () => {
      const s = net.createConnection({ host: "127.0.0.1", port }, () => { sock = s; res(s); });
      s.on("error", () => { if (++port > 8797) rej(new Error("MCP ブリッジが見つからない（エンジンは起動している？）")); else tryPort(); });
      s.on("data", (d) => {
        buf += d.toString("utf8");
        let i;
        while ((i = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, i); buf = buf.slice(i + 1);
          if (!line.trim()) continue;
          const msg = JSON.parse(line);
          const p = pending.get(msg.id);
          if (p) { pending.delete(msg.id); p(msg); }
        }
      });
    };
    tryPort();
  });
}

function call(method, params = {}, ms = 600000) {
  const my = ++id;
  return new Promise((res, rej) => {
    const t = setTimeout(() => { pending.delete(my); rej(new Error("timeout " + method)); }, ms);
    pending.set(my, (m) => { clearTimeout(t); res(m); });
    sock.write(JSON.stringify({ id: my, method, params }) + "\n");
  });
}

async function timed(fn) {
  const t0 = performance.now();
  try { return { ms: Math.round(performance.now() - t0), r: await fn() }; }
  catch (e) { return { ms: Math.round(performance.now() - t0), err: e }; }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await connect();
const ping = await call("ping");
if (!String(ping.result?.assetsDir ?? "").includes("PerfTest")) {
  console.log("PerfTest プロジェクトを開き直す…");
  await call("open_project", { path: PERFTEST_PROJECT });
  await sleep(3000);
}

const scenes = process.argv.slice(2).length ? process.argv.slice(2) : DEFAULT_SCENES;
const rows = [];

for (const name of scenes) {
  const load = await timed(() => call("open_scene", { path: `scenes/${name}.json` }));
  if (load.err || !load.r?.ok) {
    console.log(`${name}: 読み込み失敗 — ${load.err?.message ?? JSON.stringify(load.r?.error)}`);
    break;
  }
  await sleep(1500);   // 落ち着かせてから測る（ロード直後の 1 フレーム目は当てにならない）

  const bench = await timed(() => call("benchmark", { frames: 240 }));
  const diag  = await timed(() => call("diagnose", { only: "entity_refs,scene_assets,picking,instancing" }));
  const save  = await timed(() => call("save_scene", { path: "scenes/__bench_tmp.json" }));

  const b = bench.r?.result ?? {};
  rows.push({
    name,
    ents:   load.r.result?.entityCount ?? 0,
    loadMs: load.ms, diagMs: diag.ms, saveMs: save.ms,
    fps:    b.fps?.avg ?? b.fps,
    frame:  b.frameMs?.avg,
    gpu:    b.gpuPassMs?.total,
    shadow: b.gpuPassMs?.shadows,
    main:   b.gpuPassMs?.mainScene,
    draws:  b.drawCalls?.avg ?? b.drawCalls,
    trisM:  Math.round((b.triangles ?? 0) / 1e5) / 10,
    cpu:    b.cpuScopeMs,
    fence:  b.cpu?.fenceWaitMs,
    verdict: b.analysis?.verdict,
  });
  const r = rows.at(-1);
  console.log(`${name.padEnd(16)} ents=${String(r.ents).padStart(6)} `
    + `fps=${String(r.fps).padStart(5)} frame=${String(r.frame).padStart(6)} `
    + `gpu=${String(r.gpu).padStart(6)} shadow=${String(r.shadow).padStart(6)} `
    + `draws=${String(r.draws).padStart(4)} ${r.verdict ?? ""}`);
}

console.log("\n=== CPU 内訳（エンティティ数に比例して伸びる箇所を見る）===");
for (const r of rows) {
  const c = r.cpu ?? {};
  console.log(`${r.name.padEnd(16)} buildList=${c.buildList} listSort=${c.listSort} `
    + `mainRec=${c.mainRec} shadowRec=${c.shadowRec} editorUi=${c.editorUi} fenceWait=${r.fence}`);
}
console.log("\n※ fenceWait > 0 は CPU が GPU を待っている＝GPU 律速。この間は CPU を速くしても無意味。");

// 保存テストで作った一時シーンは残さない
await call("delete_asset", { path: "scenes/__bench_tmp.json" }).catch(() => {});
sock.end();
