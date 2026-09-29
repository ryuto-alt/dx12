using Serilog;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Versions;
using UeCook;

Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.BelowNormal;   // ユーザーの PC を占有しない
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
Serilog.Log.Logger = new Serilog.LoggerConfiguration().MinimumLevel.Warning().WriteTo.Console(standardErrorFromLevel: Serilog.Events.LogEventLevel.Verbose).CreateLogger();   // CUE4Parse は読み取り失敗を握りつぶしてログに出すだけなので見えるようにする

if (args.Length == 0 || args.Contains("--help") || args.Contains("-h")) { Usage(); return 1; }

string? paks = null, usmap = null, listOut = null, extract = null, extractOut = null, filter = null, info = null, extractAll = null;
string ue = "5.6";
bool listNanite = false, decode = false, onlyNanite = false, weld = true, engineCoords = false, fallback = false, force = false, flip = true;
int top = 0; long maxTris = 6_000_000;
for (int i = 0; i < args.Length; i++)
{
    string a = args[i];
    string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{a} needs a value");
    switch (a)
    {
        case "--paks": paks = Next(); break;
        case "--usmap": usmap = Next(); break;
        case "--ue": ue = Next(); break;
        case "--list-nanite": listNanite = true; break;
        case "--decode": decode = true; break;
        case "--only-nanite": onlyNanite = true; break;
        case "--filter": filter = Next(); break;
        case "--top": top = int.Parse(Next()); break;
        case "--csv": listOut = Next(); break;
        case "--extract": extract = Next(); break;
        case "--out": extractOut = Next(); break;
        case "--info": info = Next(); break;
        case "--extract-all": extractAll = Next(); break;
        case "--no-weld": weld = false; break;
        case "--engine-coords": engineCoords = true; break;
        case "--fallback": fallback = true; break;
        case "--max-tris": maxTris = long.Parse(Next()); break;
        case "--force": force = true; break;
        case "--no-flip-winding": flip = false; break;
        case "--mem-limit-mb": MeshExtractLimit(long.Parse(Next())); break;
        default: Console.Error.WriteLine($"unknown option {a}"); Usage(); return 1;
    }
}
if (paks == null || usmap == null) { Console.Error.WriteLine("--paks and --usmap are required"); return 1; }
var game = ue switch { "5.4" => EGame.GAME_UE5_4, "5.5" => EGame.GAME_UE5_5, "5.6" => EGame.GAME_UE5_6, "5.7" => EGame.GAME_UE5_7, "5.8" => EGame.GAME_UE5_8, _ => throw new ArgumentException($"unsupported --ue {ue}") };

var swMount = Stopwatch.StartNew();
var prov = UeMount.Open(paks, usmap, game);
Console.WriteLine($"mounted {prov.Files.Count} files ({game}) in {swMount.ElapsedMilliseconds} ms");

if (listNanite) return ListNanite(prov, filter, decode, onlyNanite, top, listOut);
if (info != null) return Info(prov, info);
if (extractAll != null) return ExtractAll(prov, extractAll, filter, onlyNanite, weld, engineCoords, flip, maxTris);
if (extract != null)
{
    if (extractOut == null) { Console.Error.WriteLine("--extract needs --out FILE.vgsrc"); return 1; }
    return Extract(prov, extract, extractOut, weld, engineCoords, fallback, maxTris, force, flip);
}
Usage();
return 1;

static void MeshExtractLimit(long mb) => MeshExtract.MemoryLimitBytes = mb << 20;

