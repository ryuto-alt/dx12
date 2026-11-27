// 品質プリセット: dx12_get_quality_preset / dx12_set_quality_preset(FU-E-40)。
// エンジン method は get_quality_preset / set_quality_preset(ApplicationMcpLaneC.cpp)。
// 登録の作法は shading.ts と同じ: legacy 面(旧 220 本のスナップショット)には出さず、どの面でも tools/list には出さない(長尾。dx12_call / dx12_tool_search で使う)。
import { z } from "zod";
import { engine, errResult, server, type ToolResult } from "./core.ts";
import { ENHANCED, SURFACE, TOOL_REGISTRY, ERROR_BODY } from "../toolRuntime.ts";
import { unknownKeyIssues, bodyFromIssues } from "../validate.ts";
import { envelope } from "../errors.ts";

export const QUALITY_PRESET_TOOLS = ["dx12_get_quality_preset", "dx12_set_quality_preset"];
export const QUALITY_PRESETS = ["low", "medium", "high", "epic", "cinematic"] as const;

const BUDGET_NOTE =
  "返り値は {preset, label, hasBudget, applied, note} 。hasBudget:true(high / epic / cinematic)のときだけ予算 "
  + "{targetFps, output, p95Ms, p99Ms, vfx(放出倍率・最大粒子数)} が付く。low / medium は hasBudget:false で予算の項目は無い"
  + "(maxParticles=0 は『上限なし』の意味)。値の正は budgets/quality_presets.json の生成表。"
  + "★C1 時点では選択の記録だけで、applied:false は『絵にはまだ反映していない』の意味。";

type Def = { title: string; description: string; shape: Record<string, z.ZodTypeAny>; annotations: Record<string, unknown>; keywords: string; method: string };
const DEFS: Record<string, Def> = {
  dx12_get_quality_preset: {
    title: "品質プリセット取得", method: "get_quality_preset", shape: {},
    description: "現在の品質プリセット(low / medium / high / epic / cinematic)を返す。読み取り専用。" + BUDGET_NOTE,
    annotations: { readOnlyHint: true, idempotentHint: true, openWorldHint: false },
    keywords: "品質プリセット 画質 予算 epic high cinematic 目標fps 読む quality preset budget target fps p95 p99 vfx emission",
  },
  dx12_set_quality_preset: {
    title: "品質プリセット変更",
    method: "set_quality_preset",
    shape: { preset: z.enum(QUALITY_PRESETS).describe("品質プリセット名(小文字)。low | medium | high | epic | cinematic") },
    description:
      "品質プリセット(low / medium / high / epic / cinematic)を選ぶ。プリセット名は小文字で渡すこと。" + BUDGET_NOTE
      + "絵への反映と Undo は後続(C7)で入る(C7 を入れるときはこの説明も直す)。性能を測る前に合わせる用途は dx12_perf_gate が内部で呼ぶ。",
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    keywords: "品質プリセット 画質 切替 epic high cinematic low medium 予算 目標fps quality preset set switch graphics quality budget",
  },
};

if (ENHANCED && SURFACE !== "legacy") {
  for (const name of QUALITY_PRESET_TOOLS) {
    const def = DEFS[name];
    const declared = Object.keys(def.shape);
    const invoke = async (args: any): Promise<ToolResult> => {
      const issues = unknownKeyIssues(args ?? {}, declared);
      if (issues.length > 0) {
        const body = bodyFromIssues(name, args ?? {}, issues, declared);
        const res: ToolResult = { content: [{ type: "text", text: JSON.stringify(envelope(body)) }], isError: true };
        ERROR_BODY.set(res, body);
        return res;
      }
      try {
        const data = await engine.call(def.method, args ?? {});
        return { content: [{ type: "text", text: JSON.stringify(data) }], structuredContent: { result: (data ?? null) as any } };
      } catch (e: any) {
        return errResult(e);
      }
    };
    const hidden = true;
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
