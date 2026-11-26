// lookdev_grazing 用の手続きテクスチャ（法線マップ）と .dxmat を生成する。外部アセットは使わない（自作・ライセンス問題なし）。
// 実行: node tests/golden/projects/golden_core/gen_lookdev_textures.mjs
// 256x256 の継ぎ目のない法線マップ（小さな丸い凹凸の格子 + 細かいうねり）。シーン側で uvTiling を掛けて高タイリングにする。
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const { PNG } = require(path.join(HERE, "..", "..", "..", "..", "tools", "mcp-server", "node_modules", "pngjs"));

const N = 256, CELLS = 8, TWO_PI = Math.PI * 2;
// 高さ: セルの中心が盛り上がった丸い凹凸 + 周期の整数倍のうねり（どちらも 256 で一周するので継ぎ目が出ない）
const height = (x, y) => {
  const cu = ((x * CELLS / N) % 1) - 0.5, cv = ((y * CELLS / N) % 1) - 0.5;
  const bump = Math.max(0, 1 - Math.hypot(cu, cv) * 2.1);
  const wave = 0.12 * Math.sin(TWO_PI * 11 * x / N) * Math.sin(TWO_PI * 9 * y / N);
  return bump * bump * 0.9 + wave;
};
const png = new PNG({ width: N, height: N });
const STRENGTH = 5.0;
for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
  const hx = height((x + 1) % N, y) - height((x + N - 1) % N, y);
  const hy = height(x, (y + 1) % N) - height(x, (y + N - 1) % N);
  let nx = -hx * STRENGTH, ny = -hy * STRENGTH, nz = 1;
  const l = Math.hypot(nx, ny, nz); nx /= l; ny /= l; nz /= l;
  const o = (y * N + x) * 4;
  png.data[o] = Math.round((nx * 0.5 + 0.5) * 255); png.data[o + 1] = Math.round((ny * 0.5 + 0.5) * 255);
  png.data[o + 2] = Math.round((nz * 0.5 + 0.5) * 255); png.data[o + 3] = 255;
}
const texDir = path.join(HERE, "assets", "textures", "lookdev"), matDir = path.join(HERE, "assets", "materials");
fs.mkdirSync(texDir, { recursive: true }); fs.mkdirSync(matDir, { recursive: true });
fs.writeFileSync(path.join(texDir, "grazing_n.png"), PNG.sync.write(png));
fs.writeFileSync(path.join(matDir, "lookdev_grazing.dxmat"), JSON.stringify({
  name: "lookdev_grazing", normal: "textures/lookdev/grazing_n.png", metallic: 0.0, roughness: 0.2, uvTiling: [24, 24],
  source: "tests/golden/projects/golden_core/gen_lookdev_textures.mjs（手続き生成）", license: "CC0-1.0",
}, null, 2) + "\n");
console.log("lookdev のテクスチャと .dxmat を生成した:", texDir);
