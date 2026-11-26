#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""UI 書体のサブセット化と検査（W1-A1 / GG-2a）。

入力（元ファイルは巨大なのでリポジトリに入れない）:
  IBM Plex Sans JP / Sans / Mono  (SIL OFL 1.1)   -> third_party/fonts/ibm-plex/subset/*.ttf
  PlemolJP Regular                (SIL OFL 1.1)   -> third_party/fonts/plemoljp/subset/*.ttf
                                                   -> assets/editor/splash/fonts/splash_subset.ttf（起動画面専用）
  Noto Sans JP Regular（リポジトリ内 assets/editor/fonts の既存サブセットが元）
                                                 -> third_party/fonts/noto-sans-jp/subset/*.ttf
  元ファイルの URL と SHA256 は third_party/fonts/fonts.lock.json（唯一の正）。

使い方（python -I で実行する。標準ライブラリだけで動くのは --check）:
  python -I tools/subset_fonts.py --check               生成物・字の被覆・名前表の検査（標準ライブラリだけ。ctest が呼ぶ）
  python -I tools/subset_fonts.py --check --src <dir>   さらに <dir> の元ファイルから作り直して、バイト一致を確かめる（fonttools が要る）
  python -I tools/subset_fonts.py --fetch  --src <dir>  元ファイルを取得して SHA256 を確かめ <dir> に平らに置く（約 550MB を一時取得）
  python -I tools/subset_fonts.py --build  --src <dir>  サブセットを作って third_party/ へ書き、fonts.lock.json の出力欄を更新

fonttools は MIT。`python -m venv` した環境へ `pip install fonttools` で入れる（リポジトリには入れない）。

OFL の予約フォント名（RFN）: サブセットは OFL の「改変版」なので、RFN を名前表に残してはならない。
  IBM Plex = "Plex"、PlemolJP = "PlemolJP"（Hack 由来の部分は Bitstream Vera License の "Bitstream" / "Vera"）、Noto Sans JP = "Source"。
  そこで IBM Plex と PlemolJP 由来の全出力の名前表を付け替える:
  Uno UI Sans JP / Uno UI Sans / Uno UI Mono / Uno UI Mono JP / Uno Splash Mono。
  Noto の予備は名前（RFN に当たらない）を "Noto Sans JP" Regular（重さ 400）へ正規化する（元は "Thin" の名前が残っていた）。
  PlemolJP 由来の 2 出力は、Plex Mono / Plex Sans JP が持つコードポイントだけに絞る（plex_only）。PlemolJP が Hack（Bitstream Vera License）から
  借りている記号の字形を入れないため（許可ライセンスの外）。--check が「PlemolJP 由来の収録字 ⊆ Plex 由来の出力」を検査する。
  著作権・ライセンスの欄は元のまま。--check は全出力の nameID 1/4/6/16/21 に RFN が無いことを検査する。
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "third_party", "fonts", "fonts.lock.json")
CHARSET_UI = os.path.join(ROOT, "third_party", "fonts", "charset_ui.txt")
CHARSET_SPLASH = os.path.join(ROOT, "third_party", "fonts", "charset_splash.txt")
EXTRA_CHARS = os.path.join(ROOT, "third_party", "fonts", "extra_chars.txt")
ALLOW_FALLBACK = os.path.join(ROOT, "third_party", "fonts", "charset_system_fallback.txt")

# OFL / Bitstream Vera の Reserved Font Name。改変版（サブセット）の名前表（nameID 1/4/6/16/21）に残してはならない
RESERVED_NAMES = ("plex", "plemoljp", "source", "bitstream", "vera")

# --- 出力する書体の定義 -------------------------------------------------------------------------------------
# id: 元ファイルの id（lock の sources）、out: 出力先、charset: ui / splash、rename: 名前表の付け替え先
FONTS = [
    dict(src="plexjp-regular", out="third_party/fonts/ibm-plex/subset/UnoUISansJP-Regular.ttf", charset="ui",
         rename=("Uno UI Sans JP", "Regular")),
    dict(src="plexjp-medium", out="third_party/fonts/ibm-plex/subset/UnoUISansJP-Medium.ttf", charset="ui",
         rename=("Uno UI Sans JP", "Medium")),
    dict(src="plexjp-semibold", out="third_party/fonts/ibm-plex/subset/UnoUISansJP-SemiBold.ttf", charset="ui",
         rename=("Uno UI Sans JP", "SemiBold")),
    dict(src="plexsans-regular", out="third_party/fonts/ibm-plex/subset/UnoUISans-Regular.ttf", charset="ui",
         rename=("Uno UI Sans", "Regular")),
    dict(src="plexsans-medium", out="third_party/fonts/ibm-plex/subset/UnoUISans-Medium.ttf", charset="ui",
         rename=("Uno UI Sans", "Medium")),
    dict(src="plexsans-semibold", out="third_party/fonts/ibm-plex/subset/UnoUISans-SemiBold.ttf", charset="ui",
         rename=("Uno UI Sans", "SemiBold")),
    dict(src="plexmono-regular", out="third_party/fonts/ibm-plex/subset/UnoUIMono-Regular.ttf", charset="ui",
         rename=("Uno UI Mono", "Regular")),
    dict(src="plexmono-medium", out="third_party/fonts/ibm-plex/subset/UnoUIMono-Medium.ttf", charset="ui",
         rename=("Uno UI Mono", "Medium")),
    # 重さ 300 の等幅（GG tokens: gaugeNum / gaugeReadout / timecodeLarge）
    dict(src="plexmono-light", out="third_party/fonts/ibm-plex/subset/UnoUIMono-Light.ttf", charset="ui",
         rename=("Uno UI Mono", "Light")),
    dict(src="plemol-regular", out="third_party/fonts/plemoljp/subset/UnoUIMonoJP-Regular.ttf", charset="ui",
         rename=("Uno UI Mono JP", "Regular"), plex_only=True),
    dict(src="noto-regular", out="third_party/fonts/noto-sans-jp/subset/NotoSansJP-Regular.ttf", charset="ui",
         rename=("Noto Sans JP", "Regular"), weight=400),
    # 起動画面専用（DirectWrite の InMemoryLoader で読む。等幅。字は工程の文言・Tips・図番だけ。数十 KB）
    # ％（GG tokens は mono 300）と日本語の工程名を 1 ファイルで等幅に組むため PlemolJP Light を使う
    dict(src="plemol-light", out="assets/editor/splash/fonts/splash_subset.ttf", charset="splash",
         rename=("Uno Splash Mono", "Light"), plex_only=True,
         # 起動画面は ％ の桁揃え（tnum）以外のレイアウト機能を使わない
         features=["kern", "tnum", "zero"]),
]
# plex_only: PlemolJP の字のうち、IBM Plex Mono か IBM Plex Sans JP が持つコードポイントだけを残す。
# PlemolJP は Plex に無い記号（矢印・数学記号・図形・Powerline など）を Hack（元は Bitstream Vera Sans Mono）から借りており、
# その字形は Bitstream Vera License（許可ライセンスの外）になる。Plex が持つ字だけに絞れば Hack 由来の字形は入らない。
# 絞られた字は Plex 系の書体（Uno UI Sans JP / Uno UI Mono）かシステムの書体へ縮退する。
UI_FEATURES = ["kern", "liga", "calt", "tnum", "zero", "locl", "vert", "vrt2"]

LIMIT_TOTAL = 12 * 1024 * 1024
LIMIT_SPLASH = 200 * 1024

# --- 字の集合 ---------------------------------------------------------------------------------------------
# ソースから UI 文字列に使われている非 ASCII 字を抜き出す対象
SCAN_DIRS = ["src"]
SCAN_EXT = (".cpp", ".h", ".hpp", ".inc", ".cxx", ".cc", ".ixx")
SPLASH_GLOBS = ("Splash",)   # src/**/Splash* の文字列リテラルから起動画面の字を作る


