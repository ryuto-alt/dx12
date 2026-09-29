using System.Diagnostics;
using System.Numerics;
using CUE4Parse.UE4.Assets.Exports.Nanite;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse_Conversion.Dto;
using CUE4Parse_Conversion.Options;

namespace UeCook;

/// <summary>UStaticMesh → RawMesh。Nanite のフル解像度（葉クラスタ = LOD0 相当）または通常 LOD0（フォールバック）を取り出す。</summary>
static class MeshExtract
{
    public static long MemoryLimitBytes = 8L << 30;   // 8 GB を超えそうなら中止

    public static string[] MaterialNames(UStaticMesh sm)
    {
        var mats = sm.StaticMaterials;
        var names = new string[mats.Length];
        for (int i = 0; i < mats.Length; i++)
        {
            string slot = mats[i].MaterialSlotName.Text ?? $"slot{i}";
            string? mi = null;
            try { mi = mats[i].MaterialInterface?.Name; } catch { /* import 解決失敗は無視 */ }
            names[i] = mi != null ? $"{slot}|{mi}" : slot;
        }
        return names;
    }

    public static bool HasNanite(UStaticMesh sm) => sm.RenderData?.NaniteResources is { PageStreamingStates.Length: > 0 };

    public static int FallbackTriangles(UStaticMesh sm)
    {
        var lods = sm.RenderData?.LODs;
        if (lods == null || lods.Length == 0) return 0;
        long t = 0;
        foreach (var s in lods[0].Sections) t += s.NumTriangles;
        return (int)t;
    }

    static void CheckMemory(string where)
    {
        long ws = Process.GetCurrentProcess().WorkingSet64;
        if (ws > MemoryLimitBytes) throw new InvalidOperationException($"working set {ws >> 20} MB exceeds the {MemoryLimitBytes >> 20} MB limit ({where}); aborting");
    }