static void Usage()
{
    Console.WriteLine("""
        ue_cook - UE cooked StaticMesh -> VGSRC prototype (VGEO_SPEC section 12).  ファイル読み出しのみ。ゲーム/エディタは起動しない。

          ue_cook --paks <Content/Paks> --usmap <file.usmap> [--ue 5.6] <command>

        commands:
          --list-nanite [--filter SUBSTR] [--only-nanite] [--decode] [--top N] [--csv FILE]
                 StaticMesh を列挙し Nanite 有無・ページ/クラスタ・入力三角形数・フォールバック LOD0 三角形数を出す。
                 --decode で Nanite 葉クラスタを全デコードして三角形数/所要時間も出す（重い）。
          --info PACKAGE                   1 メッシュの詳細
          --extract-all DIR [--filter S] [--only-nanite]   該当する全 StaticMesh を DIR/*.vgsrc へ（一括変換の所要時間・失敗一覧）
          --extract PACKAGE --out F.vgsrc  1 メッシュを VGSRC へ
                 [--fallback]              Nanite でも通常 LOD0（フォールバック）を出す
                 [--engine-coords]         C# 側で (y,z,x)*0.01 に変換して coordSystem=0 で出す（既定は UE 座標 cm・coordSystem=1）
                 [--no-weld]               ビット一致頂点の溶接を止める
                 [--no-flip-winding]       UE の三角形順をそのまま出す（既定は外向きにするため 2,3 番目を入れ替える。VGEO_SPEC C26 参照）
                 [--max-tris N]            Nanite の入力三角形数の上限（既定 6,000,000。--force で無視）
          --mem-limit-mb N                 ワーキングセット上限（既定 8192）
        """);
}

static int ListNanite(CUE4Parse.FileProvider.DefaultFileProvider prov, string? filter, bool decode, bool onlyNanite, int top, string? csv)
{
    var rows = new List<Row>();
    var sw = Stopwatch.StartNew();
    int scanned = 0, meshes = 0, errors = 0;
    foreach (var gf in UeMount.EnumerateAssets(prov, filter))
    {
        scanned++;
        try
        {
            var pkg = prov.LoadPackage(gf);
            int ei = UeMount.FindStaticMeshExport(pkg);
            if (ei < 0) continue;
            meshes++;
            var t0 = sw.Elapsed.TotalMilliseconds;
            var sm = (UStaticMesh)pkg.ExportsLazy[ei].Value;
            var nr = sm.RenderData?.NaniteResources;
            bool nan = MeshExtract.HasNanite(sm);
            if (onlyNanite && !nan) continue;
            var r = new Row
            {
                Path = gf.Path,
                Nanite = nan,
                Pages = nr?.PageStreamingStates.Length ?? 0,
                Clusters = (long)(nr?.NumClusters ?? 0),
                InputTris = (long)(nr?.NumInputTriangles ?? 0),
                FallbackTris = MeshExtract.FallbackTriangles(sm),
                Lods = sm.RenderData?.LODs?.Length ?? 0,
                Materials = sm.StaticMaterials.Length,
                LoadMs = sw.Elapsed.TotalMilliseconds - t0,
                Flags = nr != null ? nr.ResourceFlags.ToString().Replace(", ", "|") : "",
                Assembly = nr?.AssemblyTransforms.Length ?? 0,
                HierNodes = nr?.HierarchyNodes.Length ?? 0,
            };
            if (decode && nan)
            {
                try
                {
                    var m = MeshExtract.FromNanite(sm, weld: false);
                    r.LeafTris = m.TriCount; r.DecodeMs = m.DecodeMs;
                    r.Missing = Convert.ToInt64(m.Stats["missingVerts"]); r.Degenerate = Convert.ToInt64(m.Stats["degenerateTris"]); r.Failed = Convert.ToInt32(m.Stats["failedPages"]);
                }
                catch (Exception e) { r.Error = e.GetType().Name + ": " + e.Message; }
            }
            rows.Add(r);
        }
        catch (Exception e)
        {
            errors++;
            if (errors <= 5) Console.Error.WriteLine($"  ! {gf.Path}: {e.GetType().Name}: {e.Message}");
        }
    }
    Console.WriteLine($"scanned {scanned} uassets, StaticMesh {meshes}, listed {rows.Count}, errors {errors}, {sw.Elapsed.TotalSeconds:F1} s");
    var nanRows = rows.Where(r => r.Nanite).ToList();
    Console.WriteLine($"Nanite meshes: {nanRows.Count} / {rows.Count}   inputTris sum {nanRows.Sum(r => r.InputTris):N0}, max {(nanRows.Count > 0 ? nanRows.Max(r => r.InputTris) : 0):N0}");
    if (decode)
    {
        var dec = nanRows.Where(r => r.LeafTris >= 0).ToList();
        Console.WriteLine($"decode: {dec.Count} meshes, leafTris sum {dec.Sum(r => r.LeafTris):N0} (input {dec.Sum(r => r.InputTris):N0}), mismatches {dec.Count(r => r.LeafTris != r.InputTris)}, errors {nanRows.Count(r => r.Error != null)}, failedPages {dec.Sum(r => r.Failed)}, missingVerts {dec.Sum(r => r.Missing):N0}, zero-area/degenerate tris {dec.Sum(r => r.Degenerate):N0}, decode {dec.Sum(r => r.DecodeMs) / 1000:F1} s");
    }
    var sorted = rows.OrderByDescending(r => Math.Max(r.InputTris, r.FallbackTris)).ToList();
    if (top > 0) sorted = sorted.Take(top).ToList();
    Console.WriteLine("nanite  pages clusters   inputTris fallbackTris  lods mats   leafTris decodeMs  path");
    foreach (var r in sorted)
        Console.WriteLine($"{(r.Nanite ? "Y" : "-"),6} {r.Pages,6} {r.Clusters,8} {r.InputTris,11:N0} {r.FallbackTris,12:N0} {r.Lods,5} {r.Materials,4} {(r.LeafTris >= 0 ? r.LeafTris.ToString("N0") : ""),10} {(r.DecodeMs > 0 ? r.DecodeMs.ToString("F0") : ""),8}  {r.Path}{(r.Error != null ? "  ERR " + r.Error : "")}");
    if (csv != null)
    {
        var sb = new StringBuilder("path,nanite,pages,clusters,inputTris,fallbackTris,lods,materials,leafTris,decodeMs,loadMs,resourceFlags,assemblyTransforms,hierarchyNodes\n");
        foreach (var r in rows.OrderByDescending(r => r.InputTris))
            sb.AppendLine($"{r.Path},{(r.Nanite ? 1 : 0)},{r.Pages},{r.Clusters},{r.InputTris},{r.FallbackTris},{r.Lods},{r.Materials},{r.LeafTris},{r.DecodeMs:F0},{r.LoadMs:F1},{r.Flags},{r.Assembly},{r.HierNodes}");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(csv))!);
        File.WriteAllText(csv, sb.ToString());
        Console.WriteLine($"csv -> {csv}");
    }
    return 0;
}

