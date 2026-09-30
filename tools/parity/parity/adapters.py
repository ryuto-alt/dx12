"""基準画像の供給アダプタ(差し替え可能)。

  pt            エンジン内の DXR パストレーサー(MCP `render_reference`)で作る。無ければ skipped(理由つき)
  external      人が手動で撮った画像(UE / Dreamcore のスクショ等)を references/ から読む
  previous-run  承認済み baseline(視覚回帰)

どれも get(camera) -> RefResult を返す。基準が用意できないときは img=None + skip_reason(合格扱いにしない)。

render_reference の呼び出し規約は 2 通り(reference.pt.api で選ぶ):
  "engine"(既定。実エンジンの MCP `render_reference` = 非同期 API。Q2 で実結合した):
    request : {spp, seed, output:"<基準パス。拡張子なし>", formats:["pfm"], size:[w,h], ...reference.pt.params}
              size は撮影解像度(engine.size か、エンジン画像の大きさ)。frameBudgetMs の既定は無人実行向けに 40。
              カメラは呼ぶ直前に set_editor_camera 済み(今のエディタカメラ。垂直 FOV は 45°)。
    result  : {accepted:true, state:"requested"} を即座に返す → render_reference_status を state:"done" までポーリング
              → output.files から .pfm / .exr を読む。
  "legacy"(Q1 が仮定した同期 API。偽エンジンと旧仕様のため残してある):
    request : {spp, seed, path:"<絶対パス .pfm/.exr>", ...reference.pt.params}(sizeParams = ["width","height"] で大きさを渡す)
    result  : {path, width, height, spp?}
出力は線形 RGB(Rec.709 原色・露出 1.0 ・トーンマップ前)の float。PFM 推奨。
"""
from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import baseline as B
from . import paths
from .engine import EngineClient, EngineError
from .imgio import Img, load_image
from .spec import Spec

EXTERNAL_EXT = (".png", ".exr", ".pfm", ".hdr", ".jpg", ".jpeg", ".tif", ".tiff")


@dataclass
class RefResult:
    img: Img | None
    source: str
    meta: dict = field(default_factory=dict)
    align: dict = field(default_factory=dict)        # tonemap / exposure の上書き(外部基準)
    noise_imgs: list = field(default_factory=list)   # 同じ設定で別シードの画像(ノイズ床用)
    skip_reason: str | None = None
    warnings: list = field(default_factory=list)


class PtAdapter:
    name = "pt"

    def __init__(self, spec: Spec, engine: EngineClient | None, workdir: Path, want_noise: bool = False):
        self.spec, self.engine, self.workdir, self.want_noise = spec, engine, Path(workdir), want_noise
        self._has: bool | None = None

    def available(self) -> tuple[bool, str | None]:
        pt = self.spec.reference.get("pt") or {}
        method = pt.get("method", "render_reference")
        if self.engine is None:
            return False, "エンジンに接続していない"
        if self._has is None:
            self._has = self.engine.has_method(method)
        if not self._has:
            return False, (f"このエンジンには MCP method '{method}' が無い(パストレーサー未マージ)。"
                           "reference.kind を external にして手動撮影の基準を置くか、PT のマージ後に実行する")
        return True, None

    def get(self, camera: dict, test_size: tuple[int, int] | None = None) -> RefResult:
        ok, why = self.available()
        if not ok:
            return RefResult(None, "pt", skip_reason=why)
        pt = self.spec.reference.get("pt") or {}
        method = pt.get("method", "render_reference")
        fmt = pt.get("format", "pfm")
        api = pt.get("api", "engine")
        seeds = pt.get("seeds") or [1]
        imgs: list[Img] = []
        metas = []
        use = seeds if self.want_noise else seeds[:1]
        for seed in use:
            base = (self.workdir / f"pt_{camera['name']}_s{seed}").resolve()
            out = base.with_suffix("." + fmt)
            out.parent.mkdir(parents=True, exist_ok=True)
            try:
                if api == "legacy":
                    params = {"seed": int(seed), "path": str(out)}
                    if pt.get("spp"):
                        params["spp"] = int(pt["spp"])
                    sp = pt.get("sizeParams")               # 例 ["width","height"]: エンジン画像と同じ大きさで撮らせる
                    if sp and test_size:
                        params[sp[0]], params[sp[1]] = int(test_size[0]), int(test_size[1])
                    params.update(pt.get("params") or {})
                    r = self.engine.call(method, params, timeout=float(pt.get("timeoutSec", 1800)))
                    p = Path(r.get("path") or r.get("file") or out)
                    spp_done = r.get("spp", pt.get("spp"))
                else:
                    p, spp_done, r = self._render_engine_api(method, pt, seed, base, fmt, test_size)
            except EngineError as e:
                return RefResult(None, "pt", skip_reason=f"{method} が失敗: {e}")
            except RuntimeError as e:
                return RefResult(None, "pt", skip_reason=str(e))
            img = load_image(p, "auto" if fmt != "png" else "srgb")
            imgs.append(img)
            metas.append({"seed": seed, "spp": spp_done, "file": str(p)})
        return RefResult(imgs[0], "pt", meta={"renders": metas, "method": method, "api": api}, noise_imgs=imgs[1:])

    def _render_engine_api(self, method: str, pt: dict, seed: int, base: Path, fmt: str, test_size):
        """実エンジンの render_reference(非同期)を呼んで state:"done" まで待ち、書かれたファイルを返す。"""
        params: dict = {"seed": int(seed), "output": str(base), "formats": [fmt], "frameBudgetMs": 40}
        if pt.get("spp"):
            params["spp"] = int(pt["spp"])
        size = self.spec.engine.get("size") or (list(test_size) if test_size else None)
        if size:
            params["size"] = [int(size[0]), int(size[1])]
        params.update(pt.get("params") or {})
        timeout = float(pt.get("timeoutSec", 1800))
        r = self.engine.call(method, params, timeout=120)
        if r.get("accepted") is False:
            raise RuntimeError(f"{method} が受理されなかった: {r}")
        t0 = time.time()
        poll = float(pt.get("pollSec", 1.0))
        last: dict = {}
        while True:
            last = self.engine.call("render_reference_status", {}, timeout=60)
            st = last.get("state")
            if st == "done":
                break
            if st in ("failed", "cancelled"):
                raise RuntimeError(f"{method} が {st}: {last.get('error') or last.get('progress')}")
            if time.time() - t0 > timeout:
                try:
                    self.engine.call("render_reference_cancel", {}, timeout=30)
                except EngineError:
                    pass
                raise RuntimeError(f"{method} が {timeout:g} 秒で終わらない(state={st}, samples={last.get('samples')})")
            time.sleep(poll)
        files = (last.get("output") or {}).get("files") or []
        want = "." + fmt
        for f in files:
            if str(f).lower().endswith(want):
                spp_done = (last.get("samples") or {}).get("done", (last.get("output") or {}).get("sppDone"))
                return Path(f), spp_done, last
        raise RuntimeError(f"{method} の出力に {want} が無い: {files}")


