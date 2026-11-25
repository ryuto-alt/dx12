// node スクリプトがあれば実行し、まだ無ければ終了コード 77（ctest の SKIP_RETURN_CODE）で抜ける。
// 使い方: node run_if_exists.mjs <script.mjs> [args...]
import { existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
const [script, ...args] = process.argv.slice(2);
if (!script || !existsSync(script)) {
  console.log(`SKIP: ${script} はまだ存在しない`);
  process.exit(77);
}
process.exit(spawnSync(process.execPath, [script, ...args], { stdio: 'inherit' }).status ?? 1);
