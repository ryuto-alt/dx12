#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Graphite Gauge のトークンから C++ ヘッダを生成する（W1-A2 / GG-1）。標準ライブラリだけ。

  python -I tools/theme/gen_gg_tokens.py            生成（src/editor/ThemeGraphite.gen.h / UiMotionTokens.gen.h を書く）
  python -I tools/theme/gen_gg_tokens.py --check    生成物が最新か・スナップショットの SHA256 が lock と一致するか・lock の
                                                    integrate_check が全緑かを検査する（ctest の GgTokensGenCheck が呼ぶ）

入力は assets/editor/theme/gg/tokens.json（= final/tokens.json のスナップショット。tools/theme/sync_gg_spec.py が作る）だけ。
生成物は手で直さない。tokens に数値として無いフィールドは作らない。
トークンの `cpp` 欄（Palette.xxx / NEW Palette.xxx）と生成するフィールド名は、ここで機械的に突き合わせる
（仕様の改名や追加で黙って値が落ちないように、対応の無いトークンはエラーにする）。
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GG_DIR = ROOT / "assets" / "editor" / "theme" / "gg"
OUT_THEME = ROOT / "src" / "editor" / "ThemeGraphite.gen.h"
OUT_MOTION = ROOT / "src" / "editor" / "UiMotionTokens.gen.h"

# 変数名（ThemeGraphite.gen.h の値）。フィールドへのポインタの表（kGgFieldMap / kGgContrast）が使う
STRUCT_VAR = {"Palette": "kGraphitePalette", "GgStatus": "kStatus", "GgStatusCvd": "kStatusCvd", "GgAxis": "kAxis", "GgKind": "kKind",
              "GgViewportPalette": "kViewport", "GgTimelinePalette": "kTimeline", "GgChrome": "kChrome"}


class Tracked(dict):
    """読んだキーを覚える dict。生成に使われなかったキー（= 仕様に増えたのに黙って落ちるキー）をエラーにするために使う。"""
    def __init__(self, d):
        super().__init__(d)
        self.used = set()

    def __getitem__(self, k):
        self.used.add(k)
        return super().__getitem__(k)

    def unused(self):
        return sorted(set(self.keys()) - self.used)