class ExternalAdapter:
    """手動で撮った基準画像。<dir>/<camera>.png(または camera.reference で名前指定)。<name>.json に来歴と整列の指定を書ける。"""
    name = "external"

    def __init__(self, spec: Spec, override_dir: Path | None = None):
        self.spec = spec
        self.override_dir = override_dir

    def dirs(self) -> list[Path]:
        ex = self.spec.reference.get("external") or {}
        out: list[Path] = []
        if self.override_dir:
            out.append(Path(self.override_dir))
        if ex.get("dir"):
            d = Path(ex["dir"])
            out.append(d if d.is_absolute() else self.spec.base_dir / d)
        out.append(paths.references_dir() / self.spec.id)
        return out

    def find(self, camera: dict) -> Path | None:
        names = [camera["reference"]] if camera.get("reference") else [camera["name"] + e for e in EXTERNAL_EXT]
        for d in self.dirs():
            for n in names:
                p = d / n
                if p.exists():
                    return p
        return None

    def get(self, camera: dict, test_size=None) -> RefResult:
        p = self.find(camera)
        if p is None:
            return RefResult(None, "external", skip_reason=(
                f"外部基準が無い: {camera['name']}(探した場所: " + " / ".join(str(d) for d in self.dirs()) + ")。"
                "UE 等で撮ったスクリーンショットを置く(docs/PARITY_HARNESS.md の撮り方)"))
        ex = self.spec.reference.get("external") or {}
        side = p.with_suffix(".json")
        sc = json.loads(side.read_text(encoding="utf-8")) if side.exists() else {}
        cs = sc.get("colorspace") or ex.get("colorspace") or "auto"
        img = load_image(p, cs)
        align = {}
        tm = sc.get("tonemap") or ex.get("tonemap")
        exp = sc.get("exposure", ex.get("exposure"))
        if tm:
            align["tonemap"] = tm
        if exp is not None:
            align["exposure"] = exp
        return RefResult(img, "external", meta={"file": str(p), "provenance": sc.get("provenance") or ex.get("provenance"),
                                                "sidecar": sc or None}, align=align)


class PreviousRunAdapter:
    name = "previous-run"

    def __init__(self, spec: Spec, store: B.BaselineStore | None = None):
        self.spec = spec
        self.store = store or B.BaselineStore()

    def get(self, camera: dict, test_size=None) -> RefResult:
        got = self.store.load_approved(self.spec.id, camera["name"])
        if got is None:
            return RefResult(None, "previous-run", skip_reason=(
                f"承認済み baseline が無い: {self.spec.id}/{camera['name']}"
                "(parity baseline update → parity baseline approve --reason ...)"))
        img, meta = got
        state, why = B.compatibility(meta)
        if state == "skip":
            return RefResult(None, "previous-run", skip_reason=why)
        warns = [why] if why else []
        return RefResult(img, "previous-run", meta={"baseline": meta}, warnings=warns)


def make_adapter(kind: str, spec: Spec, engine: EngineClient | None, workdir: Path, *, want_noise: bool = False,
                 external_dir: Path | None = None, store: B.BaselineStore | None = None):
    if kind == "pt":
        return PtAdapter(spec, engine, workdir, want_noise)
    if kind == "external":
        return ExternalAdapter(spec, external_dir)
    if kind == "previous-run":
        return PreviousRunAdapter(spec, store)
    raise ValueError(f"基準の種別が不明: {kind}(pt / external / previous-run)")