    /// <summary>Nanite の全ページをデコードして葉クラスタ（EdgeLength &lt; 0）だけを集める。</summary>
    public static RawMesh FromNanite(UStaticMesh sm, bool weld)
    {
        var nr = sm.RenderData!.NaniteResources!;
        var m = new RawMesh { Source = "nanite-leaf", MaterialNames = MaterialNames(sm) };
        int matCount = Math.Max(1, m.MaterialNames.Length);
        var sw = Stopwatch.StartNew();

        int np = nr.PageStreamingStates.Length;
        nr.LoadedPages = new FNaniteStreamableData?[np];
        int failedPages = 0;
        for (uint i = 0; i < np; i++)
        {
            nr.GetPage(i);
            if (nr.LoadedPages[i] == null) failedPages++;
            if ((i & 7) == 0) CheckMemory($"page {i}/{np}");
        }
        var all = nr.LoadedPages.Where(p => p != null).SelectMany(p => p!.Clusters).ToList();
        var leaf = all.Where(c => c.EdgeLength < 0.0f && !c.bVoxel).ToArray();
        m.DecodeMs = sw.Elapsed.TotalMilliseconds;

        long nVerts = 0, nTris = 0, allTris = 0;
        foreach (var c in all) allTris += c.NumTris;
        foreach (var c in leaf) { nVerts += c.NumVerts; nTris += c.NumTris; }
        if (nVerts > int.MaxValue / 3 || nTris > int.MaxValue / 3) throw new InvalidOperationException("mesh too large for the prototype (int-indexed arrays)");

        // 材質ごとの三角形数 → セクション先頭
        var perMat = new long[matCount];
        foreach (var c in leaf) foreach (var (mat, _, len) in Ranges(c, matCount)) perMat[mat] += len;
        var first = new long[matCount];
        long acc = 0;
        for (int i = 0; i < matCount; i++) { first[i] = acc; acc += perMat[i]; }

        m.Pos = new float[nVerts * 3];
        m.Nrm = new float[nVerts * 3];
        m.Uv = new float[nVerts * 2];
        m.Idx = new uint[nTris * 3];
        var cursor = (long[])first.Clone();

        long v0 = 0, missing = 0, degenerate = 0, outward = 0, inward = 0;
        var min = new Vector3(float.MaxValue); var max = new Vector3(float.MinValue);
        foreach (var c in leaf)
        {
            for (int i = 0; i < c.Vertices.Length; i++)
            {
                long v = v0 + i;
                var nv = c.Vertices[i];
                if (nv?.Attributes is not { } a) { missing++; continue; }
                float x = (float)nv.Pos.X, y = (float)nv.Pos.Y, z = (float)nv.Pos.Z;
                m.Pos[v * 3] = x; m.Pos[v * 3 + 1] = y; m.Pos[v * 3 + 2] = z;
                m.Nrm[v * 3] = (float)a.Normal.X; m.Nrm[v * 3 + 1] = (float)a.Normal.Y; m.Nrm[v * 3 + 2] = (float)a.Normal.Z;
                m.Uv[v * 2] = (float)a.UVs[0].X; m.Uv[v * 2 + 1] = (float)a.UVs[0].Y;
                min = Vector3.Min(min, new Vector3(x, y, z)); max = Vector3.Max(max, new Vector3(x, y, z));
            }
            foreach (var (mat, start, len) in Ranges(c, matCount))
            {
                for (uint t = 0; t < len; t++)
                {
                    var tri = c.TriIndices[start + t];
                    long o = cursor[mat]++ * 3;
                    uint a0 = tri.X + (uint)v0, a1 = tri.Y + (uint)v0, a2 = tri.Z + (uint)v0;
                    m.Idx[o] = a0; m.Idx[o + 1] = a1; m.Idx[o + 2] = a2;
                    Orient(m, a0, a1, a2, ref degenerate, ref outward, ref inward);
                }
            }
            v0 += c.NumVerts;
        }
        for (int i = 0; i < matCount; i++)
            if (perMat[i] > 0) m.Sections.Add(((uint)i, (uint)(first[i] * 3), (uint)(perMat[i] * 3)));

        m.Stats["pages"] = np; m.Stats["failedPages"] = failedPages;
        m.Stats["clustersAll"] = all.Count; m.Stats["clustersLeaf"] = leaf.Length;
        m.Stats["trisAllLevels"] = allTris; m.Stats["trisLeaf"] = nTris; m.Stats["vertsLeafRaw"] = nVerts;
        m.Stats["inputTris"] = (long)nr.NumInputTriangles; m.Stats["inputVerts"] = (long)nr.NumInputVertices;
        m.Stats["missingVerts"] = missing; m.Stats["degenerateTris"] = degenerate;
        m.Stats["windingOutward"] = outward; m.Stats["windingInward"] = inward;
        m.Stats["aabbMin"] = $"{min.X:F1},{min.Y:F1},{min.Z:F1}"; m.Stats["aabbMax"] = $"{max.X:F1},{max.Y:F1},{max.Z:F1}";
        m.Stats["numUVs"] = leaf.Length > 0 ? (int)leaf.Max(c => c.NumUVs) : 0;
        m.Stats["hasTangents"] = leaf.Any(c => c.bHasTangents);
        m.Stats["posPrecision"] = nr.PositionPrecision;
        if (leaf.Length > 0) { m.Stats["posStepCm"] = leaf[0].PosScale; m.Stats["normalBits"] = (int)leaf[0].NormalPrecision; }
        nr.UnloadAllPages();

        if (weld)
        {
            var sw2 = Stopwatch.StartNew();
            Weld(m);
            m.WeldMs = sw2.Elapsed.TotalMilliseconds;
        }
        m.Stats["vertsOut"] = m.VertexCount;
        m.Stats["signedVolumeRatio"] = Math.Round(SignedVolumeRatio(m, min, max), 4);
        return m;
    }

