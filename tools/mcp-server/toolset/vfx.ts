// VFX(パーティクル / トレイル)
// index.ts から機械分割したモジュール(コードの移動のみ・挙動不変)。ツールの登録順は index.ts の import 順で決まる。
import { z } from "zod";
import { describeLibrary, maxParticleLife, measureVfxFrames, resolveVfx, VFX_TAGS } from "../vfx.ts";
import { argError, v3 } from "../sceneTools.ts";
import path from "node:path";
import os from "node:os";
import fs from "node:fs";
import { guardApproval } from "../guardCtx.ts";
import { buildContactSheet } from "../contactSheet.ts";
import { engine, entityId, entityRef, errResult, reg, regRaw, run } from "./core.ts";
import type { ToolResult } from "./core.ts";

// ════════════════════════════════════════════════════════════════
//  VFX（パーティクル / トレイル）
// ════════════════════════════════════════════════════════════════
// エンジンのパーティクルは 1 レイヤー 40 フィールド近くあり、生の数値を並べても
// まず「それらしく」ならない。ここは 3 段構えにしてある:
//   ① dx12_vfx_library  … 何が作れるかを知る（レシピ一覧）
//   ② dx12_vfx_apply    … レシピ + 倍率で【複数レイヤーまとめて】置く
//   ③ dx12_vfx_preview  … 時間を進めながら連写して「本当に出ているか」を数える
// 生のレイヤー操作(下の 3 本)は、レシピから外れた微調整をするときに使う。

reg(
  "dx12_list_particle_layers",
  "パーティクルのレイヤー一覧",
  "放出器のレイヤーを {index, name, kind, rate, looping, offset, vfxPath} で列挙する。"
  + "★dx12_set_component の layer 引数に渡す名前/添字はここで分かる。"
  + "Trigger の PlayEffect / StopEffect に渡すレイヤー名も同じもの。"
  + "名前が空のレイヤーは \"Layer 1\" のように 1 始まりの既定名で引ける。",
  { ...entityRef },
  { readOnlyHint: true, idempotentHint: true },
  ({ entity, name }) => run(() => engine.call("list_particle_layers", { entity, name })),
);

reg(
  "dx12_add_particle_layer",
  "パーティクルのレイヤーを追加",
  "放出器にレイヤーを 1 枚足す(上限 16 枚)。中身は続けて dx12_set_component(component:'particleEmitter', layer:<index か名前>) で書く。"
  + "★これが無いと 1 枚目しか触れない＝『炎に煙を重ねる』ができない。本物の炎/爆発は必ず複数レイヤーの重ね合わせ。"
  + "レシピから一気に組むなら dx12_vfx_apply の方が速い(このツールを内部で呼んでいる)。",
  {
    ...entityRef,
    layerName: z.string().optional().describe("レイヤー名(省略で \"Layer N\")。Trigger/Lua から指すのに使うので付けた方がよい。"),
  },
  {},
  ({ entity, name, layerName }) => run(() => engine.call("add_particle_layer", { entity, name, layerName })),
);

reg(
  "dx12_remove_particle_layer",
  "パーティクルのレイヤーを削除",
  "放出器のレイヤーを 1 枚消す。★最後の 1 枚は消せない(付いているのに何も出ない状態を作らないため)。"
  + "放出器ごと消すなら dx12_remove_component component='particleEmitter'。",
  {
    ...entityRef,
    layer: z.union([z.number().int(), z.string()]).describe("消すレイヤーの index(0 始まり)か名前。"),
  },
  { destructiveHint: true },
  ({ entity, name, layer }) => run(() => engine.call("remove_particle_layer", { entity, name, layer })),
);

