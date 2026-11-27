#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ブルーノイズ（void-and-cluster, Ulichney 1993）の生成器。標準ライブラリだけで動く（決定的）。

全ストリームで共有する唯一の生成器（BA-3 が W1 で作る。LS-05 / PI-10 / VFX は個別に作らず、これを流用する）。
  python tools/gen/blue_noise.py --size 64 --out assets/editor/splash/bluenoise64.png
出力: 8bit グレースケール PNG。各画素は 0..N-1 の順位を 0..255 へ等間隔に写した値（ヒストグラムがほぼ平坦）。
タイル可能（トーラス上の距離）。sigma = 1.5、種は固定（同じ引数なら同じバイト列）。
"""
import argparse
import math
import random
import struct
import sys
import zlib


def generate(n, sigma=1.5, seed=0x5EED):
    total = n * n
    rad = int(math.ceil(sigma * 4))
    kern = [(dx, dy, math.exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma))) for dy in range(-rad, rad + 1) for dx in range(-rad, rad + 1)]
    energy = [0.0] * total
    binary = [False] * total

    def splat(idx, sign):
        x, y = idx % n, idx // n
        for dx, dy, w in kern:
            energy[((y + dy) % n) * n + (x + dx) % n] += sign * w

    def pick(want_one):
        best, bi = None, -1
        for i in range(total):
            if binary[i] != want_one:
                continue
            e = energy[i]
            if bi < 0 or (e > best if want_one else e < best):
                best, bi = e, i
        return bi

    rng = random.Random(seed)
    ones = max(1, total // 10)
    for i in rng.sample(range(total), ones):
        binary[i] = True
        splat(i, +1.0)
    for _ in range(total * 4):
        c = pick(True)
        binary[c] = False
        splat(c, -1.0)
        v = pick(False)
        binary[v] = True
        splat(v, +1.0)
        if v == c:
            break
    init_bin = binary[:]
    rank = [0] * total
    # 位相 1: 初期パターンから 1 を 1 つずつ外し、順位 ones-1 .. 0
    for r in range(ones - 1, -1, -1):
        c = pick(True)
        rank[c] = r
        binary[c] = False
        splat(c, -1.0)
    # 初期パターンへ戻す
    energy[:] = [0.0] * total
    for i in range(total):
        binary[i] = init_bin[i]
        if binary[i]:
            splat(i, +1.0)
    # 位相 2: 最大の空きへ 1 を足し、順位 ones .. total/2
    r = ones
    while r < total // 2:
        v = pick(False)
        rank[v] = r
        binary[v] = True
        splat(v, +1.0)
        r += 1
    # 位相 3: 残りも同じ規則（0 のうちエネルギー最小 = 0 の塊が最も詰まった所）で埋める。反転して最大の塊を探す元の定式化と等価
    while r < total:
        v = pick(False)
        rank[v] = r
        binary[v] = True
        splat(v, +1.0)
        r += 1
    return rank


def write_png(path, n, rank):
    total = n * n
    raw = bytearray()
    for y in range(n):
        raw.append(0)
        for x in range(n):
            raw.append((rank[y * n + x] * 255 + (total - 1) // 2) // (total - 1))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 0, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--sigma", type=float, default=1.5)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rank = generate(a.size, a.sigma)
    assert sorted(rank) == list(range(a.size * a.size)), "順位が順列になっていません"
    write_png(a.out, a.size, rank)
    print("OK %s (%dx%d)" % (a.out, a.size, a.size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
