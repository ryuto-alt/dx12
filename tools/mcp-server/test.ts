import net from "node:net";
import assert from "node:assert";
import { EngineClient } from "./engineClient.ts";

// C++ ブリッジを模した mock サーバ。改行区切り JSON を受けて id 相関で返す。
// EngineClient のフレーミング/相関/エラー経路だけを検証する(エンジン不要)。
const PORT = 8799;
const server = net.createServer((sock) => {
  sock.setEncoding("utf8");
  let buf = "";
  sock.on("data", (d: string) => {
    buf += d;
    let i: number;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      const req = JSON.parse(line);
      const resp =
        req.method === "list_entities" ? { id: req.id, ok: true, result: [{ id: 1, name: "Player" }] }
        // v1 で追加した method の代表をモック。params をそのまま返して相関を確認する。
        : req.method === "get_mode" ? { id: req.id, ok: true, result: { mode: "Editor" } }
        : req.method === "save_scene" ? { id: req.id, ok: true, result: { path: req.params?.path ?? null } }
        : req.method === "list_scenes" ? { id: req.id, ok: true, result: [{ path: "scenes/title.json", name: "title" }] }
        : req.method === "set_component" ? { id: req.id, ok: true, result: req.params }
        : req.method === "boom" ? { id: req.id, ok: false, error: "kaboom" }
        : { id: req.id, ok: false, error: "unknown" };
      sock.write(JSON.stringify(resp) + "\n");
    }
  });
});

await new Promise<void>((r) => server.listen(PORT, "127.0.0.1", () => r()));

const c = new EngineClient("127.0.0.1", PORT, 3000);

// 正常系: result が返る
assert.deepStrictEqual(await c.call("list_entities", {}), [{ id: 1, name: "Player" }]);

// 異常系: ok:false は error を throw する
await assert.rejects(() => c.call("boom", {}), /kaboom/);

// 引数なし method: result オブジェクトが返る
assert.deepStrictEqual(await c.call("get_mode", {}), { mode: "Editor" });

// params 透過: params が正しくシリアライズされ往復する
assert.deepStrictEqual(await c.call("save_scene", { path: "scenes/x.json" }), { path: "scenes/x.json" });
assert.deepStrictEqual(await c.call("save_scene", {}), { path: null });

// オブジェクト params (set_component の data) も保持される
assert.deepStrictEqual(
  await c.call("set_component", { entity: 1, component: "pointLight", data: { intensity: 2 } }),
  { entity: 1, component: "pointLight", data: { intensity: 2 } },
);

// 並行: 2本同時でも各 id に正しく紐づく
const [a, b] = await Promise.all([c.call("list_entities", {}), c.call("list_entities", {})]);
assert.deepStrictEqual(a, [{ id: 1, name: "Player" }]);
assert.deepStrictEqual(b, [{ id: 1, name: "Player" }]);

server.close();
console.log("OK: engineClient framing/correlation/error all pass");
process.exit(0);
