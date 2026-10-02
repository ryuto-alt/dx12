// 1 method を N 回呼び、応答時間・サイズ・先頭を出す。 node call.mjs <port> <method> '<json>' [reps=1] [headChars=300]  (json は @ファイル名 も可)
import net from "node:net";
import fs from "node:fs";
let [port, method, pj = "{}", reps = "1", head = "300"] = process.argv.slice(2);
if (pj.startsWith("@")) pj = fs.readFileSync(pj.slice(1), "utf8");
const s = net.connect(Number(port), "127.0.0.1");
let buf = "", t0 = 0, n = 0; const times = [];
s.on("error", (e) => { console.log("ERR " + e.message); process.exit(3); });
const send = () => { t0 = performance.now(); s.write(JSON.stringify({ id: ++n, method, params: JSON.parse(pj) }) + "\n"); };
s.on("connect", send);
s.on("data", (d) => {
  buf += d.toString("utf8"); let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    times.push(performance.now() - t0);
    if (n >= Number(reps)) {
      console.log(`${method} ms=[${times.map((x) => Math.round(x)).join(",")}] bytes=${Buffer.byteLength(line)}`);
      console.log(line.slice(0, Number(head))); process.exit(0);
    }
    send();
  }
});
