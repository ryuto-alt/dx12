// GroveLab のカートレースに、スマホをコントローラーとしてつなぐ中継。
//   GET /            スマホのコントローラー画面（phone.html）
//   GET /qr?r=1234   参加用 URL の QR コード（0/1 の行。PC の中継が取ってゲーム画面に描く）
//   GET /ws?room=1234&role=host            友達の PC（phone-relay.ps1）
//   GET /ws?room=1234&role=phone&name=...  スマホ
// 部屋ごとに Durable Object を 1 つ使う。スマホの入力は 30 回/秒の「まとめ」にして PC へ送り、
// PC から来たレースの状況（JSON）はそのまま全スマホへ配る。
import qrcode from "qrcode-generator";
import PHONE_HTML from "./phone.html";

const MAX_PHONES = 3;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const room = url.searchParams.get("room") || url.searchParams.get("r") || "";
    if (url.pathname === "/ws") {
      if (!/^\d{4}$/.test(room)) return new Response("部屋番号は 4 桁の数字", { status: 400 });
      return env.ROOM.get(env.ROOM.idFromName(room)).fetch(req);
    }
    if (url.pathname === "/qr") {
      if (!/^\d{4}$/.test(room)) return new Response("bad room", { status: 400 });
      const q = qrcode(0, "M");
      q.addData(`${url.origin}/?r=${room}`);
      q.make();
      const n = q.getModuleCount();
      const rows = [];
      for (let y = 0; y < n; y++) {
        let s = "";
        for (let x = 0; x < n; x++) s += q.isDark(y, x) ? "1" : "0";
        rows.push(s);
      }
      return new Response(rows.join("\n") + "\n", { headers: { "content-type": "text/plain; charset=utf-8" } });
    }
    if (url.pathname === "/" || url.pathname === "/index.html") {
      return new Response(PHONE_HTML, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } });
    }
    return new Response("not found", { status: 404 });
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
    this.host = null;
    this.phones = new Map();       // id -> { ws, name, steer, btn }
    this.nextId = 1;
    this.timer = null;
  }

  async fetch(req) {
    if (req.headers.get("Upgrade") !== "websocket") return new Response("websocket only", { status: 426 });
    const url = new URL(req.url);
    const role = url.searchParams.get("role");
    const pair = new WebSocketPair();
    const [client, ws] = Object.values(pair);
    ws.accept();

    if (role === "host") {
      if (this.host) { try { this.host.close(4000, "replaced"); } catch {} }
      this.host = ws;
      this.broadcast(JSON.stringify({ t: "host", on: true }));
      ws.addEventListener("message", (e) => {
        if (typeof e.data === "string" && e.data.startsWith("{")) this.broadcast(e.data);   // レースの状況をそのまま配る
      });
      const bye = () => {
        if (this.host === ws) { this.host = null; this.broadcast(JSON.stringify({ t: "host", on: false })); }
      };
      ws.addEventListener("close", bye);
      ws.addEventListener("error", bye);
    } else {
      if (this.phones.size >= MAX_PHONES) {
        ws.send(JSON.stringify({ t: "full" }));
        ws.close(4001, "full");
        return new Response(null, { status: 101, webSocket: client });
      }
      const id = this.nextId++;
      const name = (url.searchParams.get("name") || "").replace(/[\r\n\t]/g, " ").trim().slice(0, 8) || `P${id}`;
      const p = { ws, name, steer: 0, btn: 0 };
      this.phones.set(id, p);
      ws.send(JSON.stringify({ t: "hello", id, host: !!this.host }));
      ws.addEventListener("message", (e) => {
        // "steer,btn"（steer: -1..1、btn: 押した回数の累計）
        const m = /^(-?[\d.]+),(\d+)$/.exec(String(e.data));
        if (m) { p.steer = Math.max(-1, Math.min(1, parseFloat(m[1]))); p.btn = parseInt(m[2], 10) % 100000; }
      });
      const bye = () => { this.phones.delete(id); };
      ws.addEventListener("close", bye);
      ws.addEventListener("error", bye);
    }

    if (!this.timer) this.timer = setInterval(() => this.tick(), 33);
    return new Response(null, { status: 101, webSocket: client });
  }

  // PC へ全スマホの入力を 1 行ずつ: "id steer btn name"
  tick() {
    if (!this.host && this.phones.size === 0) { clearInterval(this.timer); this.timer = null; return; }
    if (!this.host) return;
    let s = "#phones\n";
    for (const [id, p] of this.phones) s += `${id} ${p.steer.toFixed(3)} ${p.btn} ${p.name}\n`;
    try { this.host.send(s); } catch {}
  }

  broadcast(text) {
    for (const p of this.phones.values()) { try { p.ws.send(text); } catch {} }
  }
}
