// UnoFX（W1-BD7）のゴールデン用シーンを生成する。gen_scenes.mjs とは別（VFX の資産は engine/ 側にあり、プロジェクトの外を指す）。
//   vfx_flipbook : 夜の地面に、フリップブックの焚き火（炎 + 6-way の煙）を 1 つ。時刻は --deterministic のフレーム数で決まる。
//   vfx_sprite   : 同じ配置で旧系（スプライト）の p_core_fire。BD7 の A/B 比較（上半分の明るさ・ちらつき）用で、ゴールデンには登録しない。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), "assets", "scenes");
const tf = (p, r = [0, 0, 0], s = [1, 1, 1]) => ({ position: p, rotation: r, scale: s });
const scene = (asset) => ({
  skybox: { iblIntensity: 0.15, skyboxIntensity: 0.1, drawSkybox: true },
  shadows: false,
  entities: [
    { name: "Moon", directionalLight: { direction: [-0.3, -1.0, 0.2], color: [0.55, 0.65, 1.0], intensity: 0.35, ambient: 0.0 },
      transform: tf([0, 12, 0], [70, -20, 0]) },
    { name: "Ground", primitive: "plane", primitiveSize: 30, color: [0.18, 0.17, 0.16], material: { metallic: 0.0, roughness: 0.9 }, transform: tf([0, 0, 0]) },
    { name: "Stone", primitive: "box", color: [0.35, 0.34, 0.33], material: { metallic: 0.0, roughness: 0.8 }, transform: tf([-1.1, 0.25, 0.6], [0, 20, 0], [0.6, 0.5, 0.5]) },
    { name: "Fire", transform: tf([0, 0, 0]), vfx: { asset } },
  ],
});
const write = (name, s) => fs.writeFileSync(path.join(OUT, `${name}.json`), JSON.stringify(s, null, 2) + "\n");
write("vfx_flipbook", scene("engine/vfx_core/p_core_flipbook_fire.uvfx"));
write("vfx_sprite", scene("engine/vfx_core/p_core_fire.uvfx"));
