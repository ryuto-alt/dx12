"""エンジン(MCP ブリッジ)との接続と起動。

★安全ルール(dx12-ui-audit/AGENT_RULES.md):
  * エンジンは tools/engine_instance.ps1 でだけ起動する(exe をコピーして --background / headless で起動。
    build\\release\\DX12Engine.exe を直接起動してはいけない)。止めるのも同じスクリプトの -Stop(自分の名前だけ)。
  * 操作・撮影は MCP の仮想入力系だけ。OS の入力・前面化は一切しない。
  * ポートは既定 8820 / 名前 par(ランナーの動作確認用に割り当て)。
エンジンのブリッジは「改行区切り JSON・単一クライアント」。接続は 1 本だけ張って全部そこで済ませる。
"""
from __future__ import annotations

import json
import socket
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path

from .paths import REPO_ROOT

DEFAULT_PORT = 8820
DEFAULT_NAME = "par"


class EngineError(Exception):
    def __init__(self, msg: str, code=None, hint: str | None = None, method: str | None = None):
        super().__init__(msg)
        self.code, self.hint, self.method = code, hint, method


class EngineClient:
    """行 JSON の TCP クライアント(id で応答を突き合わせる。遅延応答も同じ id で返る)。"""

    def __init__(self, port: int, host: str = "127.0.0.1", connect_timeout: float = 30.0):
        self.port, self.host = port, host
        self._seq = 0
        self._buf = b""
        deadline = time.time() + connect_timeout
        last: Exception | None = None
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=5)
                break
            except OSError as e:
                last = e
                if time.time() > deadline:
                    raise EngineError(f"エンジン(port {port})に接続できない: {last}") from e
                time.sleep(0.4)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def call(self, method: str, params: dict | None = None, timeout: float = 180.0) -> dict:
        self._seq += 1
        rid = self._seq
        payload = json.dumps({"id": rid, "method": method, "params": params or {}}, ensure_ascii=False) + "\n"
        self.sock.settimeout(timeout)
        try:
            self.sock.sendall(payload.encode("utf-8"))
            while True:
                while b"\n" not in self._buf:
                    chunk = self.sock.recv(1 << 16)
                    if not chunk:
                        raise EngineError(f"{method}: エンジンが接続を閉じた(クラッシュの可能性)", method=method)
                    self._buf += chunk
                line, self._buf = self._buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line.decode("utf-8"))
                except json.JSONDecodeError:
                    continue
                if msg.get("id") != rid:
                    continue                      # 別の id(遅延応答の取りこぼし等)は読み捨てる
                if msg.get("ok") is False:
                    raise EngineError(f"{method}: {msg.get('error')}", code=msg.get("error_code"),
                                      hint=msg.get("error_hint"), method=method)
                return msg.get("result") or {}
        except socket.timeout as e:
            raise EngineError(f"{method}: {timeout:g} 秒待っても応答が無い", method=method) from e

    def has_method(self, name: str) -> bool:
        try:
            self.call("describe_mcp_manifest", {"method": name, "brief": True}, timeout=30)
            return True
        except EngineError as e:
            if e.code == 8 or "unknown method" in str(e):
                return False
            raise


def wait_ping(cli: EngineClient, timeout: float = 240.0) -> dict:
    """ping が返るまで待つ(起動直後はロード中で遅い)。"""
    deadline = time.time() + timeout
    last: Exception | None = None
    while time.time() < deadline:
        try:
            return cli.call("ping", timeout=20)
        except EngineError as e:
            last = e
            time.sleep(1.0)
    raise EngineError(f"ping が {timeout:g} 秒で返らない: {last}")


# ── 起動 ────────────────────────────────────────────────────────────────────

def _ps_quote(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


class PwshLauncher:
    """tools/engine_instance.ps1 経由の起動/停止。"""

    def __init__(self, name: str = DEFAULT_NAME, port: int = DEFAULT_PORT, mode: str = "headless",
                 script: Path | None = None, build_dir: str | None = None):
        self.name, self.port, self.mode = name, port, mode
        self.script = script or (REPO_ROOT / "tools" / "engine_instance.ps1")
        self.build_dir = build_dir

    def _run(self, arglist: str, timeout: float = 180.0) -> subprocess.CompletedProcess:
        cmd = ["pwsh", "-NoProfile", "-NonInteractive", "-Command", f"& {_ps_quote(str(self.script))} {arglist}"]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")

    def start(self, project: str | None, extra_args: list[str] | None = None) -> dict:
        self.stop()                                   # 自分の名前だけ止める(前回の異常終了の残り)
        parts = [f"-Name {_ps_quote(self.name)}", f"-Port {int(self.port)}", f"-Mode {self.mode}"]
        if project:
            parts.append(f"-Project {_ps_quote(str(project))}")
        if self.build_dir:
            parts.append(f"-BuildDir {_ps_quote(self.build_dir)}")
        if extra_args:
            parts.append("-ExtraArgs @(" + ",".join(_ps_quote(a) for a in extra_args) + ")")
        r = self._run(" ".join(parts))
        if r.returncode != 0:
            raise EngineError(f"engine_instance.ps1 の起動に失敗(exit {r.returncode}): {(r.stderr or r.stdout).strip()[:500]}")
        try:
            return json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return {"name": self.name, "port": self.port, "raw": r.stdout.strip()[:300]}

    def stop(self) -> None:
        try:
            self._run(f"-Name {_ps_quote(self.name)} -Stop", timeout=60)
        except Exception:
            pass


class NoLauncher:
    """接続だけ(--attach)。起動も停止もしない。"""

    def start(self, project, extra_args=None) -> dict:
        return {"attached": True}

    def stop(self) -> None:
        pass


@contextmanager
def engine_session(port: int = DEFAULT_PORT, launcher=None, project: str | None = None, extra_args: list[str] | None = None,
                   attach: bool = False, ping_timeout: float = 240.0, log=lambda m: None):
    """with engine_session(...) as (client, ping)。抜けるときは必ず接続を閉じ、自分が起動したなら停止する。"""
    launcher = NoLauncher() if attach else (launcher or PwshLauncher(port=port))
    client: EngineClient | None = None
    started = False
    try:
        if not attach:
            log(f"エンジンを起動: port {port}")
            launcher.start(project, extra_args)
            started = True
        client = EngineClient(port, connect_timeout=(30 if attach else 120))
        ping = wait_ping(client, ping_timeout)
        yield client, ping
    finally:
        if client is not None:
            client.close()
        if started:
            launcher.stop()
