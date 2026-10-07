using System.IO;
using System.IO.Compression;

namespace BloodborneLauncher;

public static class SaveSafety
{
    static IEnumerable<string> Files(string directory)
    {
        var info = new DirectoryInfo(directory);
        if ((info.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("El save contiene enlaces no admitidos.");
        foreach (var entry in info.EnumerateFileSystemInfos()) {
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("El save contiene enlaces no admitidos.");
            if (entry is DirectoryInfo child) { foreach (var file in Files(child.FullName)) yield return file; }
            else yield return entry.FullName;
        }
    }
    public static string Backup(string data)
    {
        var source = Path.Combine(data, "saves");
        if (!Directory.Exists(source)) throw new IOException("Todavía no hay partidas guardadas en bbhost.");
        var target = Path.Combine(data, "launcher-backups"); Directory.CreateDirectory(target);
        string file = Path.Combine(target, $"saves-{DateTime.Now:yyyy-MM-dd_HH-mm-ss}-{Guid.NewGuid():N}.zip");
        var files = Files(source).ToArray();
        using (var zip = ZipFile.Open(file, ZipArchiveMode.Create))
            foreach (var entry in files) zip.CreateEntryFromFile(entry, Path.GetRelativePath(source, entry), CompressionLevel.Optimal);
        return file;
    }
    public static void Import(string source, string data)
    {
        source = Path.GetFullPath(source);
        if (Directory.Exists(Path.Combine(source, "saves"))) source = Path.Combine(source, "saves");
        var from = Path.Combine(source, "SPRJ0005");
        if (!Directory.Exists(from)) throw new IOException("Selecciona la carpeta de saves que contiene SPRJ0005.");
        var root = Path.Combine(data, "saves"); var target = Path.Combine(root, "SPRJ0005");
        if (Directory.Exists(target) || File.Exists(target)) throw new IOException("bbhost ya tiene una partida SPRJ0005. La importación no sobrescribe partidas.");
        Directory.CreateDirectory(root);
        string staging = Path.Combine(root, "import-" + Guid.NewGuid().ToString("N"));
        try {
            Directory.CreateDirectory(staging);
            foreach (var file in Files(from)) {
                string destination = Path.Combine(staging, Path.GetRelativePath(from, file));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.Copy(file, destination, false);
            }
            // An atomic directory move refuses a destination created meanwhile.
            Directory.Move(staging, target);
        } finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
    }
}
