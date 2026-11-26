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
// 2b) FU-E-23: RT 影 ON・TAA/SSR/SSGI OFF（RT 影デノイザだけが速度プリパスを要求する条件）で、
//     40m を超える大きな箱の地面・高い塔の上部がラスタから欠けないこと。細い遠い柱の影も入れる。
{
  const es = [sun([-0.55, -0.6, -0.4], 4.0),
    box("BigGround", [0, -0.5, 60], [160, 1, 200], [0.5, 0.5, 0.5], 0.0, 0.85),
    box("Tower", [-6, 12, 30], [3, 24, 3], [0.75, 0.72, 0.68])];
  for (const z of [15, 30, 60]) es.push(box(`Thin_${z}`, [4, 1.5, z], [0.35, 3.0, 0.35], [0.8, 0.78, 0.74]));
  write("rt_shadow_raster", {
    skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true,
    taa: { enabled: false }, ssr: { enabled: false }, ssgi: { enabled: false },
    raytracing: { shadowEnabled: true, shadowDenoise: true },
  });
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
// ─── MH-1 ルックデブ（W1-C4）。tools/lookdev_check.ps1 が V1 / V2 の両方で撮る ─────────────────────────────
// 6) lookdev_grid: 粗さ x 金属度の 7x7 球 + 18% グレーカード + 24 パッチ（ColorChecker 近似）。
//    エンジンは entity の color を線形の乗数として読む（Forward.hlsl: albedo = tex * color）ので、sRGB の値は線形へ直して書く。
const srgbToLinear = (v) => { const c = v / 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
{
  const es = [sun([-0.35, -0.8, 0.3], 3.0), ground(40, [0.22, 0.22, 0.23])];
  const R = [0.05, 0.2, 0.35, 0.5, 0.65, 0.8, 1.0];   // 列 = 粗さ
  const M = [0.0, 1 / 6, 2 / 6, 3 / 6, 4 / 6, 5 / 6, 1.0];   // 行 = 金属度（手前が誘電体）
  M.forEach((m, row) => R.forEach((r, col) =>
    es.push(sphere(`G_m${row}_r${col}`, [(col - 3) * 1.05, 0.45, row * 1.05], 0.45, [0.82, 0.82, 0.82], m, r))));
  es.push(box("GrayCard18", [-5.9, 0.6, 1.5], [1.1, 1.2, 0.04], [0.18, 0.18, 0.18], 0.0, 0.9));
  const cc = [[115,82,68],[194,150,130],[98,122,157],[87,108,67],[133,128,177],[103,189,170],
    [214,126,44],[80,91,166],[193,90,99],[94,60,108],[157,188,64],[224,163,46],
    [56,61,150],[70,148,73],[175,54,60],[231,199,31],[187,86,149],[8,133,161],
    [243,243,242],[200,200,200],[160,160,160],[122,122,121],[85,85,85],[52,52,52]];
  cc.forEach((c, i) => es.push(box(`Patch_${i}`, [(i % 6 - 2.5) * 0.62 + 6.4, 2.55 - Math.floor(i / 6) * 0.62, 3.2], [0.56, 0.56, 0.03],
    c.map((v) => +srgbToLinear(v).toFixed(5)), 0.0, 0.85)));
  write("lookdev_grid", { skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true });
}
// 7) lookdev_grazing: 高タイリングの法線マップの床（textures/lookdev/grazing_n.png、gen_lookdev_textures.mjs が生成）を
//    かすめる角度で撮る。スペキュラ AA と水平線の遮蔽の確認用。向かい風の低い太陽 + 金属の柱の列
{
  const es = [sun([0.18, -0.14, -1.0], 5.0)];
  es.push({ name: "Floor", primitive: "plane", primitiveSize: 80, color: [0.6, 0.6, 0.62],
    material: { metallic: 0.0, roughness: 0.2 }, materialAssets: ["materials/lookdev_grazing.dxmat"], transform: tf([0, 0, 0]) });
  for (let i = 0; i < 8; i++) {
    es.push(box(`Post_${i}`, [(i % 2 ? 1.6 : -1.6), 0.6, 3 + i * 3.2], [0.18, 1.2, 0.18], [0.9, 0.9, 0.92], 1.0, 0.12 + 0.1 * (i % 4)));
  }
  es.push(sphere("Chrome", [0, 0.5, 2.2], 0.5, [0.95, 0.95, 0.97], 1.0, 0.05));
  write("lookdev_grazing", { skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true });
}
// 8) lookdev_glass: 白黒の縞を背に、ガラス（blend + IOR + 鏡面）と色つきの透過とクリアコートの球を並べる
{
  const es = [sun([-0.5, -0.8, 0.25], 3.5), ground(30, [0.35, 0.35, 0.37])];
  for (let i = 0; i < 12; i++)
    es.push(box(`Stripe_${i}`, [(i - 5.5) * 0.9, 1.1, 3.2], [0.45, 2.2, 0.08], i % 2 ? [0.92, 0.92, 0.92] : [0.04, 0.04, 0.04], 0.0, 0.7));
  [[0.15, 1.45, 0.02], [0.35, 1.5, 0.12], [0.6, 1.5, 0.3], [0.85, 1.52, 0.5]].forEach(([op, ior, r], i) =>
    es.push(sphere(`Glass_${i}`, [-2.7 + i * 1.8, 0.8, 1.2], 0.75, [0.92, 0.97, 1.0], 0.0, r,
      { alphaMode: "blend", opacity: op, ior, specularFactor: 1.0 })));
  [[0.9, 0.2, 0.15], [0.2, 0.75, 0.3], [0.2, 0.35, 0.9]].forEach((c, i) =>
    es.push(sphere(`Tint_${i}`, [-1.8 + i * 1.8, 0.5, -0.9], 0.5, c, 0.0, 0.08, { alphaMode: "blend", opacity: 0.45, ior: 1.5 })));
  es.push(sphere("Coat", [4.2, 0.55, -0.9], 0.55, [0.75, 0.08, 0.06], 0.0, 0.55, { clearCoat: 1.0, clearCoatRoughness: 0.03 }));
  write("lookdev_glass", { skybox: { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true }, entities: es, shadows: true });
}
// 9) lookdev_emissive_room: 太陽の無い閉じた部屋（床・天井・四方の壁。カメラは部屋の中）。天井の発光パネルと色つきの発光球、点光源 3 灯。IBL は弱くして室内光だけを見る
{
  const W = 8, H = 4, D = 8, t = 0.2, wall = [0.78, 0.78, 0.76];
  const es = [
    box("Floor", [0, -t / 2, D / 2], [W, t, D], [0.6, 0.6, 0.58], 0.0, 0.35),
    box("Ceiling", [0, H + t / 2, D / 2], [W, t, D], wall, 0.0, 0.8),
    box("Back", [0, H / 2, D + t / 2], [W, H, t], wall, 0.0, 0.8),
    box("Front", [0, H / 2, -t / 2], [W, H, t], wall, 0.0, 0.8),
    box("Left", [-W / 2 - t / 2, H / 2, D / 2], [t, H, D], [0.75, 0.12, 0.1], 0.0, 0.8),
    box("Right", [W / 2 + t / 2, H / 2, D / 2], [t, H, D], [0.12, 0.6, 0.18], 0.0, 0.8),
    box("PanelStrip", [0, H - 0.05, D / 2], [2.4, 0.06, 5.0], [1.0, 0.95, 0.85], 0.0, 0.5, { emissiveColor: [1.0, 0.92, 0.78], emissiveIntensity: 8.0 }),
    sphere("OrbR", [-2.2, 0.55, 3.0], 0.5, [1, 1, 1], 0.0, 0.4, { emissiveColor: [1.0, 0.25, 0.1], emissiveIntensity: 5.0 }),
    sphere("OrbB", [2.2, 0.55, 3.0], 0.5, [1, 1, 1], 0.0, 0.4, { emissiveColor: [0.15, 0.4, 1.0], emissiveIntensity: 5.0 }),
    sphere("Chrome", [0, 0.6, 5.0], 0.6, [0.95, 0.95, 0.97], 1.0, 0.08),
    sphere("Matte", [-1.0, 0.45, 1.6], 0.45, [0.8, 0.8, 0.8], 0.0, 0.9),
    sphere("Satin", [1.0, 0.45, 1.6], 0.45, [0.85, 0.65, 0.2], 1.0, 0.35),
  ];
  const pl = (name, pos, color, intensity, range) => ({ name, pointLight: { color, intensity, range }, transform: tf(pos) });
  es.push(pl("Lamp_Top", [0, H - 0.4, D / 2], [1.0, 0.92, 0.78], 18.0, 9.0));
  es.push(pl("Lamp_R", [-2.2, 1.2, 3.0], [1.0, 0.3, 0.12], 6.0, 5.0));
  es.push(pl("Lamp_B", [2.2, 1.2, 3.0], [0.2, 0.4, 1.0], 6.0, 5.0));
  write("lookdev_emissive_room", { skybox: { iblIntensity: 0.12, skyboxIntensity: 0.0, drawSkybox: false }, entities: es, shadows: true });
}
console.log("シーンを生成した:", OUT);
