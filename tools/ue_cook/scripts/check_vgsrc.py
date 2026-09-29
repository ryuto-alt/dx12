"""VGSRC v1.0（docs/VGEO_SPEC.md 12 章）を読み、C++ の ReadVgsrc が見ない項目を点検する。

  python check_vgsrc.py FILE.vgsrc

出力:
  - cross(p1-p0, p2-p0) と頂点法線の内積が正（外向き）/ 負（内向き）の三角形数
  - AABB 中心を原点にした符号付き体積（閉じたメッシュなら、外向きのとき正）
  - 面積ゼロの三角形数、法線の長さ、UV の範囲

巻き順は座標系を問わない（(x,y,z)→(y,z,x) は巡回置換なので cross の向きは保たれる）。numpy が必要。
"""
import struct
import sys

import numpy as np


def read(path):
    d = open(path, 'rb').read()
    magic, maj, mnr, flags, mc, sc, r0, vc, ic, unit, cs, h, r1 = struct.unpack_from('<IHHIIIIQQfIQQ', d, 0)
    if magic != 0x52534756:
        raise SystemExit('not a VGSRC file')

    def al(x):
        return (x + 15) & ~15

    c = 64
    pos_o = al(c); c = pos_o + vc * 12
    nrm_o = al(c); c = nrm_o + vc * 12
    uv_o = al(c); c = uv_o + vc * 8
    idx_o = al(c)
    return dict(
        P=np.frombuffer(d, np.float32, vc * 3, pos_o).reshape(-1, 3),
        N=np.frombuffer(d, np.float32, vc * 3, nrm_o).reshape(-1, 3),
        UV=np.frombuffer(d, np.float32, vc * 2, uv_o).reshape(-1, 2),
        I=np.frombuffer(d, np.uint32, ic, idx_o).reshape(-1, 3),
        coord=cs, unit=unit, vc=vc, ic=ic)


if __name__ == '__main__':
    m = read(sys.argv[1])
    P, N, I = m['P'], m['N'], m['I']
    a, b, c = P[I[:, 0]], P[I[:, 1]], P[I[:, 2]]
    cr = np.cross(b - a, c - a)
    fn = N[I[:, 0]] + N[I[:, 1]] + N[I[:, 2]]
    d = (cr * fn).sum(1)
    print(f"coordSystem {m['coord']}  unitScale {m['unit']}  vertices {m['vc']}  triangles {len(I)}")
    print('cross . normal   > 0 (outward):', int((d > 0).sum()), '  < 0 (inward):', int((d < 0).sum()))
    cen = (P.min(0) + P.max(0)) / 2
    a2, b2, c2 = a - cen, b - cen, c - cen
    vol = (a2 * np.cross(b2, c2)).sum() / 6
    print('signed volume / AABB volume:', vol / max(1e-30, float(np.prod(P.max(0) - P.min(0)))), '(closed mesh: +outward)')
    area = np.linalg.norm(cr, axis=1) / 2
    print('zero-area triangles:', int((area == 0).sum()))
    nl = np.linalg.norm(N, axis=1)
    print('normal length', float(nl.min()), float(nl.max()), '  uv range', m['UV'].min(0), m['UV'].max(0))
