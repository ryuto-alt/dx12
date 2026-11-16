// ゴールデン画像の比較に使う純関数（pngjs にも エンジンにも依存しない。tools/bench/golden.test.mjs で検証）。
import crypto from "node:crypto";

export function sha256(img) {
  return crypto.createHash("sha256").update(img.data).digest("hex");
}

/** 面積平均で dw x dh へ縮める（拡大にも使えるが用途は縮小）。アルファは 255 固定。 */
export function resampleArea(img, dw, dh) {
  const { width: sw, height: sh, data: s } = img;
  if (sw === dw && sh === dh) return { width: dw, height: dh, data: Buffer.from(s) };
  const out = Buffer.alloc(dw * dh * 4);
  const fx = sw / dw, fy = sh / dh;
  for (let dy = 0; dy < dh; dy++) {
    const y0 = dy * fy, y1 = (dy + 1) * fy;
    const iy0 = Math.floor(y0), iy1 = Math.min(sh, Math.ceil(y1));
    for (let dx = 0; dx < dw; dx++) {
      const x0 = dx * fx, x1 = (dx + 1) * fx;
      const ix0 = Math.floor(x0), ix1 = Math.min(sw, Math.ceil(x1));
      let r = 0, g = 0, b = 0, wsum = 0;
      for (let y = iy0; y < iy1; y++) {
        const wy = Math.min(y + 1, y1) - Math.max(y, y0);
        if (wy <= 0) continue;
        for (let x = ix0; x < ix1; x++) {
          const wx = Math.min(x + 1, x1) - Math.max(x, x0);
          if (wx <= 0) continue;
          const w = wx * wy, i = (y * sw + x) * 4;
          r += s[i] * w; g += s[i + 1] * w; b += s[i + 2] * w; wsum += w;
        }
      }
      const o = (dy * dw + dx) * 4;
      out[o] = Math.round(r / wsum); out[o + 1] = Math.round(g / wsum);
      out[o + 2] = Math.round(b / wsum); out[o + 3] = 255;
    }
  }
  return { width: dw, height: dh, data: out };
}

/**
 * 2 枚を画素ごとに比べる。
 *   diffPixels … RGB ユークリッド距離が pixelThreshold を超えた画素数
 *   meanAbs    … RGB 全チャンネルの平均絶対差（0..255）
 *   maxDist    … 画素の RGB 距離の最大
 * 差分画像: 一致画素は基準を暗く、違う画素は赤（距離が大きいほど明るい）。
 */
export function compareImages(a, b, pixelThreshold) {
  if (a.width !== b.width || a.height !== b.height)
    throw new Error(`大きさが違う: ${a.width}x${a.height} と ${b.width}x${b.height}`);
  const n = a.width * a.height;
  const diff = Buffer.alloc(n * 4);
  let diffPixels = 0, absSum = 0, maxDist = 0;
  for (let p = 0; p < n; p++) {
    const i = p * 4;
    const dr = a.data[i] - b.data[i], dg = a.data[i + 1] - b.data[i + 1], db = a.data[i + 2] - b.data[i + 2];
    absSum += Math.abs(dr) + Math.abs(dg) + Math.abs(db);
    const d = Math.sqrt(dr * dr + dg * dg + db * db);
    if (d > maxDist) maxDist = d;
    if (d > pixelThreshold) {
      diffPixels++;
      diff[i] = Math.min(255, 96 + Math.round(d)); diff[i + 1] = 0; diff[i + 2] = 0;
    } else {
      const l = Math.round((a.data[i] * 0.2126 + a.data[i + 1] * 0.7152 + a.data[i + 2] * 0.0722) * 0.35);
      diff[i] = l; diff[i + 1] = l; diff[i + 2] = l;
    }
    diff[i + 3] = 255;
  }
  return {
    diffPixels, diffRatio: diffPixels / n, meanAbs: absSum / (n * 3), maxDist,
    diffImage: { width: a.width, height: a.height, data: diff },
  };
}

export function judge(cmp, tol) {
  const reasons = [];
  if (cmp.diffRatio > tol.maxDiffRatio)
    reasons.push(`違う画素 ${(cmp.diffRatio * 100).toFixed(3)}% > 上限 ${(tol.maxDiffRatio * 100).toFixed(3)}%`);
  if (cmp.meanAbs > tol.maxMeanAbs)
    reasons.push(`平均絶対差 ${cmp.meanAbs.toFixed(3)} > 上限 ${tol.maxMeanAbs}`);
  return { pass: reasons.length === 0, reasons };
}
