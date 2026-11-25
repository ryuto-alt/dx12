// W05-36: 比較に使う TS とエンジンの「出どころ」を記録・照合する。名札(--ts-ref)だけでなく、実際の commit を残す。
//   gitInfo(dir)   : dir(MCP サーバのフォルダ)の HEAD commit、作業ツリーの汚れ、タグ。
//   readNightly(d) : --build-dir の nightly.json から ref / commit を読む。
//   checkBaseline  : ベースライン実行の前提(タグ指定・commit 一致・汚れなし)を検査して理由を投げる。
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";

export type GitInfo = { commit: string; dirty: boolean; tags: string[] };
export type Provenance = {
  tsRef: string; tsCommit: string; tsDirty: boolean; buildRef: string; buildCommit: string; assetsSource: string;
};

function git(dir: string, args: string[]): { ok: boolean; out: string } {
  const r = spawnSync("git", ["-C", dir, ...args], { encoding: "utf8" });
  return { ok: r.status === 0, out: (r.stdout ?? "").trim() };
}

/** git で commit を引けないときは null。 */
export function gitInfo(dir: string): GitInfo | null {
  const h = git(dir, ["rev-parse", "HEAD"]);
  if (!h.ok || !h.out) return null;
  const st = git(dir, ["status", "--porcelain", "--", "."]);
  const tg = git(dir, ["tag", "--points-at", "HEAD"]);
  return { commit: h.out, dirty: st.ok && st.out.length > 0, tags: tg.ok && tg.out ? tg.out.split(/\r?\n/) : [] };
}

/** タグ(や ref)が指す commit。無ければ null。 */
export function resolveRef(dir: string, ref: string): string | null {
  const r = git(dir, ["rev-parse", "--verify", "--quiet", `${ref}^{commit}`]);
  return r.ok && r.out ? r.out : null;
}

export function readNightly(buildDir: string): { ref: string; commit: string } | null {
  if (!buildDir) return null;
  try {
    const j = JSON.parse(fs.readFileSync(path.join(buildDir, "nightly.json"), "utf8").replace(/^﻿/, ""));
    return { ref: String(j.ref ?? ""), commit: String(j.commit ?? "") };
  } catch { return null; }
}

export function collectProvenance(tsDir: string, tsRef: string, buildDir: string): Provenance {
  const g = gitInfo(tsDir);
  const n = readNightly(buildDir);
  return {
    tsRef, tsCommit: g?.commit ?? "", tsDirty: g?.dirty ?? false,
    buildRef: n?.ref ?? "", buildCommit: n?.commit ?? "",
    assetsSource: buildDir ? "current-checkout(engine_instance.ps1 は -BuildDir でも assets/shaders-src を現行のチェックアウトから写す)" : "current-checkout",
  };
}

/** ベースライン実行の前提を検査する。満たさなければ理由つきで投げる。 */
export function checkBaseline(tsDir: string, tsRef: string, buildDir: string): void {
  if (!tsRef || tsRef === "unpinned") throw new Error("ベースラインでは --ts-ref にタグ名が必要(unpinned は拒否)");
  const g = gitInfo(tsDir);
  if (!g) throw new Error(`--ts-dir が git で引けない: ${tsDir}`);
  const want = resolveRef(tsDir, tsRef);
  if (!want) throw new Error(`--ts-ref ${tsRef} が --ts-dir のリポジトリに無い`);
  if (want !== g.commit) throw new Error(`--ts-dir の HEAD(${g.commit.slice(0, 10)})が --ts-ref ${tsRef}(${want.slice(0, 10)})と違う。そのタグをチェックアウトしてから実行する`);
  if (g.dirty) throw new Error("--ts-dir に未コミットの変更がある(タグの中身と一致しない)");
  if (buildDir && !readNightly(buildDir)) throw new Error(`--build-dir に nightly.json が無い(エンジンの ref/commit を記録できない): ${buildDir}`);
}

/** replay 時の照合。食い違いの理由の配列(空なら一致)。 */
export function compareTsToTrace(traceMeta: Record<string, unknown>, tsDir: string): string[] {
  const d: string[] = [];
  const g = gitInfo(tsDir);
  const want = typeof traceMeta["tsCommit"] === "string" ? (traceMeta["tsCommit"] as string) : "";
  if (!want) { d.push("トレースの meta に tsCommit が無い(どの TS で撮ったか分からない)"); return d; }
  if (!g) { d.push(`--ts-dir の commit を引けない: ${tsDir}`); return d; }
  if (g.commit !== want) d.push(`TS の commit が違う: トレース ${want.slice(0, 10)}(tsRef=${String(traceMeta["tsRef"] ?? "?")}) / --ts-dir ${g.commit.slice(0, 10)}`);
  return d;
}
