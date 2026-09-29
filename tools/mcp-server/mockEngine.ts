// テスト用の偽エンジン(TCP・改行区切り JSON)。実エンジンの振る舞いのうち、shell/エラー構造化が
// 依存する部分だけを再現する: ping.manifestHash / describe_mcp_manifest / describe_mcp_params /
// 未知 method(error_code 8 + error_did_you_mean) / エンティティ・シーン・アセットの一覧と NotFound /
// モード衝突 / 応答しない・遅い・切断 などのシナリオ。
//
// ★実エンジンには繋がない。method を実行中に足す/消す(setMethods)と manifestHash が変わる
//   ＝「再起動なしのツール追加」テストで、エンジンの再ビルド・再起動を模す。

import net from "node:net";
import type { ManifestMethod } from "./manifest.ts";

export type MockMethod = ManifestMethod & { handler?: (params: any, ctx: MockCtx) => any };
export type MockCtx = { state: MockState; received: { method: string; params: any }[] };

export type MockState = {
  mode: "Editor" | "Playing";
  entities: string[];
  scenes: string[];
  assets: string[];
  sceneGeneration: number;
  sceneDirty: boolean;
  virtualInput: boolean;
  /** 応答を返さない method(タイムアウト再現)。 */
  hang: Set<string>;
  /** method → 遅延 ms(遅延応答の再現)。 */
  delayMs: Record<string, number>;
  /** 接続を受けても何も返さない(別クライアントが握っている状態の再現)。 */
  silent: boolean;
  /** エンジンを古い版として振る舞わせる(manifestHash / describe_mcp_manifest を持たない)。 */
  legacyEngine: boolean;
  /** 構造化エラー(error_name など)を返す新しいエンジンか。false なら旧来の error / error_code / error_hint だけ。 */
  structuredErrors: boolean;
  /** 未知 method のとき、旧エンジンの形(error_code 2 の "unknown method: X")で返す。 */
  oldUnknownMethod: boolean;
};

const BASE_METHODS: string[] = [
  "ping", "list_entities", "get_entity", "find_entity", "set_transform", "set_component", "create_entity",
  "delete_entity", "spawn_model", "open_scene", "new_scene", "save_scene", "list_scenes", "list_assets", "play", "stop",
  "step_frames", "undo", "set_sun", "apply_lighting_preset", "terrain_generate", "sculpt_brush", "get_ssao", "set_ssao",
  "get_log", "git_push", "eval_lua", "imgui_pointer", "transaction_begin", "get_mode", "describe_mcp_params",
  "describe_mcp_manifest", "imgui_virtual_input",
];

