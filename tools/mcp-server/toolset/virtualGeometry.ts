// 仮想ジオメトリ(Nanite 風。docs/VIRTUAL_GEOMETRY_DESIGN.md)のツール 2 本:
//   dx12_vg_stats            … GPU カリング + ラスタの統計(エンジン method vg_stats)
//   dx12_set_virtual_geometry … ON/OFF・τ(lodPixelError)・HZB・コーン棄却・ラスタ・計測(エンジン method set_virtual_geometry)
// 引数の一覧はエンジン側 src/core/mcp/ApplicationMcpManifest.cpp の meta(vg_stats / set_virtual_geometry)。VG の段階(P4 以降)で引数が増えたら、
// この shape に足す(足さないと zod が黙って捨てる)。ドリフトは schemaDrift.test.ts [14] が突き合わせる。
//
// 登録の作法は pathTracer.ts / フリート / ジョブと同じ:
//   ・legacy 面(旧 220 本のスナップショット = 1 バイトも変えない回帰基準)には出さない
//   ・full 面: 旧 220 本の直後(パストレーサーの次)/ core・shell 面: tools/list には出さず dx12_call で使う(長尾)
import { z } from "zod";
import { engine, errResult, server, type ToolResult } from "./core.ts";
import { ENHANCED, SURFACE, TOOL_REGISTRY } from "../toolRuntime.ts";
import { unknownKeyIssues, bodyFromIssues } from "../validate.ts";
import { envelope } from "../errors.ts";
import { ERROR_BODY } from "../toolRuntime.ts";

export const VG_TOOLS = ["dx12_vg_stats", "dx12_set_virtual_geometry", "dx12_vg_cook"];

const SET_SHAPE: Record<string, z.ZodTypeAny> = {
  enabled: z.boolean().optional().describe("GPU カリング + メッシュシェーダ描画(既定 OFF)。"),
  lodPixelError: z.number().min(0.25).max(8).optional().describe("LOD の誤差しきい値 τ(px。既定 1)。"),
  hzbCulling: z.boolean().optional().describe("HZB オクルージョン(既定 true)。"),
  coneCulling: z.boolean().optional().describe("法線コーンの背面棄却(既定 true)。"),
  instanceMinPx: z.number().min(0).max(64).optional().describe("画面上の半径(px)がこれ未満のインスタンスを棄却(0 で無効。既定 0.5)。"),
  vramBudgetMB: z.number().int().min(64).max(65536).optional().describe("アセットの VRAM 予算 MB(超える読込は拒否。既定 3072)。"),
  raster: z.boolean().optional().describe("メッシュシェーダで VG 本体を描く(既定 true。false = プロキシを描く。実行時のみ)。"),
  rasterAs: z.boolean().optional().describe("増幅シェーダ経由で描く(既定 false)。"),
  smallPrimCull: z.boolean().optional().describe("画素中心を覆わない三角形を落とす(既定 false)。"),
  measure: z.boolean().optional().describe("計測を vg_stats.raster へ出す(少し遅い。既定 false)。"),
  forceLod0: z.boolean().optional().describe("検証用。LOD0 の葉クラスタだけを選ぶ(三角形が桁違いに増える)。"),
  resolve: z.boolean().optional().describe("材質 resolve(既定 true。false = 暫定シェーディング。実行時のみ)。"),
  hzbExactSphere: z.boolean().optional().describe("HZB 判定に球の正確な矩形を使う(既定 true。実行時のみ)。"),
  lodOutputPixels: z.boolean().optional().describe("τ を出力解像度の px で測る(既定 false。実行時のみ)。"),
  shadows: z.boolean().optional().describe("影にも VG 本体を描く(既定 true。シーンに保存)。"),
  shadowLodTexels: z.number().min(0.25).max(8).optional().describe("影の LOD の誤差しきい値(テクセル。既定 0.5。シーンに保存)。"),
  shadowTriBudgetM: z.number().min(0).max(1000).optional().describe("影: カスケード 1 枚の三角形の目安(百万。既定 4。0 = 上限なし。シーンに保存)。"),
  stableOrder: z.boolean().optional().describe("決定論(可視リストを安定ソート)。決定論キャプチャ中は自動で ON(既定 false)。"),
};

const COOK_SHAPE: Record<string, z.ZodTypeAny> = {
  path: z.string().describe("モデルの assets 相対パス(または絶対パス)。glb / gltf / fbx / obj。"),
  force: z.boolean().optional().describe("cook 済みでも作り直す(既定 false)。"),
};

