/**
 * Blender の自動起動と、dx12 へ流すための書き出し（BlenderMCP アドオンのソケットを直接叩く）。
 *
 * なぜ dx12 側から Blender を叩くのか（2026-09-10 にユーザーと合意）:
 *   「必要なモデルがあったら自分で Blender を立ち上げてモデリングして持ってくる」を成立させるには、
 *   ①Blender が起動しているか自分で確かめて要れば起動する ②書き出しの規約が守られている
 *   の 2 つが要る。②は文章で書いても毎回守られないので、**踏んだ罠を全部コードに埋めた
 *   書き出し関数**を 1 本用意して、それしか使わせない形にする。
 *
 * 埋めてある罠（すべて実際に踏んだもの。詳細は各所のコメント）:
 *   ・use_selection=False は .blend 内の【全シーン】を書き出す → 全部 deselect してから対象だけ選ぶ
 *   ・glTF エクスポータが画像を tmpXXXX.jpg という一時名で出す → 再書き出しで前の参照が切れる
 *   ・エンジンは baseColorFactor を読まない → テクスチャ無しの単色マテリアルは真っ白になる
 *   ・エンジンにアルファ抜きが無い → 葉・枝カードのような α 前提の面は不透明な板になる
 *   ・シェイプキーが .bin の大半を占める（実例: 75MB のうち 70MB）
 *   ・UI が日本語だとノード名も日本語 → nodes["Principled BSDF"] は KeyError。type で引く
 *
 * プロトコル: BlenderMCP アドオンは TCP 9876 で行 JSON ではなく「1 リクエスト = 1 JSON」を受け、
 * `{"status":"success","result":{...}}` を 1 回返して待ち受けに戻る。
 * execute_code は **stdout をそのまま result.result に入れて返す**ので、
 * スクリプト側は print(json.dumps(...)) で結果を戻す。
 */

import net from "node:net";

export const BLENDER_PORT = 9876;

// ─── ソケットクライアント ────────────────────────────────────────────────

export interface BlenderResponse {
  status?: string;
  result?: unknown;
  message?: string;
}

/** ポートが開いているか（＝アドオンのサーバーが動いているか）だけ見る。 */
export function isPortOpen(port: number, host = "127.0.0.1", timeoutMs = 700): Promise<boolean> {
  return new Promise((resolve) => {
    const sock = new net.Socket();
    let done = false;
    const finish = (v: boolean) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
    sock.setTimeout(timeoutMs);
    sock.once("connect", () => finish(true));
    sock.once("timeout", () => finish(false));
    sock.once("error", () => finish(false));
    sock.connect(port, host);
  });
}

/**
 * 1 コマンド送って 1 レスポンスを受ける。
 * ★アドオンは長さ枠を付けないので、受信を JSON として parse できるまで貯める。
 *   途中で切れた JSON を parse しようとして毎回落ちる実装にしないこと。
 */
export function blenderCall(
  type: string,
  params: Record<string, unknown> = {},
  opts: { port?: number; timeoutMs?: number } = {},
): Promise<BlenderResponse> {
  const port = opts.port ?? BLENDER_PORT;
  const timeoutMs = opts.timeoutMs ?? 120_000;   // モデリングは普通に数十秒かかる
  return new Promise((resolve, reject) => {
    const sock = new net.Socket();
    let buf = "";
    let settled = false;
    const fail = (e: Error) => { if (!settled) { settled = true; sock.destroy(); reject(e); } };
    sock.setTimeout(timeoutMs);
    sock.once("timeout", () => fail(new Error(`Blender が ${timeoutMs}ms 応答しない`)));
    sock.once("error", (e) => fail(e));
    sock.once("connect", () => sock.write(JSON.stringify({ type, params })));
    sock.on("data", (chunk) => {
      buf += chunk.toString("utf8");
      try {
        const parsed = JSON.parse(buf) as BlenderResponse;
        settled = true;
        sock.destroy();
        resolve(parsed);
      } catch {
        /* まだ全部届いていない。次の chunk を待つ */
      }
    });
    sock.once("close", () => {
      if (settled) return;
      try { resolve(JSON.parse(buf) as BlenderResponse); }
      catch { fail(new Error("Blender が応答を返さずに切断した")); }
    });
    sock.connect(port, "127.0.0.1");
  });
}

/** execute_code の戻りは stdout 文字列。JSON を print していれば取り出す。 */
export function parseCodeResult(resp: BlenderResponse): { stdout: string; json?: unknown } {
  const inner = (resp?.result ?? {}) as { result?: unknown; executed?: boolean };
  const stdout = typeof inner.result === "string" ? inner.result : "";
  // 末尾の行から順に JSON として読めるものを探す（print が複数あっても最後を採る）
  const lines = stdout.split(/\r?\n/).filter((l) => l.trim());
  for (let i = lines.length - 1; i >= 0; i--) {
    const l = lines[i].trim();
    if (!(l.startsWith("{") || l.startsWith("["))) continue;
    try { return { stdout, json: JSON.parse(l) }; } catch { /* 次の行 */ }
  }
  return { stdout };
}