reg(
  "dx12_vfx_library",
  "VFX レシピ一覧",
  "使える VFX レシピ(松明/焚き火/爆発/魔法陣/雨/雪/剣閃 など)を一覧する。"
  + "各項目は {id, title, summary, tags, sizeHint, layers, layerNames, hasTrail, oneShot}。"
  + "★どれも【複数レイヤーの重ね合わせ】で作ってある(炎＋煙＋火の粉)。1 種類の粒だけでは本物に見えないため。"
  + "id を dx12_vfx_apply に渡して置き、dx12_vfx_preview で確認するのが基本の流れ。"
  + "tag で絞れる: fire / smoke / magic / impact / weather / water / electric / trail / ambient / oneshot / loop / light。",
  {
    tag: z.string().optional().describe("タグで絞る(fire / smoke / magic / impact / weather / water / electric / trail / ambient / oneshot / loop / light 等)。"),
    id: z.string().optional().describe("1 つの id を指定すると、そのレシピの【全レイヤーの実値】と注意書きまで返す(適用前に中身を確かめたいとき)。"),
  },
  { readOnlyHint: true, idempotentHint: true },
  ({ tag, id }) => run(async () => {
    if (id) {
      const r = resolveVfx(id);
      return {
        id: r.preset.id, title: r.preset.title, summary: r.preset.summary,
        tags: r.preset.tags, sizeHint: r.preset.sizeHint, notes: r.preset.notes,
        lookHint: r.preset.lookHint,
        layers: r.layers, trail: r.trail,
        estimatedLiveParticles: r.estimatedLiveParticles,
        warnings: r.warnings,
      };
    }
    const list = describeLibrary(tag);
    if (list.length === 0) {
      throw argError(`タグ "${tag}" に当てはまるレシピが無い`, "tag を省略して全件見るか、下の有効値から選ぶ", VFX_TAGS);
    }
    return {
      presets: list, count: list.length, tags: VFX_TAGS,
      next: "dx12_vfx_apply(preset:<id>, position:[x,y,z]) で置いて、dx12_vfx_preview で出ているか確認する",
    };
  }),
);