type Def = { title: string; description: string; shape: Record<string, z.ZodTypeAny>; annotations: Record<string, unknown>; keywords: string; method: string };
const DEFS: Record<string, Def> = {
  dx12_vg_stats: {
    title: "仮想ジオメトリの統計", method: "vg_stats", shape: {},
    description:
      "仮想ジオメトリ(Nanite 風)の統計を返す({enabled, active, instances, visibleClusters, trianglesDrawn, cullGpuMs, vramMB, overflow, raster{…}, "
      + "resolve{…}, stableOrder{…}, vgGpuTotalMs, ineligible{…}, autoCook{active, recent}, assets[…]}。約 3 フレーム遅れ)。対象は VirtualGeometry コンポーネントを持つエンティティ。設定は dx12_set_virtual_geometry。",
    annotations: { readOnlyHint: true, idempotentHint: true, openWorldHint: false },
    keywords: "vg virtual geometry nanite 仮想ジオメトリ クラスタ カリング 統計 vgeo",
  },
  dx12_set_virtual_geometry: {
    title: "仮想ジオメトリの設定", method: "set_virtual_geometry", shape: SET_SHAPE,
    description:
      "仮想ジオメトリ(Nanite 風)の設定を変えて、現在の統計を返す。enabled = ON/OFF(既定 OFF)、lodPixelError = LOD の τ。SM 6.6 + Resource Binding Tier 3 が無い GPU では無効(gpuSupported:false)。",
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    keywords: "vg virtual geometry nanite 仮想ジオメトリ 設定 enabled lodPixelError hzb cone",
  },
  dx12_vg_cook: {
    title: "モデルを仮想ジオメトリへ cook", method: "vg_cook", shape: COOK_SHAPE,
    description:
      "モデル(glb / gltf / fbx / obj)を仮想ジオメトリ(Nanite 風)へ裏で cook する(エンジンと同じ import 設定・座標変換。出力は assets/.modelcache/vg/)。"
      + "置いてあるエンティティは cook が終わると自動でプロキシ + VG に差し替わる。20 万三角形以上の静的・不透明なモデルは読み込み時に自動で cook されるので、"
      + "これは「しきい値未満でも VG にしたい」「作り直したい(force)」とき用。{queued, model, note} を返す。進み具合と結果は dx12_vg_stats の autoCook.active / recent。",
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: false },
    keywords: "vg cook 仮想ジオメトリ 変換 vgeo モデル 作り直し force nanite",
  },
};

if (ENHANCED && SURFACE !== "legacy") {
  for (const name of VG_TOOLS) {
    const def = DEFS[name];
    const declared = Object.keys(def.shape);
    const invoke = async (args: any): Promise<ToolResult> => {
      const issues = unknownKeyIssues(args ?? {}, declared);
      if (issues.length > 0) {
        const res: ToolResult = { content: [{ type: "text", text: JSON.stringify(envelope(bodyFromIssues(name, args ?? {}, issues, declared))) }], isError: true };
        ERROR_BODY.set(res, bodyFromIssues(name, args ?? {}, issues, declared));
        return res;
      }
      try {
        const data = await engine.call(def.method, args ?? {});
        return { content: [{ type: "text", text: JSON.stringify(data) }], structuredContent: { result: (data ?? null) as any } };
      } catch (e: any) {
        return errResult(e);
      }
    };
    // dx12_vg_cook だけは長尾(tools/list に出さない。visual.ts と同じ作法)。他の VG は従来どおり。
    const hidden = name === "dx12_vg_cook" || SURFACE === "shell" || SURFACE === "core";
    const registered = server.registerTool(
      name,
      { title: def.title, description: def.description, inputSchema: z.object(def.shape).passthrough() as any, annotations: { title: def.title, ...def.annotations } },
      async (args: any) => invoke(args),
    );
    if (hidden) registered.disable();
    TOOL_REGISTRY.set(name, {
      name, title: def.title, description: def.description, shape: def.shape, annotations: def.annotations, tier: "core", core: false, coreDescription: def.description,
      extraKeywords: def.keywords, invoke: (args: any) => invoke(args), listed: !hidden, registered,
    });
  }
}

// schemaDrift.test.ts [14] が、エンジンの set_virtual_geometry が読むキーと突き合わせる。
export const VG_SET_SHAPE_KEYS = Object.keys(SET_SHAPE);