// ─── Blender の実行ファイルを探す ────────────────────────────────────────

/** よくある場所を新しい版から順に返す（存在確認は呼び出し側）。 */
export function blenderCandidatePaths(programFiles = "C:\\Program Files"): string[] {
  const versions = ["5.2", "5.1", "5.0", "4.5", "4.4", "4.3", "4.2", "4.1", "4.0"];
  return versions.map((v) => `${programFiles}\\Blender Foundation\\Blender ${v}\\blender.exe`);
}

// ─── 書き出し規約 ────────────────────────────────────────────────────────

export interface ModelBrief {
  rules: string[];
  materials: string[];
  gotchas: string[];
}

/**
 * dx12 へ持ってくるモデルの作り方。Blender で作り始める**前**に読む。
 * ここに書いてあることは全部「守らないと実際に壊れた」項目。
 */
export function modelBrief(kind: string): ModelBrief {
  const common: ModelBrief = {
    rules: [
      "単位はメートル。1.8m の人なら Blender 上でも 1.8。spawn 時のスケールは常に 1 で置く",
      "原点は接地面の中心（底面の真ん中）。snap_to_ground と当たり判定がここを基準にする",
      "正面は +Y（Blender の -Y がエンジンの +Z）。壁に付ける家具は rotY を間違えると背板が手前に来る",
      "テクセル密度は 512〜1024 texel/m に揃える。UV は bpy.ops.uv.cube_project(cube_size=N) で N メートル = 1 UV",
      "三角形数は用途相応に。小物 1k〜5k / 家具 5k〜2万 / 主役 5万まで",
      "書き出しは glTF (.glb か .gltf+bin)。FBX は cm 基準なので避ける（読めるが余計な換算が挟まる）",
    ],
    materials: [
      "★単色マテリアルは禁止。エンジンは glTF の baseColorFactor を読まないので、テクスチャ無しは【真っ白】になる。真鍮・革・蝋のような単色で済ませたい物にも必ず col テクスチャを作る",
      "ORM は G=roughness / B=metallic。Blender では Separate Color の G を Roughness、B を Metallic に繋ぐ（factor は 1.0 のまま）",
      "法線は OpenGL 規約（nor_gl）。DirectX 規約（nor_dx）は使わない",
      "★アルファ抜きは無い。葉・枝カード・角膜のような α 前提の面は Blender で【消してから】出す。残すと不透明な板になる",
    ],
    gotchas: [
      "shape_key_clear() してから出す。使わないシェイプキーが .bin の大半を占める（実例: 75MB のうち 70MB）",
      "UI が日本語だとノード名も日本語。nodes['Principled BSDF'] は KeyError になるので type='BSDF_PRINCIPLED' で引く",
      "書き出し前に全シーンの全 view_layer で deselect してから対象だけ選ぶ（use_selection=False は .blend 内の全シーンを出す）",
      "transform_apply を重ねがけすると location が頂点に焼かれ、エンジンでだけ物が飛ぶ",
    ],
  };
  const k = kind.toLowerCase();
  if (k.includes("charact") || k.includes("キャラ") || k.includes("player") || k.includes("enemy")) {
    common.rules.push("スキンメッシュはボーン付きで出す。エンジンは FBX のスケール補正をスキンには掛けない規約なので glTF が安全");
    common.rules.push("アニメーションはクリップ名がそのまま Lua の playAnimByName に渡る名前になる");
  }
  if (k.includes("prop") || k.includes("小物") || k.includes("家具") || k.includes("furniture")) {
    common.rules.push("当たり判定を付けるなら箱で足りることが多い。凹んだ形が要るときだけ sculpt/MeshShape を検討する");
  }
  if (k.includes("level") || k.includes("床") || k.includes("wall") || k.includes("壁")) {
    common.rules.push("床・壁はエンジン側で rigidBody{motionType:0,mass:0} を必ず付ける（コライダーだけでは Jolt に載らない）");
    common.rules.push("床と壁を同一平面で突き合わせない。1mm 以内で重なると Z ファイティングでちらつく（dx12_validate_layout の Z_FIGHT）");
  }
  return common;
}

/**
 * 規約どおりに書き出す Blender Python を組み立てる。
 * objectNames が空なら選択中のオブジェクトを使う。
 *
 * ★この関数は純粋（文字列を返すだけ）。実行は blenderCall("execute_code", {code}) 側。
 */
