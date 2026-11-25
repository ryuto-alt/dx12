// run_tests.mjs の一覧検査（重複・欠落・連結順・offline-exclude）の自己テストを呼ぶ。
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const r = spawnSync(process.execPath, [join(here, "scripts", "run_tests.mjs"), "--self-test"], { stdio: "inherit" });
process.exitCode = r.status ?? 1;
