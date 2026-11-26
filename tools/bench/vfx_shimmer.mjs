// VFX の時間方向の検査: 被覆（シークのコンタクトシートで炎などが画面に出ているか）と TEMPORAL_SHIMMER（画素のちらつき）。
// W1-NB5。NB8（焚き火・松明のプリセット）と MCP-03b の shot_review が同じ関数を使う。エンジンは起動しない（PNG の連番を読むだけ）。
//
//   node tools/bench/vfx_shimmer.mjs <frames_dir | a.png b.png ...> [--background bg.png] [--limits file.json] [--json out.json]
//
// 入力: 同じカメラ・同じ露出で、一定の dt（seek）で撮った PNG の連番（名前順 = 時間順）。
// 判定の数値は tools/bench/vfx_shimmer.limits.json に置く（このファイルには数値を書かない）。
//   被覆       : 背景（--background、無ければ画素ごとの時間の中央値）との輝度差が coverageDiffLuma を超えた画素の割合 [%]。
//                シーケンスの最大が minCoveragePct を超えること（超えなければ LOW_COVERAGE）
//   ちらつき   : 各画素の輝度から 3x3 の空間平均を引いた高域（広い範囲がいっしょに明滅するのは炎の揺れ。1 画素だけの明滅が TEMPORAL_SHIMMER）
//                の時系列で、符号が反転し、かつ両側の変化がどちらも ampLuma 以上の点の割合（対象 = 被覆に入った画素）が
//                shimmerRatioMax を超えたら TEMPORAL_SHIMMER
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(here, "../..");
const DEFAULT_LIMITS = path.join(here, "vfx_shimmer.limits.json");

export function loadLimits(file = DEFAULT_LIMITS) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

/** { width, height, data: RGBA } → 輝度 (0..255, Float32Array)。 */
export function lumaOf(img) {
  const n = img.width * img.height;
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const p = i * 4;
    out[i] = 0.2126 * img.data[p] + 0.7152 * img.data[p + 1] + 0.0722 * img.data[p + 2];
  }
  return out;
}

/** 画素ごとの時間の中央値（背景の推定）。 */
export function medianBackground(lumas) {
  const n = lumas[0].length, k = lumas.length;
  const bg = new Float32Array(n);
  const tmp = new Float32Array(k);
  for (let i = 0; i < n; i++) {
    for (let f = 0; f < k; f++) tmp[f] = lumas[f][i];
    tmp.sort();
    bg[i] = k % 2 ? tmp[(k - 1) / 2] : 0.5 * (tmp[k / 2 - 1] + tmp[k / 2]);
  }
  return bg;
}

/** 3x3 の箱平均（縁は端の値で延長）。 */
export function box3(src, w, h) {
  const out = new Float32Array(w * h);
  for (let y = 0; y < h; y++) {
    const y0 = Math.max(y - 1, 0), y1 = Math.min(y + 1, h - 1);
    for (let x = 0; x < w; x++) {
      const x0 = Math.max(x - 1, 0), x1 = Math.min(x + 1, w - 1);
      let s = 0;
      for (let yy = y0; yy <= y1; yy++) for (let xx = x0; xx <= x1; xx++) s += src[yy * w + xx];
      out[y * w + x] = s / ((y1 - y0 + 1) * (x1 - x0 + 1));
    }
  }
  return out;
}

/**
 * @param {{width:number,height:number,data:Uint8Array|Buffer}[]} frames  同じ大きさの RGBA の連番
 * @param {{background?: {width:number,height:number,data:any}, limits?: object}} opts
 */