static int Info(CUE4Parse.FileProvider.DefaultFileProvider prov, string q)
{
    var gf = UeMount.Resolve(prov, q);
    if (gf == null) { Console.Error.WriteLine($"package not found: {q}"); return 2; }
    var pkg = prov.LoadPackage(gf);
    var sm = UeMount.LoadStaticMesh(pkg);
    if (sm == null) { Console.Error.WriteLine("no StaticMesh export"); return 2; }
    var nr = sm.RenderData?.NaniteResources;
    Console.WriteLine($"path        {gf.Path}");
    Console.WriteLine($"lods        {sm.RenderData?.LODs?.Length}   fallbackTris(LOD0) {MeshExtract.FallbackTriangles(sm):N0}");
    Console.WriteLine($"materials   {sm.StaticMaterials.Length}: {string.Join(", ", MeshExtract.MaterialNames(sm))}");
    if (nr == null || nr.PageStreamingStates.Length == 0) { Console.WriteLine("nanite      no"); return 0; }
    Console.WriteLine($"nanite      yes  pages {nr.PageStreamingStates.Length} (root {nr.NumRootPages})  clusters {nr.NumClusters}  inputTris {nr.NumInputTriangles:N0}  inputVerts {nr.NumInputVertices:N0}");
    Console.WriteLine($"            flags {nr.ResourceFlags}  posPrecision {nr.PositionPrecision}  normalPrecision {nr.NormalPrecision}  hierarchyNodes {nr.HierarchyNodes.Length}  bounds {nr.MeshBounds?.Origin} r={nr.MeshBounds?.SphereRadius}");
    return 0;
}