def jis_x0208_chars():
    """JIS X 0208 の全字（記号・かな・第 1・第 2 水準）。"""
    out = set()
    for row in range(1, 95):
        for cell in range(1, 95):
            try:
                ch = bytes([0xA0 + row, 0xA0 + cell]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ch)
    return out


def range_chars(*ranges):
    s = set()
    for a, b in ranges:
        s.update(chr(c) for c in range(a, b + 1))
    return s


def base_chars():
    """どの書体にも入れる基本の字（ASCII、Latin-1、記号、かな、全角形、矢印、罫線）。書体に無い字は subsetter が落とす。"""
    return (range_chars((0x20, 0x7E), (0xA0, 0xFF), (0x2010, 0x2027), (0x2030, 0x203E), (0x2070, 0x209F),
                        (0x20A0, 0x20BF), (0x2100, 0x214F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF),
                        (0x2500, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF), (0x3000, 0x303F), (0x3040, 0x309F),
                        (0x30A0, 0x30FF), (0xFF01, 0xFF5E), (0xFF61, 0xFF9F), (0xFFE0, 0xFFE6))
            | jis_x0208_chars())


_ESC = re.compile(r'\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8})|.)', re.S)


def _literal_chars(body):
    out = set()
    pos = 0
    for m in _ESC.finditer(body):
        out.update(c for c in body[pos:m.start()] if ord(c) > 0x7F)
        pos = m.end()
        h = m.group(1) or m.group(2)
        if h:
            try:
                c = chr(int(h, 16))
            except ValueError:
                continue
            if ord(c) > 0x7F:
                out.add(c)
    out.update(c for c in body[pos:] if ord(c) > 0x7F)
    return out


