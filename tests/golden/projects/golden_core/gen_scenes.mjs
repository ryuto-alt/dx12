// golden_core のシーン JSON を生成する（手で JSON を書き換えず、ここを直して再生成する）。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), "assets", "scenes");
const tf = (p, r = [0, 0, 0], s = [1, 1, 1]) => ({ position: p, rotation: r, scale: s });
const sun = (dir = [-0.4, -1.0, -0.35], intensity = 3.0) => ({
  name: "Sun", directionalLight: { direction: dir, color: [1.0, 0.97, 0.9], intensity, ambient: 0.0 },
  transform: tf([0, 12, 0], [55, -30, 0]),
});
const ground = (size = 40, color = [0.55, 0.55, 0.58]) => ({
  name: "Ground", primitive: "plane", primitiveSize: size, color,
  material: { metallic: 0.0, roughness: 0.85 }, transform: tf([0, 0, 0]),
});
const sphere = (name, pos, r, color, metallic, roughness, extra = {}) => ({
  name, primitive: "sphere", primitiveSize: r, color, material: { metallic, roughness, ...extra }, transform: tf(pos),
});
const box = (name, pos, scale, color, metallic = 0.0, roughness = 0.6, extra = {}) => ({
  name, primitive: "box", color, material: { metallic, roughness, ...extra }, transform: tf(pos, [0, 0, 0], scale),
});
const write = (name, scene) => fs.writeFileSync(path.join(OUT, `${name}.json`), JSON.stringify(scene, null, 2) + "\n");

// 1) PBR 球の列: 金属 0/1 x 粗さ 0.05..1.0（手続きの空の IBL を映す）
{
  const es = [sun(), ground(30, [0.4, 0.4, 0.42])];
  const rough = [0.05, 0.25, 0.5, 0.75, 1.0];
  for (let row = 0; row < 2; row++)
    rough.forEach((r, i) => es.push(sphere(`S_m${row}_r${i}`, [(i - 2) * 1.3, 0.6, row * 2.2], 0.55,
      row ? [0.95, 0.78, 0.45] : [0.8, 0.15, 0.12], row, r)));
  write("pbr_spheres", { skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true });
}
// 2) CSM: 手前から奥へ柱が並ぶ長い地面。低い太陽で影を長く伸ばす
{
  const es = [sun([-0.7, -0.35, -0.5], 4.0), ground(160, [0.5, 0.5, 0.5])];
  for (let i = 0; i < 14; i++) {
    const z = 3 + i * i * 0.45 + i * 2;
    es.push(box(`Pillar_${i}`, [(i % 2 ? 3.2 : -3.2), 1.5, z], [0.6, 3.0, 0.6], [0.75, 0.72, 0.68]));
  }
  write("csm_shadows", { skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true });
}
// 3) 半透明: 不透明の箱の手前に blend の球と板を重ねる
{
  const es = [sun(), ground(30, [0.45, 0.45, 0.48]),
    box("Back_R", [-1.6, 0.9, 1.5], [1, 1.8, 1], [0.85, 0.2, 0.15]),
    box("Back_G", [0.0, 0.9, 2.5], [1, 1.8, 1], [0.2, 0.7, 0.25]),
    box("Back_B", [1.6, 0.9, 1.5], [1, 1.8, 1], [0.2, 0.3, 0.85])];
  [0.2, 0.45, 0.7].forEach((o, i) => es.push(sphere(`Glass_${i}`, [-1.6 + i * 1.6, 0.8, -0.5], 0.7, [0.9, 0.95, 1.0], 0.0, 0.1,
    { alphaMode: "blend", opacity: o })));
  es.push(box("Pane", [0, 1.0, -1.8], [4, 2, 0.05], [0.4, 0.8, 0.9], 0.0, 0.05, { alphaMode: "blend", opacity: 0.35 }));
  write("transparency", { entities: es, shadows: true });
}
// 4) ポスト: 発光する物と暗い物を置いて bloom / vignette / 彩度 / FXAA / SSAO を効かせる（grain は入れていない）
{
  const es = [sun([-0.3, -0.6, -0.5], 5.0), ground(30, [0.3, 0.3, 0.32]),
    sphere("Hot", [-1.2, 0.7, 0], 0.7, [1.0, 0.95, 0.9], 0.0, 0.2, { emissiveColor: [1.0, 0.66, 0.33], emissiveIntensity: 6.0 }),
    sphere("Chrome", [1.2, 0.7, 0], 0.7, [0.9, 0.9, 0.95], 1.0, 0.1),
    box("Wall", [0, 1.5, 3.5], [8, 3, 0.3], [0.6, 0.6, 0.65], 0.0, 0.7)];
  write("post_stack", {
    postProcess: { enabled: true, tonemapper: 1, bloomOn: true, bloom: 0.6, bloomThreshold: 1.0, vignetteOn: true, vignette: 0.4,
      saturationOn: true, saturation: 1.1, fxaaOn: true },
    ssao: { enabled: true }, entities: es, shadows: true });
}
// 5) ロード時の既定値: 設定ブロックを何も書かない手書きシーン（新規シーンコマンドの結果ではない。そちらは golden.json の core_new_scene_gi）
{
  write("load_defaults", { entities: [sun(), ground(30), box("Cube", [0, 0.5, 0], [1, 1, 1], [0.6, 0.65, 0.75]),
    sphere("Ball", [1.8, 0.5, 0.5], 0.5, [0.8, 0.4, 0.3], 0.0, 0.5)] });
}
console.log("シーンを生成した:", OUT);
