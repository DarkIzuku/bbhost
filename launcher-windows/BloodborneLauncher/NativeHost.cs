using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace BloodborneLauncher;

public sealed class NativeOption
{
    public string Key { get; set; } = "";
    public string Label { get; set; } = "";
    public string Section { get; set; } = "";
    public string Note { get; set; } = "";
    public string[] Values { get; set; } = [];
    public int Index { get; set; }
    public bool Restart { get; set; }
}
public sealed class NativePatch
{
    public string Name { get; set; } = "";
    public string Description { get; set; } = "";
    public string Option { get; set; } = "";
    public bool Enabled { get; set; }
}
public sealed class NativeState
{
    [JsonIgnore] public string GpuDescription { get; set; } = "";
    public bool Ok { get; set; }
    public string Error { get; set; } = "";
    public string Version { get; set; } = "";
    public string Commit { get; set; } = "";
    [JsonPropertyName("config_file")] public string ConfigFile { get; set; } = "";
    [JsonPropertyName("options_file")] public string OptionsFile { get; set; } = "";
    public string App0 { get; set; } = "";
    public string Data { get; set; } = "";
    public string Mods { get; set; } = "";
    public string Account { get; set; } = "";
    public List<NativeOption> Options { get; set; } = [];
    public List<NativePatch> Patches { get; set; } = [];
}
public sealed class PreparedGame
{
    public bool Ok { get; set; }
    public string Error { get; set; } = "";
    public string App0 { get; set; } = "";
    public string Eboot { get; set; } = "";
    public string Version { get; set; } = "";
    [JsonPropertyName("title_id")] public string TitleId { get; set; } = "";
    public string Sha256 { get; set; } = "";
    public bool Prepared { get; set; }
}
public sealed class AccountResult
{
    public string Account { get; set; } = "";
    public string Outcome { get; set; } = "";
    public string Detail { get; set; } = "";
}

public sealed class NativeHost(string root)
{
    public string Root { get; } = root;
    public string Executable => Path.Combine(Root, "bbhost.exe");
    public string Profile { get; set; } = "";
    static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };

    public ProcessStartInfo StartInfo(params string[] args)
    {
        var info = new ProcessStartInfo(Executable) {
            WorkingDirectory = Root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        if (!string.IsNullOrEmpty(Profile)) { info.ArgumentList.Add("--config"); info.ArgumentList.Add(Profile); }
        foreach (var arg in args) info.ArgumentList.Add(arg);
        info.Environment["BBHOST_SETUP_WINDOW"] = "0";
        info.Environment["BBHOST_WPF_CHILD"] = "1";
        return info;
    }

    public async Task<T> Query<T>(params string[] args)
    {
        using var process = Process.Start(StartInfo(args)) ?? throw new IOException("No se pudo abrir bbhost.");
        // Drain both pipes simultaneously; errors and crashes must not deadlock.
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var output = await stdout;
        var diagnostic = await stderr;
        var result = JsonSerializer.Deserialize<T>(output, JsonOptions);
        if (result is null) throw new IOException($"bbhost no devolvió una respuesta válida (exit {process.ExitCode}). {diagnostic}");
        return result;
    }

    public async Task<AccountResult> AccountAction(string action, string name, string code, Action<AccountResult> progress)
    {
        using var process = Process.Start(StartInfo("--launcher-account", action, "--account-name", name, "--account-code", code)) ?? throw new IOException("No se pudo abrir bbhost.");
        var stderr = process.StandardError.ReadToEndAsync();
        var result = new AccountResult();
        while (await process.StandardOutput.ReadLineAsync() is { } line) {
            result = JsonSerializer.Deserialize<AccountResult>(line, JsonOptions) ?? throw new IOException("Respuesta de cuenta inválida.");
            progress(result);
        }
        await process.WaitForExitAsync(); await stderr;
        if (process.ExitCode != 0) throw new IOException("La operación de cuenta no pudo completarse.");
        return result;
    }
}