reg(
  "dx12_vfx_apply",
  "VFX レシピを置く / 貼る",
  "レシピから【複数レイヤーの放出器】を 1 コールで組み立てる(新規エンティティを作るか、既存エンティティへ付ける)。"
  + "内部では create_entity → add_particle_layer ×N → set_component(layer 指定) ×N を順に撃っている。"
  + "★倍率で調整する: scale(大きさ。size/speed/offset/lightRange に掛かる) / rate(密度) / intensity(HDR 強度) / "
  + "color(主色。加算レイヤーだけ塗り替え、煙は元の色のまま。終了色は明度比を保って追従) / oneShot / life。"
  + "★Editor 限定(新規生成を伴うため)。既存エンティティへ付けるだけなら Play 中でも通るが、Stop で巻き戻る。"
  + "置いた後は必ず dx12_vfx_preview で『本当に出ているか』を確認すること(粒は静止画 1 枚では判断できない)。",
  {
    preset: z.string().describe("レシピ id(dx12_vfx_library で一覧)。例: torch / campfire / explosion / magic_circle / rain。"),
    ...entityRef,
    position: v3().optional().describe("[x,y,z]。新規作成時の置き場所。既存エンティティに渡すと移動する。"),
    entityName: z.string().optional().describe("新規作成するエンティティ名(既定 'FX_<preset>')。name は【既存を指す】引数なので別物。"),
    parentName: z.string().optional().describe("親にするエンティティ名。命名規約に従うなら 'FX'(dx12_scene_scaffold が作るグループ)。"),
    scale: z.number().optional().describe("大きさの倍率(既定 1)。0.5 で半分、2 で倍。寿命は変えない。"),
    rate: z.number().optional().describe("放出レートの倍率(既定 1)。粒の密度。"),
    intensity: z.number().optional().describe("HDR 強度の倍率(既定 1)。ブルームの乗り方が変わる。"),
    color: v3().optional().describe("[r,g,b] 0..1。加算レイヤーの主色を置き換える(魔法陣の色替え等)。"),
    oneShot: z.boolean().optional().describe("true で全レイヤーをワンショット化(looping=false, playOnStart=false)。Trigger の PlayEffect で鳴らす。"),
    duration: z.number().optional().describe("ワンショットの放出継続秒。"),
    life: z.number().optional().describe("粒の寿命の倍率(既定 1)。長くすると滞空が増える＝粒数も増える。"),
    light: z.boolean().optional().describe("実ポイントライト化の強制 ON/OFF(省略でレシピのまま)。同じ効果を大量に置くときは false にして予算を守る。"),
    replaceLayers: z.boolean().optional().describe("既存の放出器に付けるとき、余ったレイヤーを消すか(既定 true)。false なら残す。"),
    dryRun: z.boolean().optional().describe("true で【何も変更せず】適用予定の値だけ返す。値を確かめてから撃ちたいとき。"),
  },
  {},
  ({ preset, entity, name, position, entityName, parentName, scale, rate, intensity, color,
     oneShot, duration, life, light, replaceLayers, dryRun }) => run(async () => {
    const r = resolveVfx(preset, { scale, rate, intensity, color, oneShot, duration, life, light });
    if (dryRun) {
      return {
        dryRun: true, preset: r.preset.id, layers: r.layers, trail: r.trail,
        notes: r.preset.notes, warnings: r.warnings,
        estimatedLiveParticles: r.estimatedLiveParticles,
      };
    }

    // ── ① 対象エンティティを決める ──
    const needsEmitter = r.layers.length > 0;
    let targetId: number | undefined = entity;
    let targetName: string | undefined = name;
    let created = false;
    if (targetId === undefined && !targetName) {
      const nm = entityName ?? `FX_${r.preset.id}`;
      const res = await engine.call("create_entity", {
        // レイヤーが 0 枚のレシピ(剣閃など)は放出器を持たせない＝空エンティティで作る
        type: needsEmitter ? "particle_emitter" : "empty",
        name: nm, position: position ?? [0, 0, 0], parentName,
      }) as any;
      targetId = res?.entityId;
      targetName = res?.name ?? nm;
      created = true;
    } else if (position) {
      await engine.call("set_transform", { entity: targetId, name: targetName, position });
    }
    const ref = targetId !== undefined ? { entity: targetId } : { name: targetName };

    // ── ② レイヤーを必要枚数そろえる ──
    const applied: Array<Record<string, unknown>> = [];
    let removed = 0;
    if (needsEmitter) {
      let have = 0;
      try {
        const cur = await engine.call("list_particle_layers", ref) as any;
        have = cur?.count ?? 0;
      } catch {
        // ParticleEmitter がまだ無い。下の set_component(layer 無し)が 1 枚目ごと作る。
        have = 0;
      }
      for (let i = 0; i < r.layers.length; i++) {
        const layer = r.layers[i];
        if (i >= have) {
          if (i === 0 && have === 0) {
            // 1 枚目は set_component が放出器ごと作る(layer を渡さない＝1 枚目の意味)
            await engine.call("set_component", {
              ...ref, component: "particleEmitter", data: { ...layer },
            });
            have = 1;
            applied.push({ index: 0, name: layer.name });
            continue;
          }
          await engine.call("add_particle_layer", { ...ref, layerName: layer.name });
          have = i + 1;
        }
        await engine.call("set_component", {
          ...ref, component: "particleEmitter", layer: i, data: { ...layer },
        });
        applied.push({ index: i, name: layer.name });
      }
      // 余分なレイヤーを消す(最後の 1 枚は消せない仕様なので、needed >= 1 のときだけ)
      if (replaceLayers !== false && have > r.layers.length && r.layers.length >= 1) {
        for (let i = have - 1; i >= r.layers.length; i--) {
          await engine.call("remove_particle_layer", { ...ref, layer: i });
          removed++;
        }
      }
    }

    // ── ③ トレイル ──
    let trailApplied = false;
    if (r.trail) {
      await engine.call("set_component", { ...ref, component: "trailRenderer", data: { ...r.trail } });
      trailApplied = true;
    }

    // ── ④ 読み返して嘘をつかない ──
    let layersNow: unknown = null;
    if (needsEmitter) {
      layersNow = await engine.call("list_particle_layers", ref).catch(() => null);
    }

    const oneShotNow = r.layers.length > 0 && r.layers.every((l) => l.looping === false);
    return {
      applied: true,
      preset: r.preset.id, title: r.preset.title,
      entityId: targetId, name: targetName, created,
      layersApplied: applied, layersRemoved: removed, trailApplied,
      current: layersNow,
      estimatedLiveParticles: r.estimatedLiveParticles,
      notes: r.preset.notes,
      lookHint: r.preset.lookHint,
      warnings: r.warnings,
      next: oneShotNow
        ? "ワンショットなので置いただけでは鳴らない。dx12_vfx_preview(fire:true) で試し撃ちするか、"
          + "Trigger の PlayEffect(type:4, target:このエンティティ名) で鳴らす配線をすること"
        : "dx12_vfx_preview で『本当に出ているか』を確認する(粒は静止画 1 枚では判断できない)",
    };
  }),
);

