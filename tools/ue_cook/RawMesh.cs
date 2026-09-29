namespace UeCook;

/// <summary>取り出したメッシュ（UE 座標・cm のまま）。VGSRC へ書く前の中間表現。</summary>
sealed class RawMesh
{
    public string Source = "";                 // "nanite-leaf" | "fallback-lod0"
    public float[] Pos = [];                   // 3 * n
    public float[] Nrm = [];                   // 3 * n
    public float[] Uv = [];                    // 2 * n
    public uint[] Idx = [];                    // 3 * tris（セクション順に並べ替え済み）
    public List<(uint Mat, uint First, uint Count)> Sections = new();
    public string[] MaterialNames = [];
    public long VertexCount => Pos.Length / 3;
    public long TriCount => Idx.Length / 3;
    public double DecodeMs;
    public double WeldMs;
    public Dictionary<string, object> Stats = new();
}