def scan_cpp_literals(text):
    """C++ のソースから、文字列リテラル（通常・u8/u/U/L 接頭辞・生文字列）の中の非 ASCII 字を集める。
    コメントと文字リテラルは読み飛ばす。完全な字句解析ではないが、UI 文字列の抜き出しには十分。"""
    out = set()
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            # 行末の \ で続く行コメントがあるが、日本語コメントでそれを使う箇所は無い前提で素直に読む
            i = n if j < 0 else j + 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            # 生文字列か（直前が R / u8R / LR など）
            k = i - 1
            raw = k >= 0 and text[k] == 'R' and (k == 0 or not (text[k - 1].isalnum() or text[k - 1] == '_')
                                                 or text[max(0, k - 2):k] in ("u8",) or text[k - 1] in "uUL")
            if raw:
                m = re.match(r'"([^()\\ \t\r\n]{0,16})\(', text[i:i + 24])
                if m:
                    term = ')' + m.group(1) + '"'
                    j = text.find(term, i + m.end())
                    if j >= 0:
                        out.update(ch for ch in text[i + m.end():j] if ord(ch) > 0x7F)
                        i = j + len(term)
                        continue
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out |= _literal_chars(text[i + 1:j])
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            # 桁区切り（1'000）と文字リテラルを区別せず、文字リテラルの中身も UI 文字として拾っておく（無害）
            out |= _literal_chars(text[i + 1:j])
            i = j + 1
        else:
            i += 1
    return out


def read_text(path):
    with open(path, "rb") as f:
        data = f.read()
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        # 文字化け（U+FFFD）を「必要な字」に混ぜると、的外れな「書体に無い」エラーになる。読めないファイルを名指しして止める
        print("NG UTF-8 として読めないファイルがあります: %s（%s）。UTF-8 で保存し直してください" % (os.path.relpath(path, ROOT), e))
        sys.exit(1)