export function buildExportScript(opts: {
  objectNames: string[];
  outPath: string;        // .glb か .gltf の絶対パス（Blender から見えるパス）
  clearShapeKeys?: boolean;
  applyModifiers?: boolean;
}): string {
  const names = JSON.stringify(opts.objectNames ?? []);
  const out = JSON.stringify(opts.outPath.replace(/\\/g, "/"));
  const clearSk = opts.clearShapeKeys === false ? "False" : "True";
  const applyMod = opts.applyModifiers === false ? "False" : "True";
  // Python 側のインデントを壊さないよう、テンプレートリテラルは素のまま埋める
  return `
import bpy, json, os

want = ${names}
out_path = ${out}
clear_shape_keys = ${clearSk}
apply_modifiers = ${applyMod}

report = {"exported": [], "warnings": [], "path": out_path}

# ★全シーンの全 view_layer で選択を解除してから対象だけ選ぶ。
#   use_selection=False にすると glTF は .blend 内の【全シーン】を書き出す（scenes は配列なので合法）。
#   別シーンで作業していても他シーンのオブジェクトが混ざる、という事故がこれで起きる。
for sc in bpy.data.scenes:
    for vl in sc.view_layers:
        for ob in sc.objects:
            try:
                ob.select_set(False, view_layer=vl)
            except Exception:
                pass

scene = bpy.context.scene
targets = []
if want:
    for n in want:
        ob = bpy.data.objects.get(n)
        if ob is None:
            report["warnings"].append("オブジェクトが見つからない: " + n)
        else:
            targets.append(ob)
else:
    targets = [o for o in bpy.context.selected_objects] or [o for o in scene.objects if o.type == 'MESH']

if not targets:
    print(json.dumps({"error": "書き出す対象が無い", "report": report}))
else:
    for ob in targets:
        try:
            ob.select_set(True)
        except Exception:
            pass
        report["exported"].append(ob.name)

        if ob.type == 'MESH':
            # シェイプキーは使わないなら捨てる。モーフターゲットは .bin の大半を占めることがある。
            if clear_shape_keys and ob.data.shape_keys:
                n_keys = len(ob.data.shape_keys.key_blocks)
                ob.shape_key_clear()
                report["warnings"].append(ob.name + ": シェイプキー " + str(n_keys) + " 個を削除した（容量削減）")

            # 単色マテリアル（画像テクスチャ無し）はエンジンで真っ白になる。必ず言う。
            for slot in ob.material_slots:
                mat = slot.material
                if mat is None or not mat.use_nodes:
                    report["warnings"].append(ob.name + ": マテリアルが無い/ノード無効 → エンジンでは真っ白になる")
                    continue
                has_image = any(n.type == 'TEX_IMAGE' and n.image for n in mat.node_tree.nodes)
                if not has_image:
                    report["warnings"].append(
                        ob.name + " / " + mat.name +
                        ": 画像テクスチャが 1 枚も無い → エンジンは baseColorFactor を読まないので真っ白になる")

    bpy.context.view_layer.objects.active = targets[0]
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    kwargs = dict(
        filepath=out_path,
        use_selection=True,          # ★ここが False だと全シーンが出る
        export_apply=apply_modifiers,
        export_yup=True,
    )
    try:
        bpy.ops.export_scene.gltf(**kwargs)
    except TypeError:
        # 版によって引数名が違う。落ちるくらいなら最小構成で出す。
        kwargs.pop("export_apply", None)
        bpy.ops.export_scene.gltf(**kwargs)

    report["size"] = os.path.getsize(out_path) if os.path.exists(out_path) else 0
    print(json.dumps(report))
`.trim();
}

/**
 * .gltf に付いてくる tmpXXXX.jpg のような一時名の画像を、意味のある名前へ直す計画を作る。
 * （Blender 5.2 の glTF エクスポータは画像を tmp 名で出すので、モデルを個別に書き出すと
 *   同じテクスチャが別名で重複し、再書き出しで名前が変わって**前に出したモデルの参照が切れる**。
 *   額縁が白・絨毯が黒になった、という形で実際に踏んだ。）
 *
 * gltf は .gltf の JSON（パース済み）、baseName は付けたい接頭辞。
 * 返り値は [{from, to}] の並びで、呼び出し側がファイル名変更と uri 書き換えを行う。
 */
export function planImageRenames(
  gltf: { images?: { uri?: string; name?: string }[]; materials?: unknown[] },
  baseName: string,
): { index: number; from: string; to: string }[] {
  const plan: { index: number; from: string; to: string }[] = [];
  const images = gltf.images ?? [];
  const used = new Set<string>();
  images.forEach((img, i) => {
    const uri = img.uri;
    if (!uri || uri.startsWith("data:")) return;          // 埋め込みは対象外
    const dot = uri.lastIndexOf(".");
    const ext = dot >= 0 ? uri.slice(dot) : ".png";
    const stem = dot >= 0 ? uri.slice(0, dot) : uri;
    // 明らかに一時名のものだけ直す。人が付けた名前は尊重する。
    if (!/^tmp[0-9a-z_]*$/i.test(stem) && !/^Image[._-]?\d*$/i.test(stem)) return;
    let to = `${baseName}_${i}${ext}`;
    let n = 2;
    while (used.has(to)) to = `${baseName}_${i}_${n++}${ext}`;
    used.add(to);
    plan.push({ index: i, from: uri, to });
  });
  return plan;
}