export function analyze(frames, opts = {}) {
  const lim = opts.limits ?? loadLimits();
  if (frames.length < 3) throw new Error("フレームが 3 枚以上要る（ちらつきは時間の 2 階差で見る）");
  const { width: w, height: h } = frames[0];
  for (const f of frames) if (f.width !== w || f.height !== h) throw new Error("フレームの大きさが揃っていない");
  const total = w * h, nf = frames.length;
  const lumas = frames.map(lumaOf);
  const bg = opts.background ? lumaOf(opts.background) : medianBackground(lumas);

  // 被覆
  const mask = new Uint8Array(total); // 画素が被覆に入ったフレーム数
  const perFrame = [];
  for (let f = 0; f < nf; f++) {
    let c = 0;
    const L = lumas[f];
    for (let i = 0; i < total; i++) {
      if (Math.abs(L[i] - bg[i]) > lim.coverageDiffLuma) { c++; if (mask[i] < 255) mask[i]++; }
    }
    perFrame.push((c / total) * 100);
  }
  const covMax = Math.max(...perFrame);
  const covMean = perFrame.reduce((a, b) => a + b, 0) / nf;

  // ちらつき（空間の高域 × 時間の符号反転）
  const hp = lumas.map((L) => {
    const lo = box3(L, w, h);
    const o = new Float32Array(total);
    for (let i = 0; i < total; i++) o[i] = L[i] - lo[i];
    return o;
  });
  let roi = 0, samples = 0, zig = 0;
  for (let i = 0; i < total; i++) {
    if (mask[i] < lim.roiMinFrames) continue;
    roi++;
    for (let f = 1; f < nf - 1; f++) {
      const a = hp[f][i] - hp[f - 1][i], b = hp[f + 1][i] - hp[f][i];
      samples++;
      if (a * b < 0 && Math.abs(a) >= lim.ampLuma && Math.abs(b) >= lim.ampLuma) zig++;
    }
  }
  const ratio = samples ? zig / samples : 0;
  const issues = [];
  if (covMax <= lim.minCoveragePct) issues.push({ code: "LOW_COVERAGE", severity: "blocking", detail: `被覆の最大 ${covMax.toFixed(3)}% <= ${lim.minCoveragePct}%` });
  if (ratio > lim.shimmerRatioMax) issues.push({ code: "TEMPORAL_SHIMMER", severity: "blocking", detail: `ちらつきの割合 ${ratio.toFixed(4)} > ${lim.shimmerRatioMax}` });
  return {
    frames: nf, width: w, height: h,
    coverage: { maxPct: covMax, meanPct: covMean, peakFrame: perFrame.indexOf(covMax), perFramePct: perFrame },
    shimmer: { roiPixels: roi, ratio, limit: lim.shimmerRatioMax },
    issues, ok: issues.length === 0,
  };
}

function loadPng(file) {
  const require = createRequire(import.meta.url);
  const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));
  const p = PNG.sync.read(fs.readFileSync(file));
  return { width: p.width, height: p.height, data: p.data };
}

function listFrames(inputs) {
  if (inputs.length === 1 && fs.statSync(inputs[0]).isDirectory()) {
    return fs.readdirSync(inputs[0]).filter((n) => /\.png$/i.test(n)).sort().map((n) => path.join(inputs[0], n));
  }
  return inputs;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const pos = [];
  let bgFile = null, limFile = DEFAULT_LIMITS, jsonOut = null;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--background") bgFile = args[++i];
    else if (args[i] === "--limits") limFile = args[++i];
    else if (args[i] === "--json") jsonOut = args[++i];
    else pos.push(args[i]);
  }
  if (!pos.length) { console.error("使い方: node tools/bench/vfx_shimmer.mjs <frames_dir | a.png b.png ...> [--background bg.png] [--json out.json]"); process.exit(2); }
  const files = listFrames(pos);
  const r = analyze(files.map(loadPng), { background: bgFile ? loadPng(bgFile) : undefined, limits: loadLimits(limFile) });
  console.log(`被覆 最大 ${r.coverage.maxPct.toFixed(3)}%（フレーム ${r.coverage.peakFrame}）/ 平均 ${r.coverage.meanPct.toFixed(3)}%  ちらつき ${r.shimmer.ratio.toFixed(4)}（上限 ${r.shimmer.limit}、対象 ${r.shimmer.roiPixels}px）`);
  for (const i of r.issues) console.log(`  ${i.code}: ${i.detail}`);
  console.log(r.ok ? "合格" : "不合格");
  if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(r, null, 1));
  process.exit(r.ok ? 0 : 1);
}
