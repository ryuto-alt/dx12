// post_image のシーン JSON を生成する（手で JSON を書き換えず、ここを直して再生成する）。
//   node tests/golden/projects/post_image/gen_scenes.mjs
// 4 シーン（taa_torture / dof_chart / mb_wheel / hdr_range）。どれも既定のポストを基本にし、検証したい効果だけを足す。
// 動きは入れない（静止画のゴールデン）。動かす対象の名前（FenceHub / GhostOrb / Wheel）は tools/bench/temporal_metrics.scenarios.json と対。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), "assets", "scenes");
fs.mkdirSync(OUT, { recursive: true });
const tf = (p, r = [0, 0, 0], s = [1, 1, 1]) => ({ position: p, rotation: r, scale: s });
const sun = (dir = [-0.4, -1.0, -0.35], intensity = 3.0, color = [1.0, 0.97, 0.9]) => ({
  name: "Sun", directionalLight: { direction: dir, color, intensity, ambient: 0.0 },
  transform: tf([0, 12, 0], [55, -30, 0]),
});
const ground = (size = 40, color = [0.4, 0.4, 0.42]) => ({
  name: "Ground", primitive: "plane", primitiveSize: size, color,
  material: { metallic: 0.0, roughness: 0.85 }, transform: tf([0, 0, 0]),
});
const sphere = (name, pos, r, color, metallic, roughness, extra = {}) => ({
  name, primitive: "sphere", primitiveSize: r, color, material: { metallic, roughness, ...extra }, transform: tf(pos),
});
const box = (name, pos, scale, color, metallic = 0.0, roughness = 0.6, extra = {}, rot = [0, 0, 0]) => ({
  name, primitive: "box", color, material: { metallic, roughness, ...extra }, transform: tf(pos, rot, scale),
});
const emissive = (name, pos, r, rgb, nit) => sphere(name, pos, r, [0.02, 0.02, 0.02], 0.0, 0.5, { emissiveColor: rgb, emissiveIntensity: nit });
const write = (name, scene) => fs.writeFileSync(path.join(OUT, `${name}.json`), JSON.stringify(scene, null, 2) + "\n");
const sky = { iblIntensity: 1.0, skyboxIntensity: 1.0, drawSkybox: true };

// 1) taa_torture: TAA の残光・ちらつき・遮蔽解除の試験台
//    - Stripes: 幅 0.03 の縦縞（サブピクセルまで細くなる。TAA 無しなら画素ごとに明滅する）。静止領域（ちらつきの測定）
//    - FenceHub: 細い棒 12 本の円筒の柵。Y 軸まわりに回る（背景の縞の前を横切る → 遮蔽解除）
//    - Sparks: 高速で細長い粒（stretch）の火花。Ember: 発光する小球
//    - GhostOrb: 明るい発光球。temporal_metrics が途中で消して、残光（ゴースト）のエネルギーを測る
{
  const es = [sun([-0.35, -0.7, 0.4], 4.0), ground(40, [0.35, 0.35, 0.37])];
  for (let i = 0; i < 40; i++)
    es.push(box(`Stripe_${i}`, [-3.9 + i * 0.2, 1.0, 4.5], [0.03, 2.0, 0.03], i % 2 ? [0.9, 0.9, 0.9] : [0.05, 0.05, 0.06], 0.0, 0.7));
  const hubIdx = es.length;
  es.push({ name: "FenceHub", transform: tf([-1.4, 0.0, 1.0]) });
  for (let i = 0; i < 12; i++) {
    const a = (i / 12) * Math.PI * 2;
    es.push({ ...box(`Bar_${i}`, [Math.cos(a) * 0.9, 1.0, Math.sin(a) * 0.9], [0.025, 2.0, 0.025], [0.85, 0.8, 0.7], 0.0, 0.5), parent: hubIdx });
  }
  es.push({
    name: "Sparks", transform: tf([1.8, 0.3, 1.0]),
    particleEmitter: { kind: 3, blend: 0, rate: 160, dir: [0.0, 1.0, 0.0], spread: 0.55, speed: 9.0, speedVar: 0.5, size: 0.05,
      sizeEnd: 0.01, life: 0.7, lifeVar: 0.3, color: [1.0, 0.75, 0.35], colorEnd: [1.0, 0.2, 0.05], intensity: 10.0,
      gravity: -9.0, drag: 0.4, stretch: 0.12, looping: true, playOnStart: true, gpu: false },
  });
  es.push(emissive("Ember", [0.4, 1.6, 1.0], 0.05, [1.0, 0.6, 0.25], 30.0));
  es.push(emissive("GhostOrb", [0.0, 1.2, 2.0], 0.25, [1.0, 0.9, 0.7], 12.0));
  write("taa_torture", { skybox: sky, entities: es, shadows: true,
    taa: { enabled: true, sampleCount: 8, feedbackMin: 0.88, feedbackMax: 0.97, varianceGamma: 1.0, jitterScale: 1.0 } });
}

