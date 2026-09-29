using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace UeCook;

/// <summary>docs/VGEO_SPEC.md §12 の VGSRC v1.0 を書く。配列は順に流し、sourceHash（本体の FNV-1a 64）を書きながら計算してヘッダへ書き戻す。</summary>
static class VgsrcWriter
{
    const uint Magic = 0x52534756;   // "VGSR"
    const uint None = 0xFFFFFFFF;
    const int HeaderSize = 64;

    sealed class HashingStream
    {
        readonly Stream _s;
        public ulong Hash = 0xcbf29ce484222325ul;
        public long Cursor = HeaderSize;
        public HashingStream(Stream s) { _s = s; }
        public void Write(ReadOnlySpan<byte> b)
        {
            ulong h = Hash;
            foreach (byte x in b) { h ^= x; h *= 0x100000001b3ul; }
            Hash = h;
            _s.Write(b);
            Cursor += b.Length;
        }
        public void PadTo(long abs)
        {
            Span<byte> z = stackalloc byte[16];
            z.Clear();
            while (Cursor < abs) Write(z[..(int)Math.Min(16, abs - Cursor)]);
        }
        public void Align16() => PadTo((Cursor + 15) & ~15L);
    }

    /// <param name="engineCoords">true: C# 側で (x,y,z)_UE→(y,z,x)×0.01 に変換して coordSystem=0 で出す。false: UE 座標・cm のまま coordSystem=1 で出す（cooker が変換）。</param>
    public static void Write(string path, RawMesh m, bool engineCoords, bool flipWinding = true)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var fs = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20);
        fs.Write(new byte[HeaderSize]);
        var hs = new HashingStream(fs);

        long vc = m.VertexCount, ic = m.Idx.LongLength;
        var names = m.MaterialNames.Length > 0 ? m.MaterialNames : ["default"];

        // positions / normals（必要なら変換）/ uv0
        hs.Align16(); WriteFloats(hs, m.Pos, 3, engineCoords, scale: 0.01f);
        hs.Align16(); WriteFloats(hs, m.Nrm, 3, engineCoords, scale: 1f);
        hs.Align16(); WriteRaw(hs, MemoryMarshal.AsBytes(m.Uv.AsSpan()));
        hs.Align16(); WriteIndices(hs, m.Idx, flipWinding);

        // sections
        hs.Align16();
        Span<byte> rec = stackalloc byte[16];
        foreach (var (mat, first, count) in m.Sections)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(rec, mat);
            BinaryPrimitives.WriteUInt32LittleEndian(rec[4..], first);
            BinaryPrimitives.WriteUInt32LittleEndian(rec[8..], count);
            BinaryPrimitives.WriteUInt32LittleEndian(rec[12..], 0);
            hs.Write(rec);
        }

        // 文字列プール（先頭 '\0'、各名前、末尾 '\0'）
        var pool = new List<byte> { 0 };
        var nameOff = new uint[names.Length];
        for (int i = 0; i < names.Length; i++)
        {
            nameOff[i] = (uint)pool.Count;
            pool.AddRange(Encoding.UTF8.GetBytes(names[i]));
            pool.Add(0);
        }

        // materials（96 B）。テクスチャ・PBR 値は P6 本実装で UMaterialInterface から埋める。スパイクでは既定値。
        hs.Align16();
        Span<byte> mr = stackalloc byte[96];
        foreach (uint off in nameOff)
        {
            mr.Clear();
            BinaryPrimitives.WriteUInt32LittleEndian(mr, off);                 // nameOff
            BinaryPrimitives.WriteUInt32LittleEndian(mr[8..], None);           // albedoPathOff
            BinaryPrimitives.WriteUInt32LittleEndian(mr[12..], None);          // normalPathOff
            BinaryPrimitives.WriteUInt32LittleEndian(mr[16..], None);          // metalRoughPathOff
            BinaryPrimitives.WriteUInt32LittleEndian(mr[20..], None);          // emissivePathOff
            PutF(mr, 28, 0.8f);                                                // roughness
            PutF(mr, 48, 0.5f);                                                // alphaCutoff
            PutF(mr, 52, 1f);                                                  // baseColorAlpha
            for (int k = 0; k < 4; k++) PutF(mr, 56 + 4 * k, 1f);              // baseColorFactor
            PutF(mr, 72, 1f); PutF(mr, 76, 1f);                                // uvScaleOffset = (1,1,0,0)
            hs.Write(mr);
        }

        // strings: u32 byteLength + bytes
        hs.Align16();
        Span<byte> len = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(len, (uint)pool.Count);
        hs.Write(len);
        hs.Write(pool.ToArray());

        // ヘッダ（最後に書き戻す）
        var h = new byte[HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(h, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(6), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(8), 3);                          // hasNormals | hasUV0
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(12), (uint)names.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(16), (uint)m.Sections.Count);
        BinaryPrimitives.WriteUInt64LittleEndian(h.AsSpan(24), (ulong)vc);
        BinaryPrimitives.WriteUInt64LittleEndian(h.AsSpan(32), (ulong)ic);
        BinaryPrimitives.WriteSingleLittleEndian(h.AsSpan(40), engineCoords ? 1.0f : 0.01f);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(44), engineCoords ? 0u : 1u);    // coordSystem
        BinaryPrimitives.WriteUInt64LittleEndian(h.AsSpan(48), hs.Hash);
        fs.Position = 0;
        fs.Write(h);
    }

    /// <summary>UE の cooked 三角形は cross(p1-p0, p2-p0) が「内向き」（実測: BasicShapes/Cube で符号付き体積 -1）。
    /// VGSRC / VGEO は外向きを要求する（VGEO_SPEC C26）ので、既定で 2 番目と 3 番目の頂点を入れ替える。</summary>
    static void WriteIndices(HashingStream hs, uint[] idx, bool flip)
    {
        if (!flip) { WriteRaw(hs, MemoryMarshal.AsBytes(idx.AsSpan())); return; }
        var buf = new uint[3 * 65536];
        for (long i = 0; i < idx.LongLength; i += buf.Length)
        {
            int n = (int)Math.Min(buf.Length, idx.LongLength - i);
            for (int k = 0; k < n; k += 3) { buf[k] = idx[i + k]; buf[k + 1] = idx[i + k + 2]; buf[k + 2] = idx[i + k + 1]; }
            WriteRaw(hs, MemoryMarshal.AsBytes(buf.AsSpan(0, n)));
        }
    }

    static void PutF(Span<byte> b, int off, float v) => BinaryPrimitives.WriteSingleLittleEndian(b[off..], v);

    static void WriteRaw(HashingStream hs, ReadOnlySpan<byte> bytes)
    {
        const int Chunk = 1 << 20;
        for (int o = 0; o < bytes.Length; o += Chunk) hs.Write(bytes.Slice(o, Math.Min(Chunk, bytes.Length - o)));
    }

    /// <summary>engine 座標なら (x,y,z)→(y,z,x)×scale。UE のままなら素通し。</summary>
    static void WriteFloats(HashingStream hs, float[] a, int stride, bool engineCoords, float scale)
    {
        if (!engineCoords) { WriteRaw(hs, MemoryMarshal.AsBytes(a.AsSpan())); return; }
        var buf = new float[stride * 65536];
        for (long i = 0; i < a.LongLength; i += buf.Length)
        {
            int n = (int)Math.Min(buf.Length, a.LongLength - i);
            for (int k = 0; k < n; k += 3)
            {
                buf[k] = a[i + k + 1] * scale; buf[k + 1] = a[i + k + 2] * scale; buf[k + 2] = a[i + k] * scale;
            }
            WriteRaw(hs, MemoryMarshal.AsBytes(buf.AsSpan(0, n)));
        }
    }
}
