import net from "node:net";

// エディタ(C++)の TCP ブリッジへ改行区切り JSON を送り、id で応答を相関させる薄いクライアント。
// 遅延接続＋切断時は次回呼び出しで再接続。単一接続で十分(ponytail)。
export class EngineClient {
  private sock: net.Socket | null = null;
  private connecting: Promise<net.Socket> | null = null;
  private buf = "";
  private nextId = 1;
  private pending = new Map<number, { resolve: (v: any) => void; reject: (e: Error) => void }>();
  private host: string;
  private port: number;
  private timeoutMs: number;

  // Node の型ストリップ実行はパラメータプロパティ非対応なので明示代入。
  constructor(host?: string, port?: number, timeoutMs = 10000) {
    this.host = host ?? process.env.DX12_MCP_HOST ?? "127.0.0.1";
    this.port = port ?? Number(process.env.DX12_MCP_PORT ?? "8787");
    this.timeoutMs = timeoutMs;
  }

  private failAll(e: Error) {
    this.sock = null;
    this.connecting = null;
    this.buf = "";   // 切断時の受信途中バッファは無効。残すと再接続後の最初の応答が連結で壊れる。
    for (const p of this.pending.values()) p.reject(e);
    this.pending.clear();
  }

  private onData(d: string) {
    this.buf += d;
    let i: number;
    while ((i = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      let msg: any;
      try { msg = JSON.parse(line); } catch { continue; }
      const p = this.pending.get(msg.id);
      if (p) { this.pending.delete(msg.id); p.resolve(msg); }
    }
  }

  private connect(): Promise<net.Socket> {
    if (this.sock && !this.sock.destroyed) return Promise.resolve(this.sock);
    // single-flight: 接続確立中の Promise を共有。並行 call が複数ソケットを張るのを防ぐ
    // (engine は単一クライアントしか捌けないため2本目以降がハングする)。
    if (this.connecting) return this.connecting;
    this.connecting = new Promise((resolve, reject) => {
      const s = net.connect(this.port, this.host);
      s.setEncoding("utf8");
      s.on("data", (d: string) => this.onData(d));
      s.on("error", (e: Error) => this.failAll(e));
      s.on("close", () => this.failAll(new Error("engine connection closed")));
      s.once("connect", () => { this.sock = s; this.connecting = null; resolve(s); });
      s.once("error", (e: Error) => {
        this.connecting = null;
        reject(new Error(`エディタに繋がらへん (${this.host}:${this.port}) — エディタ起動してる? : ${e.message}`));
      });
    });
    return this.connecting;
  }

  // method を呼んで result を返す。engine が ok:false なら error を throw。
  async call(method: string, params: Record<string, unknown>): Promise<any> {
    const s = await this.connect();
    const id = this.nextId++;
    const msg: any = await new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      s.write(JSON.stringify({ id, method, params }) + "\n", (err) => {
        if (err) { this.pending.delete(id); reject(err); }
      });
      setTimeout(() => {
        if (this.pending.has(id)) { this.pending.delete(id); reject(new Error("engine timeout")); }
      }, this.timeoutMs);
    });
    if (msg.ok === false) throw new Error(msg.error || "engine error");
    return msg.result ?? null;
  }
}
