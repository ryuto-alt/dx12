// scale_ladder.mjs が開く PerfTest プロジェクトの場所を決める純関数。
// 個人パスをリポジトリに置かないため、環境変数で受け取る。
//   1. DX12_PERFTEST_PROJECT（PerfTest フォルダそのもの）
//   2. DX12_GOLDEN_PROJECTS_ROOT/PerfTest（golden.mjs と同じ親フォルダ指定）
// どちらも無ければ null（呼び出し側が案内を出す）。
import path from "node:path";

export function resolvePerfTestProject(env = process.env) {
  if (env.DX12_PERFTEST_PROJECT) return env.DX12_PERFTEST_PROJECT;
  if (env.DX12_GOLDEN_PROJECTS_ROOT) return path.join(env.DX12_GOLDEN_PROJECTS_ROOT, "PerfTest");
  return null;
}