def scan_ui_chars(path_filter=None):
    out = set()
    for d in SCAN_DIRS:
        for dp, _dn, fns in os.walk(os.path.join(ROOT, d)):
            for fn in fns:
                if not fn.endswith(SCAN_EXT):
                    continue
                if path_filter and not path_filter(fn):
                    continue
                out |= scan_cpp_literals(read_text(os.path.join(dp, fn)))
    return out


def extra_chars(section="ui"):
    """extra_chars.txt の字。[ui]（既定）と [splash] の節に分かれ、# で始まる行は無視する。"""
    out = set()
    cur = "ui"
    if os.path.exists(EXTRA_CHARS):
        for line in read_text(EXTRA_CHARS).splitlines():
            if line.startswith("#"):
                continue
            m = re.fullmatch(r"\[(\w+)\]\s*", line)
            if m:
                cur = m.group(1)
                continue
            if cur == section:
                out.update(c for c in line if c not in "\r\n\t")
    return out


def required_ui_chars():
    return scan_ui_chars() | extra_chars("ui")


def required_splash_chars():
    # 起動画面: ASCII + src/**/Splash* の文字列リテラル + 図番・工程名の追加分（extra の [splash] 節は使わず、共通の extra を使う）
    s = set(chr(c) for c in range(0x20, 0x7F))
    s |= scan_ui_chars(lambda fn: fn.startswith(SPLASH_GLOBS))
    # 図番 UN-x.y.z と表題欄の記号
    s |= set("0123456789.-_:/%·×°→") | set("UN")
    # 起動画面の仕様（GG の boot_impl_spec）の工程名・新規プロジェクトの流れなど: extra_chars.txt の [splash] 節
    s |= extra_chars("splash")
    return s


def charset_text(chars):
    return "".join(sorted(chars, key=ord)) + "\n"


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def load_lock():
    with open(LOCK, encoding="utf-8") as f:
        return json.load(f)


