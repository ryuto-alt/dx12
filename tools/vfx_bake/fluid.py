"""3D の Stable Fluids(半ラグランジュ移流 + ヤコビ圧力投影 + 渦度の補正)。numpy だけ・CPU・決定論。

座標は a[x, y, z]。+y が上、+z がカメラ側。単位は「セル」と「ステップ」(速度 = セル/ステップ)。
オープン境界(側面と上面は圧力 0)、床だけ壁。炎と煙の両方をこれで焼く。
"""
import numpy as np


class Fluid3D:
    def __init__(self, n, scalars=("D", "T", "age", "ox", "oz")):
        self.n = n
        self.names = ("u", "v", "w") + tuple(scalars)
        self.f = np.zeros((len(self.names), n, n, n), np.float32)
        self.p = np.zeros((n, n, n), np.float32)
        g = np.arange(n, dtype=np.float32)
        self.gx, self.gy, self.gz = np.meshgrid(g, g, g, indexing="ij")
        self.idx = {k: i for i, k in enumerate(self.names)}

    def __getitem__(self, k):
        return self.f[self.idx[k]]

    # ---- 移流(半ラグランジュ + トライリニア。MacCormack は試したが MV の補間精度が落ちたので使わない) ----
    def advect(self, dt):
        n = self.n
        u, v, w = self.f[0], self.f[1], self.f[2]
        hi = np.float32(n - 1)
        px = np.clip(self.gx - dt * u, 0, hi)
        py = np.clip(self.gy - dt * v, 0, hi)
        pz = np.clip(self.gz - dt * w, 0, hi)
        x0, y0, z0 = (np.minimum(np.floor(a).astype(np.int32), n - 2) for a in (px, py, pz))
        fx, fy, fz = px - x0, py - y0, pz - z0
        flat = self.f.reshape(len(self.names), -1)
        out = np.zeros_like(flat)
        for dx in (0, 1):
            wx = fx if dx else 1 - fx
            for dy in (0, 1):
                wy = fy if dy else 1 - fy
                for dz in (0, 1):
                    wz = fz if dz else 1 - fz
                    idx = (((x0 + dx) * n + (y0 + dy)) * n + (z0 + dz)).ravel()
                    out += flat[:, idx] * (wx * wy * wz).ravel()
        self.f = out.reshape(self.f.shape)

    # ---- 圧力投影 ----
    def project(self, iters=40):
        n = self.n
        u, v, w = self.f[0], self.f[1], self.f[2]
        # 床(y=0)は壁: 法線速度 0。他の面はゼロ勾配(流出可)
        v[:, 0, :] = np.maximum(v[:, 0, :], 0)
        up = np.pad(u, 1, mode="edge")
        vp = np.pad(v, 1, mode="edge")
        wp = np.pad(w, 1, mode="edge")
        div = 0.5 * ((up[2:, 1:-1, 1:-1] - up[:-2, 1:-1, 1:-1]) + (vp[1:-1, 2:, 1:-1] - vp[1:-1, :-2, 1:-1]) +
                     (wp[1:-1, 1:-1, 2:] - wp[1:-1, 1:-1, :-2]))
        p = self.p
        for _ in range(iters):
            pp = np.pad(p, 1, mode="constant")  # 側面と上面は p = 0(オープン)
            pp[1:-1, 0, 1:-1] = p[:, 0, :]       # 床は ∂p/∂n = 0
            p = (pp[2:, 1:-1, 1:-1] + pp[:-2, 1:-1, 1:-1] + pp[1:-1, 2:, 1:-1] + pp[1:-1, :-2, 1:-1] +
                 pp[1:-1, 1:-1, 2:] + pp[1:-1, 1:-1, :-2] - div) / 6.0
        self.p = p.astype(np.float32)
        pp = np.pad(self.p, 1, mode="constant")
        pp[1:-1, 0, 1:-1] = self.p[:, 0, :]
        u -= 0.5 * (pp[2:, 1:-1, 1:-1] - pp[:-2, 1:-1, 1:-1])
        v -= 0.5 * (pp[1:-1, 2:, 1:-1] - pp[1:-1, :-2, 1:-1])
        w -= 0.5 * (pp[1:-1, 1:-1, 2:] - pp[1:-1, 1:-1, :-2])

    # ---- 渦度の補正(細部が潰れないように) ----
    def vorticity_confinement(self, eps, dt):
        u, v, w = self.f[0], self.f[1], self.f[2]
        du = np.gradient(u)
        dv = np.gradient(v)
        dw = np.gradient(w)
        wx = dw[1] - dv[2]
        wy = du[2] - dw[0]
        wz = dv[0] - du[1]
        mag = np.sqrt(wx * wx + wy * wy + wz * wz)
        gx, gy, gz = np.gradient(mag)
        gl = np.sqrt(gx * gx + gy * gy + gz * gz) + 1e-6
        nx, ny, nz = gx / gl, gy / gl, gz / gl
        u += dt * eps * (ny * wz - nz * wy)
        v += dt * eps * (nz * wx - nx * wz)
        w += dt * eps * (nx * wy - ny * wx)

    def disk_mask(self, cx, cz, radius, y0, y1):
        """床の近くの円柱(ソース)のなめらかなマスク 0..1。"""
        r = np.sqrt((self.gx - cx) ** 2 + (self.gz - cz) ** 2)
        m = np.clip(1.0 - r / radius, 0, 1)
        m = m * m * (3 - 2 * m)
        ym = ((self.gy >= y0) & (self.gy <= y1)).astype(np.float32)
        return (m * ym).astype(np.float32)
