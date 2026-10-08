using System.Diagnostics;
using System.IO;
using System.Text;

namespace BloodborneLauncher;

public sealed class SessionRunner
{
    public bool Running { get; private set; }
    public string LogPath { get; private set; } = "";
    public async Task<int> Run(NativeHost host, NativeState state, PreparedGame game, bool detailed, bool console, Action? started = null)
    {
        if (Running) throw new InvalidOperationException("Bloodborne ya está abierto.");
        Running = true;
        try {
            string logs = Path.Combine(host.Root, "logs"); Directory.CreateDirectory(logs);
            LogPath = Path.Combine(logs, $"bbhost-{DateTime.Now:yyyy-MM-dd_HH-mm-ss}-{Guid.NewGuid():N}.log");
            using var file = new StreamWriter(new FileStream(LogPath, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
            foreach (var old in new DirectoryInfo(logs).GetFiles("bbhost-*.log").OrderByDescending(x => x.CreationTimeUtc).Skip(10)) {
                try { old.Delete(); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
            await file.WriteLineAsync($"Bloodborne PC / bbhost {state.Version} ({state.Commit})");
            await file.WriteLineAsync($"Game {game.TitleId} {game.Version}; verified ELF {game.Sha256}");
            await file.WriteLineAsync("Vulkan GPU: " + state.GpuDescription);
            var config = TomlSettings.Read(host.Profile.Length > 0 ? host.Profile : state.ConfigFile);
            foreach (var plugin in config.Where(x => x.Key.StartsWith("plugins.") && x.Value == "true")) await file.WriteLineAsync("Enabled " + plugin.Key);
            // Log only non-secret display/effect choices, never raw TOML or account.
            foreach (var option in state.Options) await file.WriteLineAsync($"Setting {option.Key}={option.Values[option.Index]}");
            var info = host.StartInfo("--app0", game.App0, "--eboot", game.Eboot, "--data", state.Data);
            info.CreateNoWindow = !console;
            // A user's old environment must never bypass the new compatibility gate.
            info.Environment.Remove("BBHOST_ANY_EBOOT");
            if (detailed) {
                info.Environment["BBHOST_GPU_PROFILE"] = "1";
                info.Environment["BBHOST_AUDIO_STATS"] = "1";
                // Native per-second counters distinguish CPU waits, streaming
                // and shader work. No frame readbacks or capture are enabled.
                info.Environment["BBHOST_FRAME_STATS"] = "1";
                info.Environment["BBHOST_STALL_MS"] = "40";
            }
            using var process = Process.Start(info) ?? throw new IOException("No se pudo iniciar Bloodborne.");
            started?.Invoke();
            var gate = new SemaphoreSlim(1, 1);
            async Task Drain(StreamReader reader) {
                while (await reader.ReadLineAsync() is { } line) {
                    await gate.WaitAsync();
                    try { await file.WriteLineAsync(line); } finally { gate.Release(); }
                }
            }
            var output = Drain(process.StandardOutput);
            var error = Drain(process.StandardError);
            await process.WaitForExitAsync();
            await Task.WhenAll(output, error);
            await file.WriteLineAsync($"Exit code: {process.ExitCode}");
            return process.ExitCode;
        } finally { Running = false; }
    }
}