def save_lock(lock):
    with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
        json.dump(lock, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


# --- サブセット化（fonttools が要る）------------------------------------------------------------------------
def _name_rename(font, family, style, weight=None):
    """OFL の予約名（Plex / PlemolJP / Source）を含む名前を外す。著作権(0)・ライセンス(13,14)・製造元(8,9)・URL(11,12)は元のまま。"""
    name = font["name"]
    ps = (family + "-" + style).replace(" ", "")
    unique = "UnoUI;" + ps + ";subset"
    # RIBBI 以外（Light / Medium / SemiBold）は、旧式の名前（1/2）を「家族 + 重さ」/ Regular にし、
    # 家族と重さそのものは 16/17 に置く（GDI など家族＋サブファミリ名で選ぶ読み込み側で、重さの違う面が混ざらないように）
    ribbi = style in ("Regular", "Bold", "Italic", "Bold Italic")
    legacy_family = family if ribbi else family + " " + style
    legacy_style = style if ribbi else "Regular"
    for rec in list(name.names):
        i = rec.nameID
        if i == 1:
            rec.string = legacy_family
        elif i == 2:
            rec.string = legacy_style
        elif i == 16:
            rec.string = family
        elif i == 17:
            rec.string = style
        elif i == 4:
            rec.string = family + " " + style
        elif i == 3:
            rec.string = unique
        elif i == 6:
            rec.string = ps
        elif i in (7, 10, 18, 19, 20, 21, 22, 25):
            name.names.remove(rec)   # 商標名・説明・互換名など、旧名を含みうる欄は外す
    if not ribbi:
        name.removeNames(nameID=16)
        name.removeNames(nameID=17)
        for pid, eid, lid in ((3, 1, 0x409), (1, 0, 0)):
            name.setName(family, 16, pid, eid, lid)
            name.setName(style, 17, pid, eid, lid)
    if weight is not None and "OS/2" in font:
        font["OS/2"].usWeightClass = weight
    return font


def build_one(spec, src_path, chars):
    from fontTools import subset as fsub
    from fontTools.ttLib import TTFont
    opts = fsub.Options()
    opts.hinting = False                      # 描画は ImGui の FreeType（LightHinting。TT 命令を使わない）
    opts.desubroutinize = True
    opts.notdef_outline = True
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.layout_features = spec.get("features", UI_FEATURES)
    opts.glyph_names = False
    opts.legacy_kern = True
    opts.recalc_timestamp = False
    opts.recalc_bounds = True
    opts.drop_tables = list(opts.drop_tables) + ["DSIG"]
    font = TTFont(src_path, recalcTimestamp=False)
    ss = fsub.Subsetter(opts)
    ss.populate(unicodes=[ord(c) for c in chars])
    if spec.get("plex_only"):
        # fonttools は要求した字の「双方向の鏡像の字」（∋ に対する ∌ など）まで勝手に足す。その字形は Plex に無い（Hack 由来）ので止める
        orig_mirror = fsub._cmap_add_bidi_mirroring
        fsub._cmap_add_bidi_mirroring = lambda _s: None
        try:
            ss.subset(font)
        finally:
            fsub._cmap_add_bidi_mirroring = orig_mirror
    else:
        ss.subset(font)
    if spec.get("rename"):
        _name_rename(font, *spec["rename"], weight=spec.get("weight"))
    font.save_path = None
    return font


def font_bytes(font):
    import io
    buf = io.BytesIO()
    font.save(buf, reorderTables=True)
    return buf.getvalue()


def find_source(src_dir, member):
    base = os.path.basename(member)
    for dp, _dn, fns in os.walk(src_dir):
        if base in fns:
            return os.path.join(dp, base)
    return None


def require_fonttools(lock):
    try:
        import fontTools
    except ImportError:
        print("fonttools が見つかりません。`python -m venv <dir>` の環境へ `pip install fonttools==%s` で入れてください（リポジトリには入れない）"
              % lock.get("tool", {}).get("fonttools", "<版>"))
        return False
    want = lock.get("tool", {}).get("fonttools")
    if want and fontTools.version != want:
        print("fonttools の版が lock と違います（lock=%s, 今=%s）。生成物のバイト一致は同じ版でしか保証されません: pip install fonttools==%s"
              % (want, fontTools.version, want))
        return False
    return True


def build_all(src_dir, strict_version=True):
    lock = load_lock()
    if strict_version:
        if not require_fonttools(lock):
            return None
    else:
        try:
            import fontTools  # noqa: F401
        except ImportError:
            print("fonttools が見つかりません。`pip install fonttools` で入れてください（venv。リポジトリには入れない）")
            return None
    ui = required_ui_chars() | base_chars()
    splash = required_splash_chars()
    plex_cps = None
    results = {}
    for spec in FONTS:
        s = lock["sources"][spec["src"]]
        path = os.path.join(ROOT, s["repoPath"]) if "repoPath" in s else find_source(src_dir, s["member"])
        if not path:
            print("元ファイルが見つかりません: %s（--fetch --src %s で取得）" % (s["member"], src_dir))
            return None
        got = sha256_file(path)
        if got != s["sha256"]:
            print("元ファイルの SHA256 が lock と違います: %s\n  lock=%s\n  now =%s" % (s["member"], s["sha256"], got))
            return None
        chars = ui if spec["charset"] == "ui" else splash
        if spec.get("plex_only"):
            if plex_cps is None:
                plex_cps = set()
                from fontTools.ttLib import TTFont
                for sid in ("plexmono-regular", "plexjp-regular"):
                    ps_ = lock["sources"][sid]
                    pp = find_source(src_dir, ps_["member"])
                    if not pp:
                        print("元ファイルが見つかりません: %s（--fetch --src %s で取得）" % (ps_["member"], src_dir))
                        return None
                    f_ = TTFont(pp, lazy=True)
                    plex_cps |= set(f_.getBestCmap())
                    f_.close()
            chars = set(c for c in chars if ord(c) in plex_cps)
        data = font_bytes(build_one(spec, path, chars))
        results[spec["out"]] = data
    return results, ui, splash


# --- 取得 ------------------------------------------------------------------------------------------------
def do_fetch(src_dir):
    import urllib.request
    import zipfile
    lock = load_lock()
    os.makedirs(src_dir, exist_ok=True)
    zips = {}
    for sid, s in lock["sources"].items():
        if "zipUrl" not in s:
            continue
        zips.setdefault(s["zipUrl"], []).append((sid, s))
    for url, items in zips.items():
        want = [(sid, s) for sid, s in items
                if not (os.path.exists(os.path.join(src_dir, os.path.basename(s["member"])))
                        and sha256_file(os.path.join(src_dir, os.path.basename(s["member"]))) == s["sha256"])]
        if not want:
            continue
        with tempfile.TemporaryDirectory() as td:
            zp = os.path.join(td, "x.zip")
            print("取得:", url)
            try:
                urllib.request.urlretrieve(url, zp)
            except Exception as e:   # ネットワークの失敗はトレースバックでなく 1 行で伝える
                print("取得に失敗しました: %s (%s)" % (url, e))
                return 1
            zsha = sha256_file(zp)
            if zsha != items[0][1]["zipSha256"]:
                print("zip の SHA256 が lock と違います:", url, zsha)
                return 1
            try:
                zf = zipfile.ZipFile(zp)
            except zipfile.BadZipFile as e:
                print("zip を開けません: %s (%s)" % (url, e))
                return 1
            with zf as z:
                for sid, s in want:
                    dst = os.path.join(src_dir, os.path.basename(s["member"]))
                    try:
                        with z.open(s["member"]) as fi, open(dst, "wb") as fo:
                            fo.write(fi.read())
                    except KeyError:
                        print("zip に %s がありません" % s["member"])
                        return 1
                    if sha256_file(dst) != s["sha256"]:
                        print("展開したファイルの SHA256 が違います:", s["member"])
                        return 1
    print("元ファイルを", src_dir, "へ置きました")
    return 0


# --- 検査（標準ライブラリだけ）-----------------------------------------------------------------------------
def _sfnt_tables(data):
    import struct
    off = struct.unpack(">I", data[12:16])[0] if data[:4] == b"ttcf" else 0
    n = struct.unpack(">H", data[off + 4:off + 6])[0]
    tabs = {}
    for i in range(n):
        tag, _cs, o, ln = struct.unpack(">4sIII", data[off + 12 + 16 * i:off + 28 + 16 * i])
        tabs[tag.decode("latin-1")] = (o, ln)
    return tabs


def font_cmap(data):
    """TrueType の cmap（format 4 / 12）から収録コードポイントの集合を読む。"""
    import struct
    o, _ = _sfnt_tables(data)["cmap"]
    nsub = struct.unpack(">H", data[o + 2:o + 4])[0]
    cps = set()
    for i in range(nsub):
        pid, eid, so = struct.unpack(">HHI", data[o + 4 + 8 * i:o + 12 + 8 * i])
        if (pid, eid) not in ((3, 1), (3, 10), (0, 3), (0, 4), (0, 6)):
            continue
        s = o + so
        fmt = struct.unpack(">H", data[s:s + 2])[0]
        if fmt == 4:
            segx2 = struct.unpack(">H", data[s + 6:s + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(">%dH" % seg, data[s + 14:s + 14 + segx2])
            starts = struct.unpack(">%dH" % seg, data[s + 16 + segx2:s + 16 + 2 * segx2])
            deltas = struct.unpack(">%dh" % seg, data[s + 16 + 2 * segx2:s + 16 + 3 * segx2])
            ro_pos = s + 16 + 3 * segx2
            ros = struct.unpack(">%dH" % seg, data[ro_pos:ro_pos + segx2])
            for k in range(seg):
                for c in range(starts[k], ends[k] + 1):
                    if c == 0xFFFF:
                        continue
                    if ros[k] == 0:
                        g = (c + deltas[k]) & 0xFFFF
                    else:
                        gp = ro_pos + 2 * k + ros[k] + 2 * (c - starts[k])
                        g = struct.unpack(">H", data[gp:gp + 2])[0]
                        if g:
                            g = (g + deltas[k]) & 0xFFFF
                    if g:
                        cps.add(c)
        elif fmt == 12:
            ng = struct.unpack(">I", data[s + 12:s + 16])[0]
            for k in range(ng):
                sc, ec, sg = struct.unpack(">III", data[s + 16 + 12 * k:s + 28 + 12 * k])
                if sg:
                    cps.update(range(sc, ec + 1))
    return cps


def font_names(data, ids=(1, 4, 6, 16, 21)):
    """name テーブルの指定 nameID の文字列（全プラットフォーム・全言語）。"""
    import struct
    o, _ = _sfnt_tables(data)["name"]
    cnt, so = struct.unpack(">HH", data[o + 2:o + 6])
    out = []
    for i in range(cnt):
        pid, _eid, _lid, nid, ln, off = struct.unpack(">6H", data[o + 6 + 12 * i:o + 18 + 12 * i])
        if nid not in ids:
            continue
        raw = data[o + so + off:o + so + off + ln]
        out.append((nid, raw.decode("utf-16-be", "replace") if pid in (0, 3) else raw.decode("latin-1")))
    return out


def read_charset_file(path):
    if not os.path.exists(path):
        return set()
    out = set()
    for line in read_text(path).splitlines():
        if line.startswith("#"):
            continue
        out.update(c for c in line if c not in "\r\n\t")
    return out


def do_check(src_dir):
    ok = True
    lock = load_lock()
    datas = {}
    for spec in FONTS:
        p = os.path.join(ROOT, spec["out"])
        if os.path.exists(p):
            with open(p, "rb") as f:
                datas[spec["out"]] = f.read()

    # 1) 字の被覆: 必要な UI 文字が、コミット済みのどれかの書体の cmap に入っているか、許可リスト（システム書体へ縮退）にあること。
    #    charset_ui.txt を毎回作り直させない（他レーンが新しい文字列を足しても、既に書体にある字なら赤くならない）
    allow = read_charset_file(ALLOW_FALLBACK)
    ui_cov = set()
    splash_cov = set()
    for spec in FONTS:
        d = datas.get(spec["out"])
        if d is None:
            continue
        (splash_cov if spec["charset"] == "splash" else ui_cov).update(chr(c) for c in font_cmap(d))
    miss = sorted((required_ui_chars() - ui_cov) - allow, key=ord)
    if miss:
        ok = False
        print("NG どの UI 書体にも入っていない UI 文字が %d 字あります: %s" % (len(miss), "".join(miss[:80])))
        print("   -> third_party/fonts/extra_chars.txt に足して --build --src <dir> で作り直すか、"
              "アイコン・絵文字なら charset_system_fallback.txt の許可リストへ")
    miss_s = sorted((required_splash_chars() - splash_cov) - allow, key=ord)
    if miss_s:
        ok = False
        print("NG 起動画面の書体（splash_subset.ttf）に無い文字が %d 字あります: %s" % (len(miss_s), "".join(miss_s[:80])))
        print("   -> third_party/fonts/extra_chars.txt の [splash] 節に足して --build --src <dir> で作り直してください")

    # 1b) PlemolJP 由来の出力に Hack（Bitstream Vera License）由来の字形が無いこと:
    #     収録字がすべて、Plex 由来の出力（Uno UI Mono / Uno UI Sans JP。Plex Mono・Plex Sans JP の字だけを持つ）にもあること
    plex_out = set()
    for spec in FONTS:
        if spec["out"] in datas and "/ibm-plex/" in spec["out"] and spec["src"] in ("plexmono-regular", "plexjp-regular"):
            plex_out.update(font_cmap(datas[spec["out"]]))
    for spec in FONTS:
        if spec.get("plex_only") and spec["out"] in datas:
            extra = sorted(c for c in font_cmap(datas[spec["out"]]) if c not in plex_out)
            if extra:
                ok = False
                print("NG %s に Plex に無い字（Hack 由来の可能性。Bitstream Vera License）が %d 字: %s" % (
                    spec["out"], len(extra), " ".join("U+%04X" % c for c in extra[:20])))

    # 2) 生成物の SHA256 が lock と一致、サイズの上限、予約名（RFN）が名前表に無いこと
    total = 0
    for spec in FONTS:
        p = os.path.join(ROOT, spec["out"])
        if spec["out"] not in datas:
            ok = False
            print("NG 生成物がありません:", spec["out"])
            continue
        size = os.path.getsize(p)
        if spec["out"].endswith("splash_subset.ttf"):
            if size > LIMIT_SPLASH:
                ok = False
                print("NG splash_subset.ttf が %d バイト（上限 %d）" % (size, LIMIT_SPLASH))
        else:
            total += size
        rec = lock["outputs"].get(spec["out"])
        if not rec or rec["sha256"] != sha256_file(p):
            ok = False
            print("NG 生成物の SHA256 が lock と違います:", spec["out"])
        for nid, s in font_names(datas[spec["out"]]):
            low = s.lower()
            bad = [r for r in RESERVED_NAMES if r in low]
            if bad:
                ok = False
                print("NG %s の nameID %d '%s' に予約フォント名 %s が残っています" % (spec["out"], nid, s, bad))
    if total > LIMIT_TOTAL:
        ok = False
        print("NG フォント資産の合計が %d バイト（上限 %d）" % (total, LIMIT_TOTAL))

    # 3) 元ファイルがあれば、作り直してバイト一致
    if src_dir:
        r = build_all(src_dir)
        if r is None:
            return 1
        results, _ui, _sp = r
        for out, data in results.items():
            with open(os.path.join(ROOT, out), "rb") as f:
                if f.read() != data:
                    ok = False
                    print("NG 元ファイルから作り直した結果が生成物と一致しません:", out)
    print("OK subset_fonts --check（合計 %.2f MB / 上限 %.0f MB）" % (total / 1048576.0, LIMIT_TOTAL / 1048576.0) if ok else "FAILED")
    return 0 if ok else 1


def do_build(src_dir):
    r = build_all(src_dir, strict_version=False)
    if r is None:
        return 1
    results, ui, splash = r
    lock = load_lock()
    lock["outputs"] = {}
    for out, data in results.items():
        p = os.path.join(ROOT, out)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        lock["outputs"][out] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    with open(CHARSET_UI, "w", encoding="utf-8", newline="\n") as f:
        f.write(charset_text(required_ui_chars()))
    with open(CHARSET_SPLASH, "w", encoding="utf-8", newline="\n") as f:
        f.write(charset_text(splash))
    import fontTools
    lock["tool"] = {"fonttools": fontTools.version}
    save_lock(lock)
    for out, data in results.items():
        print("%8d  %s" % (len(data), out))
    print("charset_ui: %d 字（抜き出し＋追加） / 書体に入れた字: %d 字の候補" % (len(required_ui_chars()), len(ui)))
    return 0


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--src", help="元ファイルを置いた（置く）ディレクトリ")
    a = ap.parse_args()
    if a.fetch:
        if not a.src:
            ap.error("--fetch には --src が要ります")
        return do_fetch(a.src)
    if a.build:
        if not a.src:
            ap.error("--build には --src が要ります")
        return do_build(a.src)
    if a.check:
        return do_check(a.src)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