function fnv1a64(s: string): string {
  let h = 0xcbf29ce484222325n;
  for (const b of Buffer.from(s, "utf8")) { h ^= BigInt(b); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
  return h.toString(16).padStart(16, "0");
}

function nearestNames(target: string, names: string[], n = 5): string[] {
  const t = target.toLowerCase();
  const d = (a: string, b: string) => {
    const dp = Array.from({ length: b.length + 1 }, (_, j) => j);
    for (let i = 1; i <= a.length; i++) { let prev = dp[0]; dp[0] = i; for (let j = 1; j <= b.length; j++) { const tmp = dp[j]; dp[j] = Math.min(dp[j] + 1, dp[j - 1] + 1, prev + (a[i - 1] === b[j - 1] ? 0 : 1)); prev = tmp; } }
    return dp[b.length];
  };
  return names.map((x) => ({ x, s: d(t, x.toLowerCase()) })).filter((o) => o.s <= Math.max(2, Math.floor(t.length / 3))).sort((a, b) => a.s - b.s).slice(0, n).map((o) => o.x);
}

export type MockOptions = Partial<MockState> & { methods?: MockMethod[] };

export async function startMockEngine(opts: MockOptions = {}) {
  const received: { method: string; params: any }[] = [];
  const state: MockState = {
    mode: "Editor", entities: ["Player", "Floor", "Wall_01", "Wall_02", "Light_Main"],
    scenes: ["scenes/default.json", "scenes/level1.json"], assets: ["models/tree.glb", "models/rock.glb", "textures/wood_albedo.png"],
    sceneGeneration: 3, sceneDirty: false, virtualInput: true, hang: new Set(), delayMs: {}, silent: false,
    legacyEngine: false, structuredErrors: true, oldUnknownMethod: false, ...opts,
  };
  const extra = new Map<string, MockMethod>();
  for (const m of opts.methods ?? []) extra.set(m.name, m);
  let removed = new Set<string>();
  const sockets = new Set<net.Socket>();

  const allNames = () => [...new Set([...BASE_METHODS, ...extra.keys()])].filter((n) => !removed.has(n))
    .filter((n) => !(state.legacyEngine && n === "describe_mcp_manifest"));
  const manifestOf = () => {
    const methods: ManifestMethod[] = allNames().map((name) => {
      const e = extra.get(name);
      if (e) { const { handler: _h, ...rest } = e; return rest as ManifestMethod; }
      return { name, category: "test", summary: `mock ${name}`, effect: /^(get_|list_|ping|describe_|find_)/.test(name) ? "read" : "write_scene", mode: "any", timeoutMs: 8000, params: [], source: "meta" } as ManifestMethod;
    }).sort((a, b) => a.name.localeCompare(b.name));
    return { protocol: 1, manifestHash: fnv1a64(JSON.stringify(methods)), engineVersion: "mock-1.0.0", count: methods.length, methods, categories: [] };
  };

  const err = (id: number, code: number, error: string, extraFields: Record<string, unknown> = {}) => {
    const out: any = { id, ok: false, error_code: code, error };
    for (const [k, v] of Object.entries(extraFields)) {
      if (!state.structuredErrors && !["error_hint", "error_values"].includes(k)) continue;
      out[k] = v;
    }
    return out;
  };

  const handle = (req: any): any | null => {
    const { id, method } = req; const params = req.params ?? {};
    received.push({ method, params });
    if (state.hang.has(method)) return null;
    const names = allNames();
    if (!names.includes(method)) {
      if (state.oldUnknownMethod || !state.structuredErrors) return { id, ok: false, error_code: 2, error: `unknown method: ${method}` };
      return err(id, 8, `unknown method: ${method}`, {
        error_name: "E_UNKNOWN_TOOL", error_did_you_mean: nearestNames(method, names),
        error_fix: [{ tool: "describe_mcp_manifest", args: {}, why: "method 一覧" }],
      });
    }
    const custom = extra.get(method)?.handler;
    if (custom) {
      try { return { id, ok: true, result: custom(params, { state, received }) }; }
      catch (e: any) { return err(id, e?.code ?? 7, String(e?.message ?? e), e?.fields ?? {}); }
    }
    switch (method) {
      case "ping":
        return {
          id, ok: true, result: {
            pong: true, mode: state.mode, entityCount: state.entities.length, sceneGeneration: state.sceneGeneration,
            currentScene: "scenes/default.json", sceneDirty: state.sceneDirty, protocolVersion: 4, virtualInput: state.virtualInput,
            background: "hidden", baseDir: "C:/mock/project", cwd: "C:/mock/bin", dpiScale: 1,
            ...(state.legacyEngine ? {} : { manifestHash: manifestOf().manifestHash, manifestProtocol: 1, engineVersion: "mock-1.0.0", engineStartedAtMs: 1000, methodCount: names.length }),
          },
        };
      case "describe_mcp_manifest": {
        const m = manifestOf();
        const only = params.method;
        if (only) { const one = m.methods.find((x) => x.name === only); if (!one) return err(id, 8, `unknown method: ${only}`, { error_name: "E_UNKNOWN_TOOL" }); return { id, ok: true, result: { ...m, methods: [one], count: 1 } }; }
        return { id, ok: true, result: m };
      }
      case "describe_mcp_params": {
        const methods: Record<string, unknown> = {};
        for (const n of names) methods[n] = (extra.get(n)?.params ?? []).map((p) => ({ key: p.name, type: p.type }));
        return { id, ok: true, result: { methods, count: names.length, globalKeys: ["idempotency_key"] } };
      }
      case "list_entities": return { id, ok: true, result: { entities: state.entities.map((name, i) => ({ entityId: i + 1, name })), count: state.entities.length, sceneGeneration: state.sceneGeneration } };
      case "list_scenes": return { id, ok: true, result: state.scenes.map((p) => ({ path: p, name: p.split("/").pop() })) };
      case "list_assets": return { id, ok: true, result: { assets: state.assets.map((p) => ({ path: p })) } };
      case "get_entity":
      case "find_entity": {
        const nm = params.name;
        if (nm !== undefined && !state.entities.includes(nm)) {
          if (method === "find_entity") return { id, ok: true, result: null };
          return err(id, 1, `no entity named '${nm}'`);
        }
        return { id, ok: true, result: { entityId: 1, name: nm ?? state.entities[0], componentTypes: ["transform"], sceneGeneration: state.sceneGeneration } };
      }
      case "delete_entity":
      case "set_transform":
      case "set_component": {
        const nm = params.name;
        if (nm !== undefined && !state.entities.includes(nm)) return err(id, 1, `no entity named '${nm}'`);
        return { id, ok: true, result: { applied: true } };
      }
      case "open_scene": {
        const p = params.path;
        if (p && !state.scenes.includes(p)) return err(id, 1, `scene not found: ${p}`);
        return { id, ok: true, result: { opened: p } };
      }
      case "spawn_model": {
        const p = params.model ?? params.path;
        if (p && !state.assets.includes(p)) return err(id, 1, `model not found: ${p}`);
        return { id, ok: true, result: { entityId: 99, name: "Model_01", sceneGeneration: state.sceneGeneration } };
      }
      case "new_scene":
        if (state.mode === "Playing") return err(id, 3, "cannot new_scene while Playing", { error_hint: "先に dx12_stop で Editor に戻してから new_scene を呼ぶこと" });
        return { id, ok: true, result: { ok: true } };
      case "terrain_generate":
        if (state.mode === "Playing") return err(id, 3, "terrain is Editor-only", { error_hint: "Play 中は地形を編集できない。dx12_stop してから撃つ" });
        return { id, ok: true, result: { generated: true } };
      case "play": state.mode = "Playing"; return { id, ok: true, result: { mode: "Playing" } };
      case "stop": state.mode = "Editor"; return { id, ok: true, result: { mode: "Editor" } };
      case "imgui_virtual_input": if (params.enable !== undefined) state.virtualInput = !!params.enable; return { id, ok: true, result: { enabled: state.virtualInput } };
      case "imgui_pointer":
        if (!state.virtualInput) return err(id, 3, "virtual input is OFF", { error_hint: "imgui_virtual_input {enable:true}" });
        return { id, ok: true, result: { ok: true } };
      case "get_log": return { id, ok: true, result: { lines: ["mock log"] } };
      case "save_scene": {
        if (params.path === undefined) return err(id, 14, "WIC stream open failed", { error_name: "E_FILE_IO" });
        return { id, ok: true, result: { saved: params.path } };
      }
      default: return { id, ok: true, result: { ok: true, method, params } };
    }
  };

  const server = net.createServer((sock) => {
    sockets.add(sock);
    sock.setEncoding("utf8");
    let buf = "";
    sock.on("error", () => {});
    sock.on("close", () => sockets.delete(sock));
    sock.on("data", (chunk: string) => {
      if (state.silent) return;
      buf += chunk;
      let nl: number;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1);
        if (!line) continue;
        let req: any; try { req = JSON.parse(line); } catch { continue; }
        const out = handle(req);
        if (out === null) continue;
        const delay = state.delayMs[req.method] ?? 0;
        const send = () => { if (!sock.destroyed) sock.write(JSON.stringify(out) + "\n"); };
        if (delay > 0) setTimeout(send, delay); else send();
      }
    });
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  const port = (server.address() as net.AddressInfo).port;

  return {
    port, state, received,
    /** エンジンの method 一覧を差し替える(=再ビルドして再起動した状態)。manifestHash が変わる。 */
    addMethod(m: MockMethod) { extra.set(m.name, m); removed.delete(m.name); },
    removeMethod(name: string) { extra.delete(name); removed.add(name); },
    manifestHash: () => manifestOf().manifestHash,
    methodNames: allNames,
    /** 接続を全部切る(エンジンが落ちた/再起動した状態)。 */
    dropConnections() { for (const s of sockets) s.destroy(); },
    close: () => new Promise<void>((r) => { for (const s of sockets) s.destroy(); server.close(() => r()); }),
  };
}
