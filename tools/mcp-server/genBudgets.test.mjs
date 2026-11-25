// W05-34: 予算生成器（tools/gen_budgets.mjs）の自己テストを npm test から呼ぶ（生成器の退行を M5 のゲートで捕まえる）。
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const r = spawnSync(process.execPath, [join(here, "..", "gen_budgets.mjs"), "--self-test"], { encoding: "utf8" });
process.stdout.write(r.stdout ?? "");
process.stderr.write(r.stderr ?? "");
if (r.status !== 0 || !/self-test OK/.test(r.stdout ?? "")) {
  console.error(`gen_budgets --self-test が失敗（exit ${r.status}）。出力に "self-test OK" が無い場合も失敗とする`);
  process.exitCode = 1;
}
