r"""視覚回帰の baseline(基準画像)の保管・承認。MCP 書 M9 の流儀に合わせる。

  %LOCALAPPDATA%\UnoEngine\parity\baselines\<sceneId>\
      approved\<camera>.png + <camera>.json   人が承認した基準
      pending\<camera>.png  + <camera>.json   撮ったばかりの候補(承認されるまで基準にならない)
      history.jsonl                           承認の履歴(いつ・誰が・なぜ)

ルール:
  * capture(update)は pending にしか書かない。基準を置き換えるのは approve だけで、reason(理由)が必須。
  * baseline には撮影環境のキー(GPU 名)を持たせる。違う GPU の baseline は check が skipped にする
    (GPU が違えば決定論でも画素は揃わないため。ドライバ違いは警告)。
  * 画像はリポジトリに入れない(巨大 + PUBLIC)。置き場は上の通りリポジトリの外。
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from . import paths
from .imgio import Img, load_image, save_png

_GPU_CACHE: dict | None = None


def gpu_info() -> dict:
    """{'name':..., 'driver':...}。取れなければ unknown。環境変数 PARITY_GPU_KEY で固定できる(テスト・CI 用)。"""
    global _GPU_CACHE
    env = os.environ.get("PARITY_GPU_KEY")
    if env:
        return {"name": env, "driver": os.environ.get("PARITY_GPU_DRIVER", "")}
    if _GPU_CACHE is not None:
        return _GPU_CACHE
    info = {"name": "unknown", "driver": ""}
    try:
        cmd = ["pwsh", "-NoProfile", "-NonInteractive", "-Command",
               "Get-CimInstance Win32_VideoController | Where-Object { $_.Name -notmatch 'Basic|Remote|Virtual' } | "
               "Select-Object -First 1 | ForEach-Object { $_.Name + '|' + $_.DriverVersion }"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30, encoding="utf-8", errors="replace")
        line = (r.stdout or "").strip().splitlines()[-1] if r.stdout.strip() else ""
        if "|" in line:
            n, d = line.split("|", 1)
            info = {"name": n.strip() or "unknown", "driver": d.strip()}
    except Exception:
        pass
    _GPU_CACHE = info
    return info


def _sha(png_path: Path) -> str:
    return hashlib.sha256(png_path.read_bytes()).hexdigest()


def _px_sha(path: Path) -> str:
    return hashlib.sha256(np.asarray(Image.open(path).convert("RGB")).tobytes()).hexdigest()


class BaselineStore:
    def __init__(self, root: Path | None = None):
        self.root = Path(root) if root else paths.baselines_dir()

    def scene_dir(self, scene_id: str) -> Path:
        return self.root / scene_id

    def _dir(self, scene_id: str, kind: str) -> Path:
        return self.scene_dir(scene_id) / kind

    # ── 保存 ────────────────────────────────────────────────────────────────
    def save_pending(self, scene_id: str, camera: str, png: str | Path, meta: dict | None = None) -> dict:
        d = paths.ensure(self._dir(scene_id, "pending"))
        dst = d / f"{camera}.png"
        shutil.copyfile(png, dst)
        info = gpu_info()
        m = {"scene": scene_id, "camera": camera, "sha256": _px_sha(dst), "capturedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
             "gpu": info["name"], "driver": info["driver"], **(meta or {})}
        (d / f"{camera}.json").write_text(json.dumps(m, indent=2, ensure_ascii=False), encoding="utf-8")
        return m

    def cameras(self, scene_id: str, kind: str) -> list[str]:
        d = self._dir(scene_id, kind)
        return sorted(p.stem for p in d.glob("*.png")) if d.exists() else []

    def approve(self, scene_id: str, reason: str, cameras: list[str] | None = None, approver: str = "") -> list[dict]:
        if len((reason or "").strip()) < 4:
            raise ValueError("approve には reason(承認の理由。4 文字以上)が要る。何が変わって、なぜ受け入れるかを書く")
        pend = self.cameras(scene_id, "pending")
        targets = cameras or pend
        missing = [c for c in targets if c not in pend]
        if missing:
            raise ValueError(f"pending に無いカメラ: {', '.join(missing)}(先に parity baseline update)")
        if not targets:
            raise ValueError("承認する pending が無い(先に parity baseline update)")
        ad = paths.ensure(self._dir(scene_id, "approved"))
        pd = self._dir(scene_id, "pending")
        done = []
        for c in targets:
            prev = None
            if (ad / f"{c}.json").exists():
                prev = json.loads((ad / f"{c}.json").read_text(encoding="utf-8")).get("sha256")
            meta = json.loads((pd / f"{c}.json").read_text(encoding="utf-8"))
            shutil.move(str(pd / f"{c}.png"), str(ad / f"{c}.png"))
            meta.update({"approvedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "reason": reason.strip(), "approver": approver})
            (ad / f"{c}.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")
            (pd / f"{c}.json").unlink(missing_ok=True)
            entry = {"at": meta["approvedAt"], "scene": scene_id, "camera": c, "sha256": meta["sha256"],
                     "previousSha256": prev, "reason": reason.strip(), "approver": approver}
            with open(self.scene_dir(scene_id) / "history.jsonl", "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
            done.append(entry)
        return done

    def discard_pending(self, scene_id: str) -> int:
        d = self._dir(scene_id, "pending")
        n = 0
        if d.exists():
            for p in d.glob("*"):
                p.unlink()
                n += 1
        return n

    # ── 読み出し ────────────────────────────────────────────────────────────
    def load_approved(self, scene_id: str, camera: str) -> tuple[Img, dict] | None:
        d = self._dir(scene_id, "approved")
        png = d / f"{camera}.png"
        if not png.exists():
            return None
        meta = json.loads((d / f"{camera}.json").read_text(encoding="utf-8")) if (d / f"{camera}.json").exists() else {}
        img = load_image(png, "gamma22" if meta.get("eotf") == "gamma22" else "srgb")
        return img, meta

    def history(self, scene_id: str) -> list[dict]:
        p = self.scene_dir(scene_id) / "history.jsonl"
        if not p.exists():
            return []
        return [json.loads(x) for x in p.read_text(encoding="utf-8").splitlines() if x.strip()]

    def list(self, scene_id: str | None = None) -> list[dict]:
        out = []
        scenes = [scene_id] if scene_id else sorted(p.name for p in self.root.glob("*") if p.is_dir()) if self.root.exists() else []
        for s in scenes:
            for kind in ("approved", "pending"):
                for c in self.cameras(s, kind):
                    mp = self._dir(s, kind) / f"{c}.json"
                    m = json.loads(mp.read_text(encoding="utf-8")) if mp.exists() else {}
                    out.append({"scene": s, "camera": c, "kind": kind, "gpu": m.get("gpu"), "sha256": (m.get("sha256") or "")[:12],
                                "capturedAt": m.get("capturedAt"), "approvedAt": m.get("approvedAt"), "reason": m.get("reason")})
        return out


def compatibility(meta: dict) -> tuple[str, str | None]:
    """baseline の撮影環境が今と合うか。('ok'|'skip'|'warn', 理由)。GPU 名が違えば skip、ドライバ違いは warn。"""
    cur = gpu_info()
    if meta.get("gpu") and cur["name"] != "unknown" and meta["gpu"] != cur["name"]:
        return "skip", f"baseline は別の GPU で撮られている({meta['gpu']} / 今は {cur['name']})。画素が揃わないので判定しない(撮り直して approve)"
    if meta.get("driver") and cur["driver"] and meta["driver"] != cur["driver"]:
        return "warn", f"ドライバが baseline と違う({meta['driver']} -> {cur['driver']})。1〜2 LSB の揺れは許容内かもしれない"
    return "ok", None
