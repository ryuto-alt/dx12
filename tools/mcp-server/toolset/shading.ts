// 材質シェーディングのシーン設定(マルチスキャッタのエネルギー補償): dx12_get_shading / dx12_set_shading。
// エンジン method は get_shading / set_shading(src/core/mcp/ApplicationMcpRender.cpp)。
// 登録の作法は virtualGeometry.ts と同じ: legacy 面(旧 220 本のスナップショット)には出さず、full 面ではその後ろ / core・shell 面は dx12_call で使う。
import { z } from "zod";
import { engine, errResult, server, type ToolResult } from "./core.ts";
import { ENHANCED, SURFACE, TOOL_REGISTRY, ERROR_BODY } from "../toolRuntime.ts";
import { unknownKeyIssues, bodyFromIssues } from "../validate.ts";
import { envelope } from "../errors.ts";

export const SHADING_TOOLS = ["dx12_get_shading", "dx12_set_shading"];

const SET_SHAPE: Record<string, z.ZodTypeAny> = {
  multiScatter: z.boolean().optional().describe("マルチスキャッタのエネルギー補償(既定 true。粗い金属が暗く沈まない)。false で従来(補償なし)と同じ絵。"),
};

type Def = { title: string; description: string; shape: Record<string, z.ZodTypeAny>; annotations: Record<string, unknown>; keywords: string; method: string };
const DEFS: Record<string, Def> = {
  dx12_get_shading: {
    title: "材質シェーディング設定取得", method: "get_shading", shape: {},
    description: "材質シェーディングのシーン設定を返す。{multiScatter}(マルチスキャッタのエネルギー補償が ON か)。",
    annotations: { readOnlyHint: true, idempotentHint: true, openWorldHint: false },
    keywords: "shading multiScatter multi scatter マルチスキャッタ エネルギー補償 材質 金属 粗い",
  },
  dx12_set_shading: {
    title: "材質シェーディング設定変更", method: "set_shading", shape: SET_SHAPE,
    description:
      "材質シェーディングのシーン設定を変える。multiScatter(既定 true)= マルチスキャッタのエネルギー補償(粗い金属が暗く沈まない)。"
      + "false で従来(補償なし)と同じ絵。シーン JSON の shading.multiScatter に保存(既定の true のときはキーを書かない)。{applied, multiScatter} を返す。",
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    keywords: "shading multiScatter multi scatter マルチスキャッタ エネルギー補償 材質 金属 粗い 設定",
  },
};

if (ENHANCED && SURFACE !== "legacy") {
  for (const name of SHADING_TOOLS) {
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
    const hidden = SURFACE === "shell" || SURFACE === "core";
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