static int Extract(CUE4Parse.FileProvider.DefaultFileProvider prov, string q, string outPath, bool weld, bool engineCoords, bool fallback, long maxTris, bool force, bool flip)
{
    var gf = UeMount.Resolve(prov, q);
    if (gf == null) { Console.Error.WriteLine($"package not found: {q}"); return 2; }
    var sw = Stopwatch.StartNew();
    var pkg = prov.LoadPackage(gf);
    var sm = UeMount.LoadStaticMesh(pkg);
    if (sm == null) { Console.Error.WriteLine("no StaticMesh export"); return 2; }
    double loadMs = sw.Elapsed.TotalMilliseconds;
    bool nan = MeshExtract.HasNanite(sm);
    RawMesh m;
    if (nan && !fallback)
    {
        long inTris = sm.RenderData!.NaniteResources!.NumInputTriangles;
        if (inTris > maxTris && !force) { Console.Error.WriteLine($"Nanite input {inTris:N0} tris exceeds --max-tris {maxTris:N0} (use --force)"); return 3; }
        m = MeshExtract.FromNanite(sm, weld);
    }
    else m = MeshExtract.FromFallback(sm);
    var swW = Stopwatch.StartNew();
    VgsrcWriter.Write(outPath, m, engineCoords, flip);
    long size = new FileInfo(outPath).Length;
    double writeMs = swW.Elapsed.TotalMilliseconds;
    Console.WriteLine($"package   {gf.Path}");
    Console.WriteLine($"source    {m.Source}  nanite={nan}");
    Console.WriteLine($"output    {outPath}  {size / 1048576.0:F1} MB  coord={(engineCoords ? "engine(m)" : "UE(cm)")}");
    Console.WriteLine($"counts    verts {m.VertexCount:N0}  tris {m.TriCount:N0}  sections {m.Sections.Count}  materials {Math.Max(1, m.MaterialNames.Length)}");
    Console.WriteLine($"time      load {loadMs:F0} ms  decode {m.DecodeMs:F0} ms  weld {m.WeldMs:F0} ms  write {writeMs:F0} ms  total {sw.Elapsed.TotalMilliseconds:F0} ms");
    foreach (var kv in m.Stats) Console.WriteLine($"stat      {kv.Key} = {kv.Value}");
    Console.WriteLine($"peakWS    {Process.GetCurrentProcess().PeakWorkingSet64 >> 20} MB");
    return 0;
}

static int ExtractAll(CUE4Parse.FileProvider.DefaultFileProvider prov, string dir, string? filter, bool onlyNanite, bool weld, bool engineCoords, bool flip, long maxTris)
{
    Directory.CreateDirectory(dir);
    var sw = Stopwatch.StartNew();
    int ok = 0, nanite = 0, skipped = 0, failed = 0; long tris = 0, verts = 0, bytes = 0; double decodeMs = 0;
    foreach (var gf in UeMount.EnumerateAssets(prov, filter))
    {
        try
        {
            var pkg = prov.LoadPackage(gf);
            var sm = UeMount.LoadStaticMesh(pkg);
            if (sm == null) continue;
            bool nan = MeshExtract.HasNanite(sm);
            if (onlyNanite && !nan) continue;
            if (nan && sm.RenderData!.NaniteResources!.NumInputTriangles > maxTris) { skipped++; continue; }
            var m = nan ? MeshExtract.FromNanite(sm, weld) : MeshExtract.FromFallback(sm);
            if (m.TriCount == 0) { skipped++; continue; }
            string file = Path.Combine(dir, gf.Path.Replace(".uasset", "").Replace('/', '_') + ".vgsrc");
            VgsrcWriter.Write(file, m, engineCoords, flip);
            ok++; if (nan) nanite++; tris += m.TriCount; verts += m.VertexCount; bytes += new FileInfo(file).Length; decodeMs += m.DecodeMs;
        }
        catch (Exception e)
        {
            failed++;
            if (failed <= 10) Console.Error.WriteLine($"  ! {gf.Path}: {e.GetType().Name}: {e.Message}");
        }
    }
    Console.WriteLine($"extract-all: ok {ok} (nanite {nanite}), skipped {skipped}, failed {failed}, tris {tris:N0}, verts {verts:N0}, {bytes / 1048576.0:F0} MB, decode {decodeMs / 1000:F1} s, total {sw.Elapsed.TotalSeconds:F1} s, peakWS {Process.GetCurrentProcess().PeakWorkingSet64 >> 20} MB");
    return failed == 0 ? 0 : 4;
}

sealed class Row
{
    public string Path = "";
    public bool Nanite;
    public int Pages, FallbackTris, Lods, Materials;
    public long Clusters, InputTris, LeafTris = -1;
    public double DecodeMs, LoadMs;
    public string? Error;
    public string Flags = "";
    public int Assembly, HierNodes, Failed;
    public long Missing, Degenerate;
}
