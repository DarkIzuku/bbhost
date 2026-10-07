using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace BloodborneLauncher;

// The same small TOML-shaped subset as core/config.cpp. Change only requested
// keys; account tokens, bindings, plugin settings, unknown keys and comments
// survive. There is no launcher JSON/INI settings store.
public static class TomlSettings
{
    public static string Quote(string value)
    {
        if (value.IndexOfAny(['"', '\r', '\n']) >= 0) throw new ArgumentException("El valor contiene caracteres no válidos.");
        return '"' + value.Replace('\\', '/') + '"';
    }
    public static string Fingerprint(string path) => File.Exists(path) ? Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))) : "";
    public static Dictionary<string, string> Read(string path)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!File.Exists(path)) return result;
        string section = "";
        foreach (var line in File.ReadAllLines(path)) {
            var text = line.Trim();
            if (text.Length == 0 || text.StartsWith('#')) continue;
            if (text.StartsWith('[') && text.EndsWith(']')) { section = text[1..^1].Trim(); continue; }
            int eq = text.IndexOf('=');
            if (eq <= 0) throw new IOException("Configuración inválida: " + path);
            string raw = text[(eq + 1)..].Trim();
            if (raw.StartsWith('"') || raw.StartsWith('\'')) {
                int close = raw.IndexOf(raw[0], 1);
                if (close < 0) throw new IOException("Cadena TOML sin cerrar.");
                raw = raw[1..close].Replace("\\\\", "\\");
            } else { raw = raw.Split('#')[0].Trim(); }
            result[section + "." + text[..eq].Trim()] = raw;
        }
        return result;
    }
    public static void Write(string path, IReadOnlyDictionary<string, string> edits, string? expected = null)
    {
        if (expected is not null && Fingerprint(path) != expected) throw new IOException("La configuración cambió desde que se abrió. Recarga antes de guardar.");
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
        foreach (var entry in edits) {
            int dot = entry.Key.IndexOf('.');
            if (dot <= 0 || !Regex.IsMatch(entry.Key, @"^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$")) throw new ArgumentException("Clave inválida.");
            string section = entry.Key[..dot], key = entry.Key[(dot + 1)..];
            if (entry.Value.IndexOfAny(['\r', '\n']) >= 0) throw new ArgumentException("Valor inválido.");
            int begin = lines.FindIndex(x => x.Trim() == $"[{section}]");
            if (begin < 0) { lines.Add(""); lines.Add($"[{section}]"); begin = lines.Count - 1; }
            int end = begin + 1;
            while (end < lines.Count && !lines[end].TrimStart().StartsWith('[')) ++end;
            int found = -1;
            for (int i = begin + 1; i < end; ++i) {
                string line = lines[i].Trim(); int eq = line.IndexOf('=');
                if (eq > 0 && line[..eq].Trim() == key) { found = i; break; }
            }
            string replacement = key + " = " + entry.Value;
            if (found >= 0) lines[found] = replacement; else lines.Insert(end, replacement);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try {
            File.WriteAllText(temp, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temp, path, null); else File.Move(temp, path);
        } finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}
