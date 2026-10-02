// MCP 全 method の応答時間・応答サイズ・固まり(別接続の ping 最大遅延)を測る。
//   node tools/mcp_perf/mcp_perf.mjs <port> [--effect read|write|all] [--only a,b] [--skip a,b]
//                                    [--args overrides.json] [--out result.json] [--timeout ms]
// 先に engine_instance.ps1 で背景起動し、シーンを開いておく(例: open_scene {path})。
//  - 既定は read 系だけ。write は使い捨てコピーのプロジェクトでだけ --effect write/all を使うこと。
//  - guarded / 入力系 / 外部に出る method(git push 等)は常に対象外(SKIP_ALWAYS)。
//  - overrides.json: { "method": {args...} | [{args...},...] }。無い method は {} で呼ぶ。
//  - プレースホルダ: "@ID"(数値の entityId) "@NAME" を、シーン中ほどのエンティティで置換する。
//  - 固まり: MCP は 1 接続ずつ・メインスレッドで同期実行なので、同期 method の応答時間 ≒ その間のフレームの固まり。
//  - 出力: 標準出力に表(method, ms, KB, 固まり ms, ok/err)と、しきい値超過の一覧。--out で JSON も残す。
import net from "node:net";
import fs from "node:fs";

const argv = process.argv.slice(2);
const port = Number(argv[0]);
const opt = (k, d) => { const i = argv.indexOf("--" + k); return i >= 0 ? argv[i + 1] : d; };
const effectSel = opt("effect", "read");
const only = opt("only", "") ? opt("only", "").split(",") : null;
const skipUser = opt("skip", "") ? opt("skip", "").split(",") : [];
const overrides = opt("args", "") ? JSON.parse(fs.readFileSync(opt("args", ""), "utf8")) : {};
const outFile = opt("out", "");
const timeoutMs = Number(opt("timeout", "120000"));
const THR = { ms: 1000, kb: 1024, stall: 100 };

const SKIP_ALWAYS = new Set([
  "eval_lua", "git_push", "git_pull", "git_fetch", "git_commit", "git_merge", "git_checkout", "git_merge_abort",
  "delete_asset", "build_game", "install_font", "engine_launch", "job_start", "net_launch_test_client",
  "key_press", "key_down", "key_up", "mouse_move", "ui_click", "imgui_virtual_input", "imgui_pointer", "imgui_key",
  "render_reference", "record_playtest", "run_playtests", "autoplay", "blender_ensure", "blender_export",
  "blender_material", "blender_polish", "jev_ask", "jev_eval", "open_project", "import_asset", "move_asset",
  "focus_and_screenshot", "screenshot_final", "benchmark",
]);

function connect() {
  return new Promise((res, rej) => {
    const s = net.connect(port, "127.0.0.1");
    s.setNoDelay(true);
    let buf = "", nextId = 1; const waiters = new Map();
    s.on("data", (d) => {
      buf += d.toString("utf8");
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i); buf = buf.slice(i + 1);
        const w = (() => { try { const m = JSON.parse(line); return [m, waiters.get(m.id)]; } catch { return [null, null]; } })();
        if (w[1]) { waiters.delete(w[0].id); w[1]({ msg: w[0], bytes: Buffer.byteLength(line) }); }
      }
    });
    s.on("error", rej);
    s.on("connect", () => res({
      call(method, params = {}, tmo = timeoutMs) {
        return new Promise((r) => {
          const id = nextId++; const t0 = performance.now();
          const to = setTimeout(() => { waiters.delete(id); r({ ms: performance.now() - t0, bytes: 0, timeout: true }); }, tmo);
          waiters.set(id, ({ msg, bytes }) => { clearTimeout(to); r({ ms: performance.now() - t0, bytes, msg }); });
          s.write(JSON.stringify({ id, method, params }) + "\n");
        });
      },
      close() { s.destroy(); },
    }));
  });
}

const main = await connect();
const man = await main.call("describe_mcp_manifest", { brief: true });
let methods = man.msg.result.methods;
// overrides の "_order": [method,...] があれば、その順だけを実行する(write 系は順序が意味を持つ)
if (Array.isArray(overrides._order)) { const byName = new Map(methods.map((m) => [m.name, m])); methods = overrides._order.map((n) => byName.get(n)).filter(Boolean); }
let ent = { id: 1, name: "Grid" };
async function pickEntity() {
  const le = await main.call("list_entities", {});
  const es = le.msg?.result?.entities ?? [];
  if (es.length) { const e = es[Math.floor(es.length / 2)]; ent = { id: e.entityId, name: e.name }; }
}
await pickEntity();
const fill = (a) => JSON.parse(JSON.stringify(a).replace(/"@ID"/g, String(ent.id)).replace(/"@NAME"/g, JSON.stringify(ent.name)));
const rows = [];
for (const m of methods) {
  if (only && !only.includes(m.name)) continue;
  if (SKIP_ALWAYS.has(m.name) || skipUser.includes(m.name)) continue;
  const eff = m.effect;
  if (eff === "guarded") continue;
  if (effectSel === "read" && eff !== "read") continue;
  if (effectSel === "write" && eff === "read") continue;
  const variants = overrides[m.name] ? (Array.isArray(overrides[m.name]) ? overrides[m.name] : [overrides[m.name]]) : [{}];
  for (const args of variants) {
    const r = await main.call(m.name, fill(args));
    if (m.name === "open_scene" || m.name === "new_scene") await pickEntity();
    const ok = r.msg ? r.msg.ok !== false : false;
    const row = { method: m.name, effect: eff, args: JSON.stringify(args).slice(0, 80), ms: Math.round(r.ms), kb: Math.round(r.bytes / 102.4) / 10,
      ok, timeout: !!r.timeout, err: ok ? "" : (r.msg?.error?.message || r.msg?.error || (r.timeout ? "TIMEOUT" : "")).toString().slice(0, 90) };
    rows.push(row);
    console.log(`${row.method.padEnd(30)} ${String(row.ms).padStart(7)}ms ${String(row.kb).padStart(8)}KB ${ok ? "ok " : "ERR"} ${row.err}`);
  }
}
console.log("\n--- しきい値超過 (ms>=%d / KB>=%d / stall>=%d) ---", THR.ms, THR.kb, THR.stall);
for (const r of rows) if (r.ms >= THR.ms || r.kb >= THR.kb || r.timeout)
  console.log(`${r.method} ${r.args} ms=${r.ms} kb=${r.kb}${r.timeout ? " TIMEOUT" : ""}`);
if (outFile) fs.writeFileSync(outFile, JSON.stringify(rows, null, 1));
main.close(); process.exit(0);