// プレビュー: 時間を進めながら連写して 1 枚の格子画像 + 計測値を返す。
// ★パーティクルは時間方向にしか存在しない。静止画 1 枚だと「たまたま写っていない」のか
//   「そもそも出ていない」のか区別できず、AI が延々と見当違いの修正を繰り返す原因になる。
regRaw(
  "dx12_vfx_preview",
  {
    title: "VFX を時間で見る",
    description:
      "放出器に寄って【時間を進めながら N 枚連写】し、格子画像 1 枚 + 計測値を返す。"
      + "★測り方: 先に放出器を一時的に遠くへ退けて『効果が無いときの絵』(baseline)を 1 枚撮り、"
      + "それとの差で効果の影響範囲・明るさ・白飛びを数える。"
      + "同じ場所で燃え続ける炎のように【動かない効果】でも正しく測れるのはこのため"
      + "(フレーム間の差だけで測ると、止まって見える効果を『出ていない』と誤判定する)。"
      + "退けた放出器は必ず元の位置へ戻す(失敗しても戻す)。"
      + "★deterministic ステップで進めるので、往復の遅さに関係なく毎回同じ間隔で撮れる(撮り終わりに解除する)。"
      + "★ワンショット(looping=false)は置いただけでは鳴らない。fire:true を渡すと "
      + "fx:play で鳴らしてから撮る(Editor のまま鳴らせるので Play/Stop は不要)。"
      + "image ブロック + text(計測値と助言)を返す。",
    inputSchema: {
      ...entityRef,
      seconds: z.number().optional().describe("撮る合計の長さ(秒)。既定 1.5。炎や煙は 2〜3 秒、爆発は 1 秒で足りる。"),
      frames: z.number().int().optional().describe("撮る枚数(2..12)。既定 6。"),
      distance: z.number().optional().describe("カメラを離す距離 m(既定 3)。効果が大きいレシピでは 6〜10 にする。"),
      height: z.number().optional().describe("注視点を放出器から何 m 上にするか(既定 0.5)。立ち上る炎/煙は 1〜2 が見やすい。"),
      fire: z.boolean().optional().describe("true でワンショットを試し撃ちする(一時的に playOnStart を立てて Play→Stop。元に戻す)。"),
      columns: z.number().int().optional().describe("格子の列数(既定 3)。"),
      additive: z.boolean().optional().describe("加算系の効果として判定する(既定 true)。煙/雪/血だけのレシピでは false にすると『暗い』警告が出なくなる。"),
      baseline: z.boolean().optional().describe(
        "効果を退けた基準画を撮るか(既定 true)。false にすると時間方向の中央値で代用する"
        + "＝速いが、動かない効果を『出ていない』と誤判定することがある。"),
    },
    annotations: { title: "VFX を時間で見る", openWorldHint: false, readOnlyHint: false },
  },
  async ({ entity, name, seconds, frames, distance, height, fire, columns, additive, baseline }) => {
    const ref: Record<string, unknown> = entity !== undefined ? { entity } : { name };
    let restorePos: number[] | null = null;   // 退けた放出器を戻すための元位置
    let firedLayers: number[] = [];
    let playing = false;
    let savedCam: { position: number[]; yawDeg: number; pitchDeg: number } | null = null;   // 人のエディタカメラ(撮影後に戻す。R1-07)
    let ownTx = false;   // 自分で開いたトランザクションがあるか(finally で必ず rollback する)
    let ownComponentWrites = 0;   // うち set_component の発行数(エンジンは Undo コマンド名を種類ごとに付けるので、名前では自分の分と区別できない)
    let ownWrites = 0;   // トランザクション中に自分が出したUndo を積む書き込み(set_transform / set_component)の発行数(eval_lua は Undo を積まないので数えない)
    const preNotes: string[] = [];
    let result: ToolResult | undefined;
    const call = (method: string, params: any) => {
      if (ownTx && (method === "set_transform" || method === "set_component")) ownWrites++;
      if (ownTx && method === "set_component") ownComponentWrites++;
      return engine.call(method, params);
    };
    try {
      if (entity === undefined && !name) {
        throw argError("entity か name のどちらかが要る", "放出器のエンティティを指定する");
      }
      const info = await call("get_entity", ref) as any;
      // ★BUG-13: プレビューの退避(set_transform / set_component)と復元を 1 つのトランザクションで包み、最後に必ず rollback する。
      //   rollback なら Undo に何も残らず、トランザクション中は自動保存も走らない。
      //   開けなかったとき(古いエンジン / 既に誰かが開いている)は従来どおり個別に復元する。
      try {
        await call("transaction_begin", { label: "vfx_preview" });
        ownTx = true;
      } catch (e: any) {
        const msg = String(e?.message ?? e);
        if (/unknown method|未対応|不明なメソッド/i.test(msg)) {
          preNotes.push(`このエンジンはトランザクションに対応していないので、プレビュー中の変更を個別に戻した(Undo に履歴が残ることがある): ${msg}`);
        } else if (/already open|既に開い/i.test(msg)) {
          preNotes.push(
            `★別のトランザクションが既に開いているので、プレビューの変更はその中に入った。`
            + `そのトランザクションの commit / rollback と運命を共にする。個別の復元は行った: ${msg}`);
        } else {
          // 分類できない失敗(Play 中の MODE_CONFLICT・通信タイムアウトなど)。タイムアウトでも begin が通っていることがあるので状態を読む。
          let adopted = false;
          try {
            const ts = await engine.call("transaction_status", {}) as any;
            if (ts?.open === true && ts?.label === "vfx_preview") { ownTx = true; adopted = true; }
          } catch { /* 状態も読めない */ }
          preNotes.push(adopted
            ? `transaction_begin は失敗を返したが実際には開いていた(${msg})。自分のものとして最後に rollback する。`
            : `トランザクションを開けなかった(別のトランザクションとは限らない。Play 中など): ${msg}。個別の復元を行った。`);
        }
      }
      const pos: number[] = info?.transform?.position ?? info?.position ?? [0, 0, 0];
      const layers: any[] = info?.particleEmitter?.layers ?? [];
      const dist = Math.max(0.3, distance ?? 3);
      const lookY = height ?? 0.5;
      const nFrames = Math.max(2, Math.min(12, Math.round(frames ?? 6)));
      const total = Math.max(0.1, seconds ?? 1.5);
      const stepFrames = Math.max(1, Math.round((total * 60) / nFrames));
      const stamp = Date.now();
      let shotIndex = 0;

      // カメラを 3/4 の位置へ。target は少し上(立ち上る効果を画角に入れるため)。
      const camPos: [number, number, number] = [pos[0] + dist * 0.75, pos[1] + dist * 0.45, pos[2] + dist * 0.75];
      const camTarget: [number, number, number] = [pos[0], pos[1] + lookY, pos[2]];
      // 撮影用にカメラを動かす前に、人が見ているエディタカメラを控える(set_editor_camera は Undo に載らず rollback でも戻らない)。
      try {
        const cam = await call("get_editor_camera", {}) as any;
        if (cam?.mode !== "Playing" && Array.isArray(cam?.position) && cam.position.length === 3
            && Number.isFinite(cam.yawDeg) && Number.isFinite(cam.pitchDeg)) {
          savedCam = { position: cam.position, yawDeg: cam.yawDeg, pitchDeg: cam.pitchDeg };
        }
      } catch { /* 読めなければ復元はできない(release だけ行う) */ }
      await call("set_editor_camera", { position: camPos, target: camTarget });

      // ★撮影先は必ず【絶対パスで明示】する。省略するとエンジンは自分の CWD へ書くが、
      //   ヘッドレス起動だと CWD が書けない場所(C:\Windows\System32 等)になることがあり、
      //   その場合 "WIC stream open failed" で撮影ごと失敗する(実際に踏んだ)。
      const shoot = async (): Promise<Buffer> => {
        const outFrame = path.join(os.tmpdir(), `dx12_vfx_frame_${stamp}_${shotIndex++}.png`);
        const shot = await call("screenshot_final", { gizmos: false, path: outFrame }) as any;
        const got = shot?.path ?? outFrame;
        if (!fs.existsSync(got)) throw new Error(`screenshot_final が PNG を残さなかった: ${got}`);
        const buf = fs.readFileSync(got);
        fs.rmSync(got, { force: true });
        return buf;
      };
      const step = async (n: number) => {
        for (let left = n; left > 0; left -= 600) {
          await call("step_frames", {
            frames: Math.min(600, left), deterministic: true, hold: true,
          });
        }
      };

      const isOneShot = layers.length > 0 && layers.every((l) => l.looping === false);
      const settleSec = Math.min(2.0, maxParticleLife(layers) + 0.2);
      const settleFrames = Math.max(6, Math.round(settleSec * 60));

      // ── baseline(効果が無いときの絵)──
      let baselineBuf: Buffer | undefined;
      if (baseline !== false) {
        if (isOneShot && !fire) {
          // ワンショットはまだ鳴っていない＝今の絵がそのまま基準画
          baselineBuf = await shoot();
        } else {
          // 放出器を遠くへ退けて、生きている粒が寿命で消えるまで待ってから撮る
          await call("set_transform", { ...ref, position: [pos[0], pos[1] + 5000, pos[2]] });
          restorePos = pos;
          await step(settleFrames);
          baselineBuf = await shoot();
          await call("set_transform", { ...ref, position: pos });
          restorePos = null;
          await step(settleFrames);   // 戻してから定常状態になるまで待つ
        }
      }

      // ── ワンショットを鳴らす ──
      // ★engine v1.18.0+ の fx:play を使う。Editor のまま鳴らせるので Play/Stop が要らない
      //   （以前は playOnStart を立てて Play→Stop→戻す、という往復をしていた。
      //     シーンが作り直されるぶん遅く、戻し損ねると playOnStart が残る危険もあった）。
      if (fire) {
        const nm = (info?.name ?? name) as string | undefined;
        if (!nm) {
          preNotes.push("fire:true だが名前が引けなかったので鳴らせなかった(name 指定で呼ぶこと)。");
        } else {
          for (const l of layers) if (l.looping === false) firedLayers.push(layers.indexOf(l));
          // 固定の 1 行(名前は JSON.stringify で埋める)。ユーザーのコードではないので、この内部呼び出しだけ eval_lua のゲートを承認済みにする。
          const r = await guardApproval.run({ approved: true, via: "internal:vfx_preview" }, () => call("eval_lua", {
            code: `return tostring(fx:play(${JSON.stringify(nm)}))`,
          })).catch(() => null) as any;
          const okFire = typeof r?.result === "string" ? r.result.includes("true") : false;
          if (!okFire) {
            preNotes.push(
              "fx:play が false を返した(エンジンが v1.18.0 より古いか、放出器が無い)。"
              + "古いエンジンなら Trigger の PlayEffect で鳴らすこと。",
            );
          }
        }
      }

      const shots: Buffer[] = [];
      for (let i = 0; i < nFrames; i++) {
        await step(stepFrames);
        shots.push(await shoot());
      }
      // 時間を止めたままにしない(hold の解除)。
      await call("step_frames", { frames: 1, deterministic: true, hold: false }).catch(() => {});

      if (playing) {
        await call("stop", {}).catch(() => {});
        playing = false;
        for (const idx of firedLayers) {
          await call("set_component", {
            ...ref, component: "particleEmitter", layer: idx, data: { playOnStart: false },
          }).catch(() => {});
        }
        firedLayers = [];
      }

      const sheet = buildContactSheet(shots, { columns: columns ?? 3 });
      const measure = measureVfxFrames(shots, { additive: additive !== false, baseline: baselineBuf });
      const outPath = path.join(os.tmpdir(), `dx12_vfx_preview_${stamp}.png`);
      fs.writeFileSync(outPath, sheet.sheetPng);

      result = {
        content: [
          { type: "image", data: sheet.sheetPng.toString("base64"), mimeType: "image/png" },
          {
            type: "text",
            text: JSON.stringify({
              path: outPath,
              entityId: info?.entityId ?? entity, name: info?.name ?? name,
              frames: nFrames, secondsTotal: Number((stepFrames * nFrames / 60).toFixed(3)),
              secondsPerFrame: Number((stepFrames / 60).toFixed(4)),
              camera: { position: camPos, target: camTarget },
              firedOneShotLayers: firedLayers,
              ...(preNotes.length > 0 ? { notes: preNotes } : {}),
              backgroundFrom: measure.backgroundFrom,
              visible: measure.visible,
              sceneLuma: measure.sceneLuma,
              stats: measure.frames,
              frameDiffs: sheet.frameDiffs,
              suggestions: measure.suggestions,
            }),
          },
        ],
      };
    } catch (e: any) {
      result = errResult(e);
    } finally {
      // 退けたまま / Play したまま / 時間を止めたままにしない。
      if (restorePos) {
        await call("set_transform", { ...ref, position: restorePos }).catch(() => {});
      }
      if (playing) {
        await call("stop", {}).catch(() => {});
        for (const idx of firedLayers) {
          await call("set_component", {
            ...ref, component: "particleEmitter", layer: idx, data: { playOnStart: false },
          }).catch(() => {});
        }
      }
      await call("step_frames", { frames: 1, deterministic: true, hold: false }).catch(() => {});
      // 撮影用のカメラ固定を残さない(残るとゲーム画面の撮影が全部この視点になる)
      if (savedCam) {
        await call("set_editor_camera", { position: savedCam.position, yawDeg: savedCam.yawDeg, pitchDeg: savedCam.pitchDeg }).catch((e: any) => {
          preNotes.push(`★エディタカメラを元の視点へ戻せなかった(${e?.message ?? e})。`);
        });
      }
      await call("set_editor_camera", { release: true }).catch(() => {});
      // 最後に rollback(成功・失敗どちらでも)。Undo には残さない(自動保存の扱いはエンジン側 L1 の修正待ち)。
      // rollback の失敗・想定外の中身は握りつぶさず、結果に載せる。
      if (ownTx) {
        try {
          // rollback の前に中身を読み、自分の書き込み以外が混じっていないか確かめる(rollback の calls は Undo を横取りした件数で、eval_lua は数えない)。
          let foreign: string[] | null = null;
          let human = 0;
          try {
            const ts = await engine.call("transaction_status", {}) as any;
            // ★callNames はエンジンの Undo コマンド名("Transform" など)で、MCP の method 名("set_transform")ではない。
            //   method 名だけを自分の分とみなすと、本物のエンジンでは自分の set_transform が全部「他の呼び出し」になり偽の警告が出る(R1-06)。
            //   set_transform 由来の "Transform" は自分の分。set_component 由来は名前が種類ごとに違うので、自分が発行した数だけ許す。
            if (Array.isArray(ts?.callNames)) {
              const others = ts.callNames.map(String).filter((n: string) => n !== "set_transform" && n !== "Transform");
              const compLike = others.filter((n: string) => n === "set_component");
              const rest = others.filter((n: string) => n !== "set_component");
              foreign = [...compLike, ...rest.slice(Math.min(ownComponentWrites, rest.length))];
            }
            human = Number(ts?.humanEditsDuringTransaction) || 0;
          } catch { /* 読めなければ calls の件数で代用 */ }
          const rb = await engine.call("transaction_rollback", {}) as any;
          const calls = Number(rb?.calls);
          if ((foreign && foreign.length > 0) || human > 0 || (foreign === null && Number.isFinite(calls) && calls > ownWrites)) {
            preNotes.push(
              `★警告: rollback でプレビュー以外の変更も破棄された可能性がある(`
              + (foreign && foreign.length > 0 ? `他の呼び出し: ${foreign.slice(0, 8).join(", ")}; ` : "")
              + (human > 0 ? `人の編集 ${human} 件; ` : "")
              + `calls=${Number.isFinite(calls) ? calls : "?"}, 自分の書き込み=${ownWrites})。別クライアントや並列呼び出しの書き込みが巻き戻されていないか確認すること。`);
          }
        } catch (e: any) {
          preNotes.push(
            `★警告: rollback に失敗した(${e?.message ?? e})。Play / Ctrl+Z などでトランザクションが自動で閉じられ、`
            + `Undo に 'vfx_preview' が積まれたか、シーンが変更されたままの可能性がある。Undo の深さとシーンの変更状態を確認すること。`);
        }
      }
    }
    // rollback の結果を含めるため、結果はここで組み立て直す。
    if (result && preNotes.length > 0) {
      const t = result.content.find((c: any) => c.type === "text") as any;
      if (result.isError) {
        if (t) t.text += "\n" + preNotes.join("\n");
      } else if (t) {
        try {
          const o = JSON.parse(t.text);
          o.notes = preNotes;
          t.text = JSON.stringify(o);
        } catch { t.text += "\n" + preNotes.join("\n"); }
      }
    }
    return result!;
  },
);
