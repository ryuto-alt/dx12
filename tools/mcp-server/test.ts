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

// 並行: 2本同時でも各 id に正しく紐づく
const [a, b] = await Promise.all([c.call("list_entities", {}), c.call("list_entities", {})]);
assert.deepStrictEqual(a, [{ id: 1, name: "Player" }]);
assert.deepStrictEqual(b, [{ id: 1, name: "Player" }]);

server.close();
console.log("OK: engineClient framing/correlation/error all pass");
process.exit(0);