    /// <summary>通常メッシュ LOD0（Nanite のフォールバック、または非 Nanite メッシュの最高 LOD）。</summary>
    public static RawMesh FromFallback(UStaticMesh sm)
    {
        var m = new RawMesh { Source = "fallback-lod0", MaterialNames = MaterialNames(sm) };
        var sw = Stopwatch.StartNew();
        var dto = new StaticMeshDto(sm, EMeshQuality.Highest, ENaniteMeshFormat.NoNanite);
        if (dto.LODs.Count == 0) throw new InvalidOperationException("no regular LOD (mesh data stripped)");
        var lod = dto.LODs[0];
        int n = lod.Vertices.Length;
        m.Pos = new float[n * 3]; m.Nrm = new float[n * 3]; m.Uv = new float[n * 2];
        var min = new Vector3(float.MaxValue); var max = new Vector3(float.MinValue);
        for (int i = 0; i < n; i++)
        {
            var v = lod.Vertices[i];
            m.Pos[i * 3] = (float)v.Position.X; m.Pos[i * 3 + 1] = (float)v.Position.Y; m.Pos[i * 3 + 2] = (float)v.Position.Z;
            m.Nrm[i * 3] = (float)v.Normal.X; m.Nrm[i * 3 + 1] = (float)v.Normal.Y; m.Nrm[i * 3 + 2] = (float)v.Normal.Z;
            m.Uv[i * 2] = v.Uv.U; m.Uv[i * 2 + 1] = v.Uv.V;
            min = Vector3.Min(min, new Vector3(m.Pos[i * 3], m.Pos[i * 3 + 1], m.Pos[i * 3 + 2]));
            max = Vector3.Max(max, new Vector3(m.Pos[i * 3], m.Pos[i * 3 + 1], m.Pos[i * 3 + 2]));
        }
        m.Idx = lod.Indices;
        int matCount = Math.Max(1, m.MaterialNames.Length);
        // セクションは firstIndex 昇順に隙間なく並べる（VGSRC の規則）。材質添字は範囲へクランプ。
        long cur = 0;
        foreach (var s in lod.Sections.OrderBy(s => s.FirstIndex))
        {
            if (s.NumFaces <= 0) continue;
            if (s.FirstIndex != cur) throw new InvalidOperationException($"sections do not tile the index buffer (first {s.FirstIndex}, expected {cur})");
            m.Sections.Add(((uint)Math.Clamp(s.MaterialIndex, 0, matCount - 1), (uint)s.FirstIndex, (uint)(s.NumFaces * 3)));
            cur += s.NumFaces * 3L;
        }
        if (cur != m.Idx.Length) throw new InvalidOperationException($"sections cover {cur} of {m.Idx.Length} indices");
        long deg = 0, outw = 0, inw = 0;
        for (long t = 0; t < m.TriCount; t++) Orient(m, m.Idx[t * 3], m.Idx[t * 3 + 1], m.Idx[t * 3 + 2], ref deg, ref outw, ref inw);
        m.DecodeMs = sw.Elapsed.TotalMilliseconds;
        m.Stats["degenerateTris"] = deg; m.Stats["windingOutward"] = outw; m.Stats["windingInward"] = inw;
        m.Stats["aabbMin"] = $"{min.X:F1},{min.Y:F1},{min.Z:F1}"; m.Stats["aabbMax"] = $"{max.X:F1},{max.Y:F1},{max.Z:F1}";
        m.Stats["lodCount"] = sm.RenderData!.LODs.Length;
        m.Stats["vertsOut"] = m.VertexCount;
        m.Stats["signedVolumeRatio"] = Math.Round(SignedVolumeRatio(m, min, max), 4);
        return m;
    }

    // --- 材質範囲（3 分割の高速経路 / 材質テーブル）---
    static IEnumerable<(int Mat, uint Start, uint Len)> Ranges(FCluster c, int matCount)
    {
        int Clamp(uint x) => (int)Math.Min(x, (uint)(matCount - 1));
        if (!c.ShouldUseMaterialTable())
        {
            uint l0 = c.Material0Length, l1 = c.Material1Length, l2 = c.NumTris - (l0 + l1);
            if (l0 > 0) yield return (Clamp(c.Material0Index), 0, l0);
            if (l1 > 0) yield return (Clamp(c.Material1Index), l0, l1);
            if (l2 > 0 && l2 <= c.NumTris) yield return (Clamp(c.Material2Index), l0 + l1, l2);
        }
        else
        {
            foreach (var r in c.MaterialRanges)
                if (r.TriLength > 0) yield return (Clamp(r.MaterialIndex), r.TriStart, r.TriLength);
        }
    }

    /// <summary>cross(p1-p0, p2-p0) と頂点法線の内積の符号で、VGSRC の巻き順規約（外向き）を満たすか数える。</summary>
    static void Orient(RawMesh m, uint a, uint b, uint c, ref long degenerate, ref long outward, ref long inward)
    {
        var p = m.Pos; var n = m.Nrm;
        float ux = p[b * 3] - p[a * 3], uy = p[b * 3 + 1] - p[a * 3 + 1], uz = p[b * 3 + 2] - p[a * 3 + 2];
        float vx = p[c * 3] - p[a * 3], vy = p[c * 3 + 1] - p[a * 3 + 1], vz = p[c * 3 + 2] - p[a * 3 + 2];
        float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        if (a == b || b == c || a == c || (cx == 0 && cy == 0 && cz == 0)) { degenerate++; return; }
        float nx = n[a * 3] + n[b * 3] + n[c * 3], ny = n[a * 3 + 1] + n[b * 3 + 1] + n[c * 3 + 1], nz = n[a * 3 + 2] + n[b * 3 + 2] + n[c * 3 + 2];
        float d = cx * nx + cy * ny + cz * nz;
        if (d > 0) outward++; else if (d < 0) inward++;
    }

