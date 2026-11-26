// 検査ジョブ(visual_regression / perf_gate)が使うエンジンの入手口。テストでは差し替える。
//   attach … 束縛中(または engine 指定)のエンジンを使う。close は何もしない。
//   launch … 専用エンジンを起動する。窓は出さない(background / headless だけ。tools/engine_instance.ps1 経由)。
//            リポジトリにあれば tools/bench/lib/engine.mjs(golden.mjs と同じ起動なので、ネイティブ解像度が一致する)、
//            無ければフリートで起動する。
import path from "node:path";
import { pathToFileURL } from "node:url";
import fs from "node:fs";
import type { KindEnv } from "../jobs/env.ts";
import { errBody, StepError, type EngineCall } from "./common.ts";

export type EngineHandle = { call: EngineCall; close(): Promise<void>; id?: string; port?: number };
export type LaunchOpts = {
  name: string; project: string; mode?: "background" | "headless";
  extraArgs?: string[]; scene?: string; buildDir?: string; port?: number; dpiScale?: number;
};
export type EngineProvider = {
  attach(ref?: string): EngineHandle;
  launch(o: LaunchOpts): Promise<EngineHandle>;
};

/** リポジトリの tools/bench/lib/<name>.mjs を遅延 import する。配布版(dx12-mcp)には無いので、無ければ null。 */
export async function importBench(repoDir: string, name: string): Promise<any | null> {
  const f = path.join(repoDir, "tools", "bench", "lib", name);
  if (!fs.existsSync(f)) return null;
  try { return await import(pathToFileURL(f).href); } catch { return null; }
}

export function defaultProvider(env: KindEnv): EngineProvider {
  return {
    attach(ref) {
      const id = env.resolveEngineId(ref);
      return { call: (m, p, o) => env.callEngine(id ?? ref, m, p ?? {}, o), close: async () => {}, id };
    },
    async launch(o) {
      const bench = await importBench(env.cfg.repoDir, "engine.mjs");
      if (bench) {
        const buildDir = o.buildDir ?? env.cfg.buildDir;
        const exe = buildDir ? path.join(buildDir, "DX12Engine.exe") : bench.findDefaultExe();
        const eng = await bench.launchInstance({
          name: o.name, project: o.project, port: o.port, buildDir: buildDir ?? path.dirname(exe), mode: o.mode ?? "headless",
          extraArgs: [...(o.extraArgs ?? []), ...(o.dpiScale ? ["--dpi-scale", String(o.dpiScale)] : []), ...(o.scene ? ["--scene", o.scene] : [])],
        });
        let conn: any;
        try { conn = await bench.connect(eng.port); } catch (e) { eng.kill(); throw e; }
        return {
          port: eng.port,
          call: (m, p, opts) => conn.call(m, p ?? {}, opts?.timeout ?? 180_000),
          close: async () => {
            try { conn.close(); } catch { /* 切れている */ }
            eng.kill();
            // プロセスが落ちてポートが閉じるまで待つ(次の起動が同じポート範囲で空きを探すため。最大 15 秒)
            for (let i = 0; i < 30 && (await bench.portOpen(eng.port)); i++) await new Promise((r) => setTimeout(r, 500));
          },
        };
      }
      // フリート経由ではビルドのフォルダを選べない。指定されたのに黙って無視すると A/B が同じバイナリになるので、できないと返す。
      if (o.buildDir && env.cfg.buildDir && path.resolve(o.buildDir) !== path.resolve(env.cfg.buildDir)) {
        throw new StepError(errBody("E_UNSUPPORTED", `buildDir(${o.buildDir})でエンジンを起動できない(tools/bench が無い配布版では、フリートが使うビルドしか起動できない)`));
      }
      const fleet = env.fleet();
      if (!fleet) throw new StepError(errBody("E_FLEET_DISABLED", "専用エンジンを起動できない(tools/bench が無く、フリートも無効)"));
      const r: any = await fleet.launch({
        name: o.name, project: o.project, mode: o.mode ?? "headless", args: o.extraArgs ?? [], noBind: true,
        ...(o.dpiScale ? { dpiScale: o.dpiScale } : {}), ...(o.scene ? { scene: o.scene } : {}),
      });
      const id = String(r.engineId);
      return { id, port: r.port, call: (m, p, opts) => env.callEngine(id, m, p ?? {}, opts), close: async () => { try { await fleet.stop({ engine: id }); } catch { /* アイドルで落ちる */ } } };
    },
  };
}
