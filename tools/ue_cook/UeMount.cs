using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.FileProvider.Objects;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;

namespace UeCook;

/// <summary>CUE4Parse のマウントと、パッケージ内の「メイン StaticMesh export」の特定。ファイル読み出しのみ（ゲームは起動しない）。</summary>
static class UeMount
{
    public static DefaultFileProvider Open(string paksDir, string usmap, EGame game)
    {
        var prov = new DefaultFileProvider(paksDir, SearchOption.AllDirectories, new VersionContainer(game), StringComparer.OrdinalIgnoreCase);
        prov.MappingsContainer = new FileUsmapTypeMappingsProvider(usmap);
        prov.Initialize();
        prov.SubmitKey(new FGuid(), new FAesKey(new byte[32])); // Dreamcore は AES 無し（ゼロ鍵で通る）
        prov.PostMount();
        prov.ReadNaniteData = true;
        return prov;
    }

    static string? ClassNameOf(IPackage pkg, int i) => pkg switch
    {
        IoPackage io => io.ResolveObjectIndex(io.ExportMap[i].ClassIndex)?.Name.Text,
        Package p => p.ExportMap[i].ClassName,
        _ => null,
    };

    /// <summary>パッケージ内で指定クラスの export の添字を返す（デシリアライズはしない）。無ければ -1。</summary>
    public static int FindExport(IPackage pkg, string className)
    {
        for (int i = 0; i < pkg.ExportMapLength; i++)
            if (ClassNameOf(pkg, i) == className) return i;
        return -1;
    }

    public static int FindStaticMeshExport(IPackage pkg) => FindExport(pkg, "StaticMesh");

    public static UStaticMesh? LoadStaticMesh(IPackage pkg)
    {
        int i = FindStaticMeshExport(pkg);
        return i < 0 ? null : pkg.ExportsLazy[i].Value as UStaticMesh;
    }

    public static IEnumerable<GameFile> EnumerateAssets(DefaultFileProvider prov, string? filter)
    {
        foreach (var (path, gf) in prov.Files)
        {
            if (!path.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase)) continue;
            if (filter != null && !path.Contains(filter, StringComparison.OrdinalIgnoreCase)) continue;
            yield return gf;
        }
    }

    /// <summary>パッケージパス（拡張子あり/なし・部分一致）から 1 つ解決する。</summary>
    public static GameFile? Resolve(DefaultFileProvider prov, string q)
    {
        q = q.Replace("\\", "/");
        if (prov.Files.TryGetValue(q, out var gf)) return gf;
        if (prov.Files.TryGetValue(q + ".uasset", out gf)) return gf;
        var hits = prov.Files.Where(kv => kv.Key.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) &&
                                          kv.Key.Contains(q, StringComparison.OrdinalIgnoreCase)).Select(kv => kv.Value).ToList();
        if (hits.Count == 1) return hits[0];
        if (hits.Count > 1)
        {
            var exact = hits.Where(h => Path.GetFileNameWithoutExtension(h.Path).Equals(q, StringComparison.OrdinalIgnoreCase)).ToList();
            if (exact.Count == 1) return exact[0];
            Console.Error.WriteLine($"ambiguous '{q}' ({hits.Count} hits): {string.Join(", ", hits.Take(5).Select(h => h.Path))}");
        }
        return null;
    }
}