def sync_module():
    """sync_gg_spec.py の check_green を共有する（-I だとスクリプトのフォルダが sys.path に無いので、ファイルから読む）。"""
    sys.dont_write_bytecode = True   # tools/theme に __pycache__ を作らない
    spec = importlib.util.spec_from_file_location("sync_gg_spec", pathlib.Path(__file__).resolve().parent / "sync_gg_spec.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

# ThemeVariants.h の Palette の宣言順（既存フィールド。ここに無い名前の `Palette.xxx` はエラー）
PALETTE_FIELDS = """bg0 bg1 bg2 bg3 bg4 windowBg popupBg inputBg inputBgHover inputBgActive inputBorder inputBorderHover
border borderStrong separator accent accentHover accentPressed onAccent selection selectionHover selectionActive
tabOverline tabOverlineDim text textMid textDim textFaint good warn bad
typeMesh typeLight typeCamera typeAudio typeScript typePhysics typeUi typePrefab typeEmpty typeFolder typeScene
btnActive scrollGrab scrollGrabHover scrollGrabActive tableBorderLight tableRowAlt treeLines
textSelectedBg dockingPreview separatorHover resizeGripHover""".split()

COLOR_GROUPS = ["surface", "text", "accent", "selection", "status", "statusCvd", "axis", "kind",
                "viewport", "timeline", "play", "overlay", "gauge"]


def lf(b):
    return b.replace(bytes([13, 10]), bytes([10]))


def sha256_file(p):
    return hashlib.sha256(lf(pathlib.Path(p).read_bytes())).hexdigest()


def first_lower(s):
    return s[:1].lower() + s[1:]


def cpp_name(cpp):
    """`cpp` 欄から (種別, 名前, NEW か) を取り出す。別名（= で始まる）は None。"""
    c = cpp.strip()
    if c.startswith("="):
        return None
    new = c.startswith("NEW")
    m = re.match(r"(?:NEW\s+)?(Palette\(cvd\)|Palette|Deco)\.(\w+)", c)
    if m:
        return (m.group(1), m.group(2), new)
    return ("other", None, new)


def parse_hex(v):
    m = re.fullmatch(r"#([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?", v)
    if not m:
        raise SystemExit("色の形式が不正: %r" % v)
    rgb = int(m.group(1), 16)
    a = int(m.group(2), 16) if m.group(2) else 255
    return rgb, a


def cvec(v):
    rgb, a = parse_hex(v)
    if a == 255:
        return "C(0x%06X)" % rgb
    return "C(0x%06X, %d)" % (rgb, a)


def fnum(x):
    """float リテラル（最短の表現）。"""
    if isinstance(x, bool):
        raise SystemExit("bool は fnum に渡さない")
    if float(x) == int(x):
        return "%d.0f" % int(x)
    return repr(float(x)) + "f"


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# ---------------------------------------------------------------- 色の分類
def classify_colors(tok):
    """色トークンを 出力先ごとに振り分ける。戻り: dict[struct名] = list[(field, tokenpath, value)]、alias の一覧"""
    dest = {k: [] for k in ["Palette", "GgStatus", "GgStatusCvd", "GgAxis", "GgKind", "GgViewportPalette",
                            "GgTimelinePalette", "GgChrome"]}
    aliases = []   # (path, cpp)
    fmap = []      # (path, struct, field, cpp)
    name2f = {}    # cpp 欄の名前（Palette.xxx の xxx）→ (struct, field)
    seen = {}
    for g in COLOR_GROUPS:
        for key, t in tok["color"][g].items():
            path = "color.%s.%s" % (g, key)
            info = cpp_name(t["cpp"])
            if info is None:
                aliases.append((path, t["cpp"]))
                continue
            kind, name, _new = info
            val = t["value"]
            if kind == "Palette" and name in PALETTE_FIELDS:
                struct, field = "Palette", name
            elif g == "statusCvd" or kind == "Palette(cvd)":
                struct, field = "GgStatusCvd", name
            elif g == "status":
                struct, field = "GgStatus", name if name else key
            elif g == "axis":
                struct, field = "GgAxis", first_lower(name[4:]) if name and name.startswith("axis") else key
            elif g == "kind":
                struct, field = "GgKind", first_lower(name[4:]) if name and name.startswith("type") else key
            elif g == "viewport":
                struct, field = "GgViewportPalette", first_lower(name[2:]) if name and name.startswith("vp") else key
            elif g == "timeline":
                struct, field = "GgTimelinePalette", first_lower(name[2:]) if name and name.startswith("tl") else key
            elif kind == "Palette" and name in PALETTE_FIELDS:
                struct, field = "Palette", name
            else:
                struct, field = "GgChrome", (name if name and kind != "Deco" or key == "shadowNear" or key == "shadowFar" else key)
            # 既存 Palette に無い Palette.xxx（NEW でない）は仕様側の不整合
            if kind == "Palette" and not _new and struct != "Palette" and g in ("surface", "text", "accent", "selection"):
                raise SystemExit("既存の Palette に無いフィールド: %s (%s)" % (name, path))
            if kind == "Deco" and g != "overlay":
                raise SystemExit("想定外の Deco 参照: %s" % path)
            k = (struct, field)
            if k in seen:
                raise SystemExit("フィールド名が重複: %s.%s (%s と %s)" % (struct, field, seen[k], path))
            seen[k] = path
            dest[struct].append((field, path, val))
            fmap.append((path, struct, field, t["cpp"]))
            if name and kind != "Palette(cvd)":   # 別名の参照先に cvd 用の色（同名の good / warn ...）を取り違えない
                name2f[name] = (struct, field)
    # 別名（`= Palette.xxx` / `= NEW Palette.xxx`）は参照先のフィールドを指す。参照先が無ければエラー
    for path, cpp in aliases:
        m = re.match(r"=\s*(?:NEW\s+)?(?:Palette|Deco)\.(\w+)", cpp.strip())
        if not m or m.group(1) not in name2f:
            raise SystemExit("別名の参照先が見つからない: %s (%s)" % (path, cpp))
        struct, field = name2f[m.group(1)]
        fmap.append((path, struct, field, cpp))
    return dest, aliases, fmap


def emit_struct(name, fields):
    out = ["struct %s" % name, "{"]
    for f, _p, _v in fields:
        out.append("    ImVec4 %s;" % f)
    out.append("};")
    return "\n".join(out)


def emit_fill(fn, struct, fields, extra_lines=()):
    out = ["inline constexpr %s %s = []() constexpr {" % (struct, fn), "    %s s{};" % struct]
    for f, _p, v in fields:
        out.append("    s.%s = %s;" % (f, cvec(v)))
    for l in extra_lines:
        out.append("    " + l)
    out += ["    return s;", "}();"]
    return "\n".join(out)


# ---------------------------------------------------------------- 文字列からの数値の取り出し（取れなければ止める）
def rx(pattern, text, what, cast=float, group=1):
    m = re.search(pattern, text)
    if not m:
        raise SystemExit("tokens から %s を取り出せません（仕様の書式が変わった。gen_gg_tokens.py を見直す）: /%s/" % (what, pattern))
    return cast(m.group(group))


def split_top(s, sep):
    """括弧の外の sep で分割する。"""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "（(":
            depth += 1
        elif ch in "）)":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return [o for o in out if o]


def dash_pair(s, what):
    m = re.fullmatch(r"(\d+)/(\d+)", s)
    if not m:
        raise SystemExit("%s の書式が不正: %r" % (what, s))
    return int(m.group(1)), int(m.group(2))


# ---------------------------------------------------------------- contrast.md（final の表）
def parse_expr(e):
    """`A` または `A ⊕ B`（A を B の上に合成）→ (path, 下の面の path or None)。"""
    parts = [x.strip() for x in e.split("⊕")]
    if len(parts) > 2 or not all(re.fullmatch(r"[A-Za-z]+\.[A-Za-z0-9]+", x) for x in parts):
        raise SystemExit("contrast.md の式が不正: %r" % e)
    return ("color." + parts[0], ("color." + parts[1]) if len(parts) == 2 else None)


def parse_contrast(text):
    """contrast.md の §1（マトリクス）・§2（文字）・§3（UI 境界）を (fg, fgOver, bg, bgOver, min, docRatio, isText, forbidden) の一覧にする。"""
    m = re.search(r"文字\s*(\d+)\s*組・UI 境界\s*(\d+)\s*組", text)
    if not m:
        raise SystemExit("contrast.md の「文字 N 組・UI 境界 M 組」の行が見つからない（書式が変わった）")
    want_text, want_ui = int(m.group(1)), int(m.group(2))
    secs = re.split(r"^## ", text, flags=re.M)
    sec = {}
    for b in secs[1:]:
        mm = re.match(r"(\d+)\.", b)
        if mm:
            sec[int(mm.group(1))] = b
    for k in (1, 2, 3):
        if k not in sec:
            raise SystemExit("contrast.md に §%d が無い" % k)
    pairs = []
    # §1: 文字 4 段 x 面
    rows = [l for l in sec[1].splitlines() if l.startswith("|")]
    head = [c.strip() for c in rows[0].strip("|").split("|")][1:]
    surf = {}
    for h in head:
        surf[h] = ("selection." + h) if h in ("sel", "selHover", "selActive") else ("surface." + h)
    n1 = 0
    for r in rows[2:]:
        cells = [c.strip() for c in r.strip("|").split("|")]
        mt = re.match(r"(t[1-4])(?![0-9])", cells[0])
        if not mt:
            raise SystemExit("contrast.md §1 の行が不正: %r" % r)
        if len(cells) - 1 != len(head):
            raise SystemExit("contrast.md §1 の列数が合わない: %r" % r)
        for h, c in zip(head, cells[1:]):
            forb = "~~" in c or "禁止" in c
            mr = re.search(r"([0-9]+\.[0-9]+)", c)
            if not mr:
                raise SystemExit("contrast.md §1 の値が不正: %r" % c)
            pairs.append(("color.text." + mt.group(1), None, "color." + surf[h], None, 4.5, float(mr.group(1)), True, forb))
            n1 += 1
    # §2 / §3
    for no, is_text in ((2, True), (3, False)):
        for l in sec[no].splitlines():
            if not l.startswith("|") or l.startswith("|--") or l.startswith("| 前景"):
                continue
            c = [x.strip() for x in l.strip("|").split("|")]
            if len(c) < 6:
                raise SystemExit("contrast.md §%d の行が不正: %r" % (no, l))
            fg, fo = parse_expr(c[0])
            bg, bo = parse_expr(c[1])
            ratio, mn = float(c[3]), float(c[4])
            if mn != (4.5 if is_text else 3.0):
                raise SystemExit("contrast.md §%d の基準が想定外: %r" % (no, l))
            if c[5] != "OK":
                raise SystemExit("contrast.md に OK でない行がある: %r" % l)
            pairs.append((fg, fo, bg, bo, mn, ratio, is_text, False))
    nt = sum(1 for p in pairs if p[6])
    nu = sum(1 for p in pairs if not p[6])
    if (nt, nu) != (want_text, want_ui):
        raise SystemExit("contrast.md の組の数が見出しの数と合わない: 文字 %d / UI %d（見出し: %d / %d）" % (nt, nu, want_text, want_ui))
    return pairs


# ---------------------------------------------------------------- ThemeGraphite.gen.h
def gen_theme(tok, lock_sha, contrast_text):
    dest, aliases, fmap = classify_colors(tok)
    pal = dict((f, v) for f, _p, v in dest["Palette"])
    missing = [f for f in PALETTE_FIELDS if f not in pal and f != "windowBg"]
    if missing:
        raise SystemExit("tokens に対応の無い Palette のフィールド: %s" % missing)
    # windowBg は bg1（cpp 欄「Palette.bg1 (= windowBg)」）
    if "windowBg" not in pal:
        pal["windowBg"] = pal["bg1"]
    pal_fields = [(f, "", pal[f]) for f in PALETTE_FIELDS]

    L = []
    A = L.append
    A("// ★自動生成: tools/theme/gen_gg_tokens.py（入力 assets/editor/theme/gg/tokens.json、SHA256 = %s）。手で直さない。" % lock_sha)
    A("// Graphite Gauge v%s のトークン。これを include してよいのは ThemeGraphite.cpp と、値を直接読むテスト・描画の .cpp だけ" % tok["meta"]["version"])
    A("// （EditorTheme.h / ThemeVariants.h などの重ヘッダから include しない。仕様の再生成で再コンパイルされる TU を最小に保つため）。")
    A("#pragma once")
    A("")
    A('#include "editor/ThemeVariants.h"')
    A("")
    A("namespace dx12e::theme::gg")
    A("{")
    A("")
    A("namespace detail")
    A("{")
    A("// 0xRRGGBB と alpha（0..255）→ ImVec4（ThemeVariants.h の Hex() と同じ計算）")
    A("constexpr ImVec4 C(unsigned int rgb, unsigned int a = 255u)")
    A("{")
    A("    return ImVec4(((rgb >> 16) & 0xFFu) / 255.0f, ((rgb >> 8) & 0xFFu) / 255.0f, (rgb & 0xFFu) / 255.0f, a / 255.0f);")
    A("}")
    A("} // namespace detail")
    A("")
    A("namespace detail_gen")
    A("{")
    A("using detail::C;")
    A("")
    for st in ["GgStatus", "GgStatusCvd", "GgAxis", "GgKind", "GgViewportPalette", "GgTimelinePalette", "GgChrome"]:
        pass
    A("} // namespace detail_gen")
    A("")
    # 構造体
    A("// ---- 色（tokens.color.* の `cpp` 欄の NEW 行。フィールド名は cpp 欄の名前から機械的に決める）----")
    A(emit_struct("GgViewportPalette", dest["GgViewportPalette"]))
    A(emit_struct("GgTimelinePalette", dest["GgTimelinePalette"]))
    A(emit_struct("GgStatus", dest["GgStatus"]))
    A(emit_struct("GgStatusCvd", dest["GgStatusCvd"]))
    A(emit_struct("GgAxis", dest["GgAxis"]))
    A(emit_struct("GgKind", dest["GgKind"]))
    A("// 面・文字・アクセント・再生・暗幕・目盛りのうち、既存の Palette に無いもの")
    A(emit_struct("GgChrome", dest["GgChrome"]))
    A("")

    # GgDeco / GgLayout / 線
    deco = Tracked(tok["deco"])
    layout = tok["metric"]["layout"]
    comp = tok["metric"]["component"]
    line = tok["line"]

    deco_fields = []   # (名前, 型, リテラル)

    def D(name, key, typ="int"):
        v = deco[key]["value"]
        deco_fields.append((name, typ, fnum(v) if typ == "float" else str(int(v))))

    D("scaleMinorStep", "scaleMinorStep"); D("scaleMinorH", "scaleMinorH"); D("scaleMajorStep", "scaleMajorStep"); D("scaleMajorH", "scaleMajorH")
    D("scaleMinorAlpha", "scaleMinorAlpha", "float"); D("scaleMajorAlpha", "scaleMajorAlpha", "float")
    D("caliperJawW", "caliperJawW"); D("caliperJawH", "caliperJawH")
    D("tomboSelArm", "tomboSelArm"); D("tomboSelOffset", "tomboSelOffset"); D("tomboTileArm", "tomboTileArm"); D("tomboTileSecondaryArm", "tomboTileSecondaryArm")
    D("hatchAngle", "hatchAngle"); D("hatchStep", "hatchStep"); D("hatchAlpha", "hatchAlpha", "float")
    D("vernierFineStep", "vernierFineStep"); D("vernierMajorStep", "vernierMajorStep")
    D("vernierMinorAlpha", "vernierMinorAlpha", "float"); D("vernierMajorAlpha", "vernierMajorAlpha", "float")
    D("progressStep", "progressStep"); D("dockGripW", "dockGripW"); D("dockGripH", "dockGripH"); D("dockHitPad", "dockHitPad")
    D("badBarW", "badBarW"); D("playTagH", "playTagH"); D("playTagPadX", "playTagPadX")
    rd, bd = Tracked(tok["metric"]["rounding"]), Tracked(tok["metric"]["border"])
    # Metrics（ImGui の StyleVar）に入らない角丸（ボタン / ピル / 再生タグ）は GgDeco で渡す（A7 が数値を持たなくて済むように）
    deco_fields += [("roundButton", "float", fnum(rd["button"]["value"])), ("roundPill", "float", fnum(rd["pill"]["value"])),
                    ("roundPlayTag", "float", fnum(rd["playTag"]["value"]))]
    wl, wg = dash_pair(deco["warnBarDash"]["value"], "deco.warnBarDash")
    pl, pg = dash_pair(deco["playDash"]["value"], "deco.playDash")
    deco_fields += [("warnBarDashLen", "int", str(wl)), ("warnBarDashGap", "int", str(wg)),
                    ("playDashLen", "int", str(pl)), ("playDashGap", "int", str(pg))]
    # 列挙値は既知のものだけ（未知の値を黙って Plain にしない）。ThemeVariants.h の HeaderStyle は Plain だけ、RowStyle の NeedleBar は GgDeco::rowNeedleBar で表す
    if deco["headerStyle"]["value"] != "Plain":
        raise SystemExit("deco.headerStyle の値 %r は未対応（ThemeVariants.h の HeaderStyle に Plain 以外を足す変更は重ヘッダの追加承認が要る）" % deco["headerStyle"]["value"])
    if deco["rowStyle"]["value"] not in ("NeedleBar", "Plain"):
        raise SystemExit("deco.rowStyle の値 %r は未対応" % deco["rowStyle"]["value"])
    deco_fields += [("caliper", "bool", "true" if deco["caliper"]["value"] else "false"),
                    ("headerOverline", "bool", "true" if deco["headerOverline"]["value"] else "false"),
                    ("rowNeedleBar", "bool", "true" if deco["rowStyle"]["value"] == "NeedleBar" else "false")]
    # 線幅の規則（line.*）の数値
    deco_fields += [("hairMinPx", "int", str(rx(r"max\((\d+)", line["hairPx"]["rule"], "line.hairPx", int))),
                    ("accentMinPx", "int", str(rx(r"max\((\d+)", line["accentPx"]["rule"], "line.accentPx", int))),
                    ("accentMulPx", "int", str(rx(r"round\((\d+)\*scale", line["accentPx"]["rule"], "line.accentPx", int))),
                    ("playFrameMinPx", "int", str(rx(r"max\((\d+)", line["playFramePx"]["rule"], "line.playFramePx", int))),
                    ("playFrameMulPx", "int", str(rx(r"round\((\d+)\*scale", line["playFramePx"]["rule"], "line.playFramePx", int))),
                    ("tickAlphaScale", "float", fnum(rx(r"×([\d.]+)", line["tickAlphaScale"]["rule"], "line.tickAlphaScale")))]
    A("// ---- 装飾の寸法（tokens.deco.* / tokens.line.*。論理 px は ui::Px を通して使う）----")
    A("struct GgDeco")
    A("{")
    for n, ty, lit in deco_fields:
        A("    %s %s = %s;" % (ty, n, lit))
    A("};")
    A("")
    lay = []
    for k, v in layout.items():
        lay.append(("layout_" + k, int(v["value"])))
    for k, v in comp.items():
        lay.append(("comp_" + k, int(v["value"])))
    A("// ---- レイアウト寸法（tokens.metric.layout / component。論理 px）----")
    A("struct GgLayout")
    A("{")
    for n, v in lay:
        A("    int %s = %d;" % (n[7:] if n.startswith("layout_") else n[5:], v))
    A("};")
    A("")

    # 黄の予算
    yb = tok["yellowBudget"]
    n_lines = len(yb["lines"])
    edit_lines = rx(r"=\s*線\s*(\d+)\s*箇所", yb["screenCap"]["worstCase"], "yellowBudget.screenCap.worstCase の線の数", int)
    tests = " ".join(yb["tests"])
    edit_ratio = rx(r"編集モード\s*≤\s*([\d.]+)\s*%", tests, "編集モードの黄の画素比")
    play_ratio = rx(r"再生モード\s*≤\s*([\d.]+)\s*%", tests, "再生モードの黄の画素比")
    edit_fills = rx(r"黄の塗り矩形[^。]*?が\s*(\d+)\s*個", tests, "編集モードの黄の面の数", int)
    boot_items = split_top(yb["boot"][0]["where"].split("（『")[0], "/")
    zone = yb["yellowZone"]
    A("// ---- 黄の予算（tokens.yellowBudget。テストが突き合わせる）----")
    A("struct GgYellowBudget")
    A("{")
    A("    int   editLinesMax = %d;          // 編集中に同時に見える黄の線の最大数（予算 01〜08 の最悪ケース）" % edit_lines)
    A("    int   budgetLineCount = %d;       // 予算表の線の番号の数（deco::Needle* / Tombo* / Caliper の呼び出し箇所数に一致させる）" % n_lines)
    A("    int   fillCount = %d;             // 予算表の黄の面の番号の数（F1 再生ボタン、F2 再生タグ）" % len(yb["fills"]))
    A("    float editPixelRatioMax = %s;    // 編集モードの黄の画素比の上限（%%。OKLCH で C>=%s かつ h in [%g,%g]）" % (fnum(edit_ratio), zone["oklchChromaMin"], zone["hueMin"], zone["hueMax"]))
    A("    float playPixelRatioMax = %s;    // 再生モードの黄の画素比の上限（%%）" % fnum(play_ratio))
    A("    int   editFillsMax = %d;          // 編集モードで許す黄の塗り矩形の数（面積 16px2 以上かつ短辺 4px 以上）" % edit_fills)
    A("    float zoneChromaMin = %s;" % fnum(zone["oklchChromaMin"]))
    A("    float zoneHueMin = %s;" % fnum(zone["hueMin"]))
    A("    float zoneHueMax = %s;" % fnum(zone["hueMax"]))
    A("    int   bootAllowedCount = %d;      // 起動画面で黄を許す要素の数（下の kBootYellowAllowed）" % len(boot_items))
    A("};")
    A("inline constexpr const char* kBootYellowAllowed[] = {")
    for b in boot_items:
        A("    %s," % cstr(b))
    A("};")
    A("")

    # 本体
    A("struct GgSpec")
    A("{")
    A("    GgViewportPalette viewport;")
    A("    GgTimelinePalette timeline;")
    A("    GgStatus status;")
    A("    GgStatusCvd statusCvd;")
    A("    GgAxis axis;")
    A("    GgKind kind;")
    A("    GgChrome chrome;")
    A("    GgDeco deco;")
    A("    GgLayout layout;")
    A("    GgYellowBudget budget;")
    A("};")
    A("")
    A("using detail::C;")
    A("")
    A("// ---- 値 ----")
    A(emit_fill("kGraphitePalette", "Palette", pal_fields))
    A(emit_fill("kViewport", "GgViewportPalette", dest["GgViewportPalette"]))
    A(emit_fill("kTimeline", "GgTimelinePalette", dest["GgTimelinePalette"]))
    A(emit_fill("kStatus", "GgStatus", dest["GgStatus"]))
    A(emit_fill("kStatusCvd", "GgStatusCvd", dest["GgStatusCvd"]))
    A(emit_fill("kAxis", "GgAxis", dest["GgAxis"]))
    A(emit_fill("kKind", "GgKind", dest["GgKind"]))
    A(emit_fill("kChrome", "GgChrome", dest["GgChrome"]))
    A("")
    A("inline constexpr GgSpec kGgSpec{ kViewport, kTimeline, kStatus, kStatusCvd, kAxis, kKind, kChrome, GgDeco{}, GgLayout{}, GgYellowBudget{} };")
    A("")

    # トークン → フィールドの表（テストが「全トークンが cpp 欄どおりのフィールドに入っているか」を見る）
    ptr = {}
    for path, struct, field, cpp in fmap:
        ptr[path] = "&%s.%s" % (STRUCT_VAR[struct], field)
    A("// ---- トークン → 生成フィールドの対応表（全色トークン。別名は参照先のフィールドを指す）----")
    A("struct GgFieldRef { const char* path; const char* cpp; const char* member; const ImVec4* ptr; bool alias; };")
    A("inline constexpr GgFieldRef kGgFieldMap[] = {")
    alias_paths = set(a for a, _c in aliases)
    for path, struct, field, cpp in fmap:
        A("    { %s, %s, %s, %s, %s }," % (cstr(path), cstr(cpp), cstr("%s::%s" % (struct, field)), ptr[path],
                                          "true" if path in alias_paths else "false"))
    A("};")
    A("")
    # コントラスト（final/contrast.md の §1〜§3 の全組。A ⊕ B = A を B の上に合成した色）
    pairs = parse_contrast(contrast_text)

    def pe(path):
        if path is None:
            return "nullptr"
        if path not in ptr:
            raise SystemExit("contrast.md のトークンが tokens に無い: %s" % path)
        return ptr[path]

    def nm(path, over):
        return cstr((path[6:] if path else "") + ((" ⊕ " + over[6:]) if over else ""))
    A("// ---- コントラスト検査の表（final/contrast.md §1〜§3 と同じ組。文字 4.5:1 / UI 境界 3:1）----")
    A("// fgOver / bgOver が空でなければ「fg を fgOver の上に合成した色」。docRatio は contrast.md の比（2 桁）。forbidden は「禁止の組み合わせ」（比は基準未満で正しい）。")
    A("struct GgContrastPair { const ImVec4* fg; const ImVec4* fgOver; const ImVec4* bg; const ImVec4* bgOver; const char* fgName; const char* bgName;")
    A("                        float minRatio; float docRatio; bool text; bool forbidden; };")
    A("inline constexpr GgContrastPair kGgContrast[] = {")
    for fg, fo, bg, bo, mn, ratio, is_text, forb in pairs:
        A("    { %s, %s, %s, %s, %s, %s, %s, %s, %s, %s }," % (pe(fg), pe(fo), pe(bg), pe(bo), nm(fg, fo), nm(bg, bo), fnum(mn),
                                                          fnum(ratio) if ratio == int(ratio) else repr(ratio) + "f",
                                                          "true" if is_text else "false", "true" if forb else "false"))
    A("};")
    A("")

    # Metrics / Deco(旧) / Extra
    sc = tok["typography"]["scale"]
    A("// ---- Metrics / Deco / Extra（ThemeVariants.h の既存の型へ）----")
    A("inline constexpr Metrics kMetrics = []() constexpr {")
    A("    Metrics m{};")
    for f, k in [("windowRounding", "window"), ("childRounding", "child"), ("frameRounding", "frame"), ("grabRounding", "grab"),
                 ("popupRounding", "popup"), ("tabRounding", "tab"), ("scrollbarRounding", "scrollbar")]:
        A("    m.%s = %s;" % (f, fnum(rd[k]["value"])))
    for f, k in [("windowBorder", "window"), ("childBorder", "child"), ("popupBorder", "popup"), ("frameBorder", "frame"),
                 ("tabBarBorder", "tabBar"), ("tabOverline", "tabOverline"), ("dockSeparator", "dockSeparator")]:
        A("    m.%s = %s;" % (f, fnum(bd[k]["value"])))
    A("    m.fontBase = %s;   // typography.scale.body.imguiPx" % fnum(sc["body"]["imguiPx"]))
    A("    m.fontMono = %s;   // typography.scale.mono.imguiPx" % fnum(sc["mono"]["imguiPx"]))
    A("    return m;")
    A("}();")
    A("")
    rgb, a = parse_hex(deco["backdropTop"]["value"])
    rgb2, a2 = parse_hex(deco["backdropBottom"]["value"])
    pr = tok["color"]["overlay"]["popupInnerTop"]["value"]
    A("// ★RowStyle::NeedleBar（= 4）は ThemeVariants.h の列挙子に無い（重ヘッダの形は A2a の 1 回だけ）。旧型の rowStyle は Plain のままにして、")
    A("//   NeedleBar かどうかは GgDeco::rowNeedleBar で表す。")
    A("inline constexpr Deco kDeco = []() constexpr {")
    A("    Deco d{};")
    for f in ["glow", "hoverTint", "easeSec", "shadowStrength", "rimAlpha", "popupRim"]:
        A("    d.%s = %s;" % (f, fnum(deco[f]["value"])))
    for f in ["panelFocusEdge", "panelTopLine", "panelSheen", "layeredShadow", "monoIcons", "statusTopLine", "toolbarLine"]:
        A("    d.%s = %s;" % (f, "true" if deco[f]["value"] else "false"))
    A("    d.rowStyle = RowStyle::Plain;")
    A("    d.headerStyle = HeaderStyle::Plain;   // deco.headerStyle == \"Plain\"（上で検査済み）")
    A("    d.backdropTop = C(0x%06X, %d);" % (rgb, a))
    A("    d.backdropBottom = C(0x%06X, %d);" % (rgb2, a2))
    A("    return d;")
    A("}();")
    A("// deco.popupRim（tokens.deco）。color.overlay.popupInnerTop の alpha との一致は tests/gg_tokens_gen_test.cpp が見る。")
    A("static_assert(kDeco.popupRim > 0.0f);")
    A("inline constexpr Extra kExtra = []() constexpr {")
    A("    Extra x{};")
    A("    x.needleDim = kChrome.accentDim;")
    A("    x.inLine = kChrome.inLine;")
    A("    x.info = kStatus.info;")
    A("    x.warnText = kGraphitePalette.warn;   // status.warnText = Palette.warn（別名）")
    A("    x.badText = kStatus.badText;")
    A("    x.axisX = kAxis.x; x.axisY = kAxis.y; x.axisZ = kAxis.z;")
    A("    return x;")
    A("}();")
    for nmx, tr in (("deco", deco), ("metric.rounding", rd), ("metric.border", bd)):
        un = tr.unused()
        if un:
            raise SystemExit("tokens の %s に生成されないキーがある（仕様に項目が増えた。gen_gg_tokens.py に足す）: %s" % (nmx, un))
    L2 = L
    L2.append("")
    L2.append("// 組み立て済みの Spec の部品（ThemeGraphite.cpp が Spec と gg::GgSpec をまとめる）。gg::kGgSpec の deco / layout / budget は")
    L2.append("// 既定値の初期化子（GgDeco{} など）が tokens の値を持つ。")
    L2.append("")
    L2.append("} // namespace dx12e::theme::gg")
    L2.append("")
    text = "\n".join(L2)
    # 空の detail_gen 名前空間は不要
    text = text.replace("namespace detail_gen\n{\nusing detail::C;\n\n} // namespace detail_gen\n\n", "")
    return text


# ---------------------------------------------------------------- UiMotionTokens.gen.h
def parse_bezier(s):
    m = re.fullmatch(r"cubic-bezier\(\s*([\d.\-]+)\s*,\s*([\d.\-]+)\s*,\s*([\d.\-]+)\s*,\s*([\d.\-]+)\s*\)", s)
    if not m:
        raise SystemExit("cubic-bezier の書式が不正: %r" % s)
    return [float(m.group(i)) for i in range(1, 5)]


def camel_const(name):
    return "k" + name[:1].upper() + name[1:]


def gen_motion(tok, lock_sha):
    mo = tok["motion"]
    L = []
    A = L.append
    A("// ★自動生成: tools/theme/gen_gg_tokens.py（入力 assets/editor/theme/gg/tokens.json、SHA256 = %s）。手で直さない。" % lock_sha)
    A("// Graphite Gauge のモーショントークン（tokens.motion）。曲線と時間の数値はここが正で、C++ に数値を書かない（ui::anim が読む）。")
    A("#pragma once")
    A("")
    A("namespace dx12e::ui::motion_tokens")
    A("{")
    A("")
    A("// cubic-bezier(x1, y1, x2, y2)（CSS と同じ。t は 0..1）")
    A("struct CubicBezier { float x1, y1, x2, y2; };")
    A("enum class Curve : int { Standard, Exit, Linear, Expo, Spring, Stepped };")
    A("")
    cur = mo["curves"]
    for key, nm in [("standard", "kStandard"), ("exit", "kExit"), ("expo", "kExpo")]:
        b = parse_bezier(cur[key]["value"])
        A("inline constexpr CubicBezier %s{ %s, %s, %s, %s };" % (nm, fnum(b[0]) if b[0] == int(b[0]) else repr(b[0]) + "f",
                                                                   fnum(b[1]) if b[1] == int(b[1]) else repr(b[1]) + "f",
                                                                   fnum(b[2]) if b[2] == int(b[2]) else repr(b[2]) + "f",
                                                                   fnum(b[3]) if b[3] == int(b[3]) else repr(b[3]) + "f"))
    A("inline constexpr CubicBezier kLinear{ 0.0f, 0.0f, 1.0f, 1.0f };   // 値だけ残す（UI では使わない）")
    omega = rx(r"ω=([\d.]+)", cur["spring"]["value"], "motion.curves.spring")
    A("inline constexpr float kSpringOmega = %s;   // 臨界減衰の角周波数（起動画面の指針 / Tween の行き先変更）" % fnum(omega))
    px = rx(r"steps\((\d+)px\)", cur["stepped"]["value"], "motion.curves.stepped")
    steps = rx(r"×\s*(\d+)\s*段", cur["stepped"]["note"], "motion.curves.stepped の段数", int)
    A("inline constexpr float kSteppedPx = %s;     // 1 段の幅" % fnum(px))
    A("inline constexpr int   kSteppedSteps = %d;" % steps)
    A("// 残りがこの px 未満になったら終点へスナップする（standard の note）")
    A("inline constexpr float kSnapPx = %s;" % fnum(rx(r"残り\s*([\d.]+)px", cur["standard"]["note"], "standard のスナップ量")))
    A("")
    A("// ---- 時間の段（ms）----")
    for k, v in mo["scaleMs"].items():
        A("inline constexpr float k%s = %s;" % (k[:1].upper() + k[1:], fnum(v["value"])))
    A("")
    A("struct Dur { float ms; Curve curve; };")
    A("// ---- 用途ごとの時間と曲線 ----")
    cm = {"standard": "Standard", "exit": "Exit", "linear": "Linear", "expo": "Expo", "spring": "Spring", "stepped": "Stepped"}
    for k, v in mo["durationsMs"].items():
        A("inline constexpr Dur %s{ %s, Curve::%s };   // %s" % (camel_const(k), fnum(v["value"]), cm[v["curve"]], v["note"].split("。")[0].replace("*/", "")))
    A("")
    A("// ---- 待ち（Reduce Motion でも残す）----")
    for k, v in mo["waitsMs"].items():
        A("inline constexpr float %sMs = %s;" % (camel_const(k), fnum(v["value"])))
    A("")
    rm = mo["reduceMotion"]
    A("// Reduce Motion: 動きの時間を 0 にする。待ち・stepped の刻み・状態の手がかり・音は残す。")
    A("inline constexpr float kReduceMotionDurationMs = %s;" % fnum(rm["durationsMs"]))
    A("")
    A("} // namespace dx12e::ui::motion_tokens")
    A("")
    return "\n".join(L)


# ---------------------------------------------------------------- 入口
def load():
    tok_path = GG_DIR / "tokens.json"
    lock_path = GG_DIR / "gg_spec.lock.json"
    tok = json.loads(tok_path.read_text(encoding="utf-8"))
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    return tok, lock, sha256_file(tok_path)


def read_norm(p):
    try:
        return lf(pathlib.Path(p).read_bytes()).decode("utf-8")
    except FileNotFoundError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="生成物が最新か / lock と一致か / integrate_check が全緑かを検査する")
    args = ap.parse_args()
    tok, lock, sha = load()
    errs = []
    if lock.get("tokensSha256") != sha:
        errs.append("tokens.json の SHA256 が gg_spec.lock.json と一致しない（手で書き換えた？ sync_gg_spec.py を再実行する）")
    ic = lock.get("integrateCheck", {})
    res = ic.get("result", {})
    bad = sync_module().check_green(res)   # 未知の項目も合否として扱う（sync_gg_spec.py と同じ判定）
    if ic.get("allGreen") is not True or bad:
        errs.append("lock の integrate_check が全緑でない: %s" % bad)
    if lock.get("metaVersion") != tok["meta"]["version"]:
        errs.append("lock の metaVersion が tokens と違う")
    contrast_text = (GG_DIR / "contrast.md").read_bytes().replace(bytes([13, 10]), bytes([10])).decode("utf-8")
    csha = sha256_file(GG_DIR / "contrast.md")
    if lock.get("contrastSha256") != csha:
        errs.append("contrast.md の SHA256 が gg_spec.lock.json と一致しない（sync_gg_spec.py を再実行する）")
    theme = gen_theme(tok, sha, contrast_text)
    motion = gen_motion(tok, sha)
    if args.check:
        for p, txt in [(OUT_THEME, theme), (OUT_MOTION, motion)]:
            cur = read_norm(p)
            if cur != txt:
                errs.append("%s が最新でない（python -I tools/theme/gen_gg_tokens.py で再生成する）" % p.name)
        if errs:
            for e in errs:
                print("NG: " + e, file=sys.stderr)
            return 1
        print("OK: gg tokens v%s sha256=%s 生成物は最新" % (tok["meta"]["version"], sha[:16]))
        return 0
    if errs:
        for e in errs:
            print("NG: " + e, file=sys.stderr)
        return 1
    for p, txt in [(OUT_THEME, theme), (OUT_MOTION, motion)]:
        p.write_text(txt, encoding="utf-8", newline="\n")
        print("書き込み: %s" % p.relative_to(ROOT).as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