    /// <summary>閉じたメッシュなら、cross(p1-p0,p2-p0) が外向きのとき正。AABB 体積との比を返す（負 = 内向き）。</summary>
    public static double SignedVolumeRatio(RawMesh m, Vector3 min, Vector3 max)
    {
        var c = (min + max) * 0.5f;
        double vol = 0;
        var p = m.Pos;
        for (long t = 0; t < m.TriCount; t++)
        {
            long a = m.Idx[t * 3], b = m.Idx[t * 3 + 1], d = m.Idx[t * 3 + 2];
            double ax = p[a * 3] - c.X, ay = p[a * 3 + 1] - c.Y, az = p[a * 3 + 2] - c.Z;
            double bx = p[b * 3] - c.X, by = p[b * 3 + 1] - c.Y, bz = p[b * 3 + 2] - c.Z;
            double dx = p[d * 3] - c.X, dy = p[d * 3 + 1] - c.Y, dz = p[d * 3 + 2] - c.Z;
            vol += ax * (by * dz - bz * dy) + ay * (bz * dx - bx * dz) + az * (bx * dy - by * dx);
        }
        vol /= 6.0;
        double box = (double)(max.X - min.X) * (max.Y - min.Y) * (max.Z - min.Z);
        return box > 0 ? vol / box : 0;
    }

    /// <summary>位置・法線・UV がビット一致する頂点を畳む（Nanite はクラスタ境界で頂点が重複する）。</summary>
    static void Weld(RawMesh m)
    {
        int n = (int)m.VertexCount;
        if (n == 0) return;
        int cap = (int)BitOperations.RoundUpToPowerOf2((uint)Math.Max(16, n * 2));
        var table = new int[cap];
        Array.Fill(table, -1);
        var remap = new uint[n];
        int outN = 0;
        Span<uint> key = stackalloc uint[8];
        for (int i = 0; i < n; i++)
        {
            LoadKey(m, i, key);
            uint h = 2166136261;
            for (int k = 0; k < 8; k++) { h ^= key[k]; h *= 16777619; h ^= h >> 15; }
            int slot = (int)(h & (uint)(cap - 1));
            int found = -1;
            while (table[slot] != -1)
            {
                int j = table[slot];
                if (Equal(m, j, key)) { found = j; break; }
                slot = (slot + 1) & (cap - 1);
            }
            if (found >= 0) { remap[i] = (uint)found; continue; }
            // 新規頂点: 先頭へ詰める（outN <= i なので読み出し済みの領域を壊さない）
            if (outN != i)
            {
                for (int k = 0; k < 3; k++) { m.Pos[outN * 3 + k] = m.Pos[i * 3 + k]; m.Nrm[outN * 3 + k] = m.Nrm[i * 3 + k]; }
                m.Uv[outN * 2] = m.Uv[i * 2]; m.Uv[outN * 2 + 1] = m.Uv[i * 2 + 1];
            }
            table[slot] = outN;
            remap[i] = (uint)outN;
            outN++;
        }
        for (long t = 0; t < m.Idx.LongLength; t++) m.Idx[t] = remap[m.Idx[t]];
        Array.Resize(ref m.Pos, outN * 3); Array.Resize(ref m.Nrm, outN * 3); Array.Resize(ref m.Uv, outN * 2);
    }

    static void LoadKey(RawMesh m, int i, Span<uint> key)
    {
        for (int k = 0; k < 3; k++) { key[k] = BitConverter.SingleToUInt32Bits(m.Pos[i * 3 + k]); key[3 + k] = BitConverter.SingleToUInt32Bits(m.Nrm[i * 3 + k]); }
        key[6] = BitConverter.SingleToUInt32Bits(m.Uv[i * 2]); key[7] = BitConverter.SingleToUInt32Bits(m.Uv[i * 2 + 1]);
    }

    static bool Equal(RawMesh m, int j, ReadOnlySpan<uint> key)
    {
        for (int k = 0; k < 3; k++)
            if (BitConverter.SingleToUInt32Bits(m.Pos[j * 3 + k]) != key[k] || BitConverter.SingleToUInt32Bits(m.Nrm[j * 3 + k]) != key[3 + k]) return false;
        return BitConverter.SingleToUInt32Bits(m.Uv[j * 2]) == key[6] && BitConverter.SingleToUInt32Bits(m.Uv[j * 2 + 1]) == key[7];
    }
}