// 2) dof_chart: 合焦面のテストチャートと、遠くの点光源の格子（ボケの形とエネルギー）
//    カメラ (0,1.5,-3) → 注視 (0,1.5,3)。チャートは z=3（距離 6）。点光源の格子は z=12（距離 15）。dofFocusDist = 6。
{
  const es = [sun([-0.3, -0.8, 0.3], 2.0), ground(60, [0.12, 0.12, 0.13])];
  for (let r = 0; r < 8; r++) for (let c = 0; c < 8; c++)
    es.push(box(`Chart_${r}_${c}`, [-1.4 + c * 0.4, 0.4 + r * 0.4, 3.0], [0.4, 0.4, 0.02], (r + c) % 2 ? [0.92, 0.92, 0.92] : [0.04, 0.04, 0.04], 0.0, 0.8));
  for (let r = 0; r < 5; r++) for (let c = 0; c < 9; c++)
    es.push(emissive(`Dot_${r}_${c}`, [-6.0 + c * 1.5, 0.6 + r * 1.1, 12.0], 0.05, [1.0, 0.95, 0.85], 60.0));
  write("dof_chart", { skybox: { iblIntensity: 0.3, skyboxIntensity: 0.15, drawSkybox: true }, entities: es, shadows: true,
    postProcess: { enabled: true, dofOn: true, dofFocusDist: 6.0, dofBlurSize: 12.0, dofAperture: 1.0, dofFocalLength: 85.0 } });
}

// 3) mb_wheel: 回る車輪（Z 軸まわり）とカメラのパン。背景の縞でカメラ運動のブラーも見える
{
  const es = [sun([-0.4, -0.9, 0.3], 3.5), ground(40, [0.3, 0.3, 0.32])];
  for (let i = 0; i < 30; i++)
    es.push(box(`Post_${i}`, [-6 + i * 0.5, 0.75, 5.0], [0.12, 1.5, 0.12], i % 3 ? [0.7, 0.7, 0.72] : [0.8, 0.2, 0.15], 0.0, 0.6));
  const wheelIdx = es.length;
  es.push({ name: "Wheel", transform: tf([0.0, 1.3, 2.0]) });
  for (let i = 0; i < 8; i++)
    es.push({ ...box(`Spoke_${i}`, [0, 0, 0], [0.08, 1.7, 0.08], [0.9, 0.85, 0.8], 0.6, 0.35, {}, [0, 0, (i / 8) * 180]), parent: wheelIdx });
  for (let i = 0; i < 24; i++) {
    const a = (i / 24) * Math.PI * 2;
    es.push({ ...box(`Rim_${i}`, [Math.cos(a) * 0.85, Math.sin(a) * 0.85, 0], [0.26, 0.1, 0.12], [0.12, 0.12, 0.14], 0.0, 0.5, {}, [0, 0, (a * 180) / Math.PI + 90]), parent: wheelIdx });
  }
  es.push({ ...box("Hub", [0, 0, 0], [0.2, 0.2, 0.2], [0.8, 0.15, 0.1], 0.3, 0.3), parent: wheelIdx });
  write("mb_wheel", { skybox: sky, entities: es, shadows: true,
    postProcess: { enabled: true, motionBlurOn: true, mbStrength: 1.0, mbSamples: 16 },
    // オブジェクトごとのブラーは TAA の速度バッファが要る（TAA OFF だと深度からの再構成＝カメラの動きだけ）
    taa: { enabled: true, sampleCount: 8, feedbackMin: 0.88, feedbackMax: 0.97, varianceGamma: 1.0, jitterScale: 1.0 } });
}

// 4) hdr_range: 物理ライティング単位（1.0 = 1 nit）・手動 EV100 15。1 枚に何桁も離れた明るさを並べる
//    - 空（約 6e3 nit の手続きの空）と日向（太陽 100000 lux × アルベド 0.25 ÷ π ≒ 8e3 nit）
//    - 室内（発光パネル 1 nit）と Lamp（64 nit）
//    ★仕様の「太陽円盤 1e9 nit / 発光 1e5 nit」は、このエンジンでは作れない: マテリアルの発光強度は上限 64
//      （Material.h kEmissiveIntensityMax）で、手続きの空は太陽円盤を描かない。大気シェーダ側の fp16 頭打ち（6e4）の試験は、
//      HDR の発光が入った後（W2 以降）にこのシーンへ足す。
{
  const es = [sun([0.25, -0.4, -0.88], 100000.0, [1.0, 0.96, 0.88]), ground(40, [0.25, 0.25, 0.26]),
    emissive("Lamp", [-1.2, 1.4, 4.0], 0.35, [1.0, 0.8, 0.5], 64.0)];
  // 室内: 床・壁 3 面・天井の小部屋。内側の発光パネルが 1 nit
  const rx = 3.0;
  es.push(box("Room_Wall_B", [rx, 1.0, 3.6], [0.1, 2.0, 2.2], [0.5, 0.5, 0.5], 0.0, 0.9));
  es.push(box("Room_Wall_F", [rx, 1.0, 1.4], [1.9, 2.0, 0.1], [0.5, 0.5, 0.5], 0.0, 0.9));
  es.push(box("Room_Wall_R", [rx + 1.0, 1.0, 2.5], [0.1, 2.0, 2.2], [0.5, 0.5, 0.5], 0.0, 0.9));
  es.push(box("Room_Roof", [rx + 0.5, 2.05, 2.5], [2.1, 0.1, 2.3], [0.5, 0.5, 0.5], 0.0, 0.9));
  es.push(box("Room_Panel", [rx + 0.5, 1.9, 2.5], [1.2, 0.03, 1.0], [0.02, 0.02, 0.02], 0.0, 0.5,
    { emissiveColor: [1.0, 0.9, 0.75], emissiveIntensity: 1.0 }));
  es.push(sphere("Room_Ball", [rx + 0.4, 0.4, 2.4], 0.4, [0.7, 0.3, 0.2], 0.0, 0.5));
  write("hdr_range", { skybox: { iblIntensity: 3000.0, skyboxIntensity: 6000.0, drawSkybox: true }, entities: es, shadows: true,
    postProcess: { enabled: true, lightingUnits: 1, exposureMode: 1, ev100: 15.0, evComp: 0.0, bloomOn: true, bloom: 0.5 } });
}
console.log("post_image: 4 シーンを書いた");
