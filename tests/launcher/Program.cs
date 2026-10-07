using BloodborneLauncher;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

static class Program
{
    static int checks;
    static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    static void Check(bool ok, string text) { if (!ok) throw new Exception(text); ++checks; }
    static void Throws(Action action, string text) { try { action(); } catch (IOException) { ++checks; return; } throw new Exception(text); }
    static void Pump() { var frame = new DispatcherFrame(); Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => frame.Continue = false)); Dispatcher.PushFrame(frame); }
    static void Until(Func<bool> done, string why) { var end = DateTime.UtcNow.AddSeconds(15); while (!done()) { if (DateTime.UtcNow > end) throw new Exception("Timed out: " + why); Pump(); Thread.Sleep(10); } }
    static void Call(MainWindow w, string name, params object[] args) => typeof(MainWindow).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(w, args);
    static T Find<T>(MainWindow w, string name) where T : class => (T)w.FindName(name);
    static void Render(MainWindow w, string file, double width, double height) {
        w.Width = width; w.Height = height; w.UpdateLayout(); Pump();
        var element = (FrameworkElement)w.Content;
        element.Measure(new Size(width - 2, height - 2)); element.Arrange(new Rect(0, 0, width - 2, height - 2)); element.UpdateLayout();
        var bitmap = new RenderTargetBitmap((int)width - 2, (int)height - 2, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(element); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = File.Create(file); encoder.Save(output);
    }
    static int FakeHost(string[] args) {
        var root = AppContext.BaseDirectory;
        string data = Path.Combine(root, "data"), config = Path.Combine(root, "config", "bbhost.toml"), options = Path.Combine(root, "config", "bbhost-options.toml");
        if (args.Contains("--launcher-gpu")) { Console.WriteLine("{\"ok\":true,\"name\":\"Fixture GPU\",\"driver\":\"test only\"}"); return 0; }
        if (args.Contains("--prepare-game")) {
            Console.WriteLine(JsonSerializer.Serialize(new PreparedGame { Ok = true, App0 = Path.Combine(root, "Test game"), Eboot = Path.Combine(data, "cache", "eboot.elf"), Version = "01.09", TitleId = "TEST-FIXTURE", Sha256 = "test-fixture-only", Prepared = true }, JsonOptions)); return 0;
        }
        if (args.Contains("--launcher-state")) {
            Console.WriteLine(JsonSerializer.Serialize(new NativeState {
                Ok = true, Version = "test-fixture", Commit = "fixture", ConfigFile = config, OptionsFile = options,
                App0 = Path.Combine(root, "Test game"), Data = data, Mods = Path.Combine(data, "mods"), Account = "Not signed in",
                Options = [
                    new() { Key = "resolution", Label = "Resolution", Section = "GRAPHICS", Values = ["1920x1080", "2560x1440", "3840x2160"], Index = 0 },
                    new() { Key = "window_mode", Label = "Window mode", Section = "DISPLAY", Values = ["Windowed", "Fullscreen"] },
                    new() { Key = "frame_cap", Label = "Frame cap", Section = "DISPLAY", Values = ["30", "60", "90", "120", "144", "Off"], Index = 1 },
                    new() { Key = "upscaler", Label = "Upscaling", Section = "UPSCALING", Values = ["Native / Off", "FSR 1"], Index = 1 },
                    new() { Key = "ssao", Label = "Ambient occlusion", Section = "GRAPHICS", Values = ["On", "Off"] },
                    new() { Key = "change_appearance", Label = "Hunter's Dream mirror", Section = "PC ENHANCEMENTS", Values = ["On", "Off"], Restart = true }
                ]
            }, JsonOptions)); return 0;
        }
        // A crashing fake child with both output pipes active tests supervision.
        Thread.Sleep(600); Console.Out.WriteLine(new string('o', 80000)); Console.Error.WriteLine("fixture crash reason"); Thread.Sleep(200); return 3;
    }
    [STAThread] static int Main(string[] args) {
        if (args.Any(x => x.StartsWith("--"))) return FakeHost(args);
        try { Run(args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "bbhost-launcher-previews")); Console.WriteLine($"launcher checks: {checks} passed"); return 0; }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    static void Run(string preview) {
        Directory.CreateDirectory(preview);
        string root = Path.Combine(Path.GetTempPath(), "bbhost-launcher-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        // Copy only test binaries into a disposable runtime. No game, user's
        // installed launcher, saves, accounts or runtime are reachable here.
        foreach (var file in Directory.EnumerateFiles(AppContext.BaseDirectory)) File.Copy(file, Path.Combine(root, Path.GetFileName(file)));
        File.Copy(Path.Combine(root, "LauncherChecks.exe"), Path.Combine(root, "bbhost.exe"));
        Directory.CreateDirectory(Path.Combine(root, "config")); Directory.CreateDirectory(Path.Combine(root, "data"));
        string config = Path.Combine(root, "config", "bbhost.toml"), options = Path.Combine(root, "config", "bbhost-options.toml");
        File.WriteAllText(config, "# preserve\n[plugins]\nunknown_plugin = true\n");
        File.WriteAllText(options, "# preserve option comment\n[options]\nversion = \"2\"\n[account]\nname = \"fixture\"\ntoken = \"fixture-token-must-survive\"\n[keys]\nattack = \"K\"\n");
        var fingerprint = TomlSettings.Fingerprint(options);
        TomlSettings.Write(options, new Dictionary<string, string> { ["options.ssao"] = "\"Off\"" }, fingerprint);
        Check(File.ReadAllText(options).Contains("fixture-token-must-survive") && File.ReadAllText(options).Contains("attack = \"K\"") && File.ReadAllText(options).Contains("# preserve option comment"), "preserve account, bindings and comments");
        Throws(() => TomlSettings.Write(options, new Dictionary<string, string> { ["options.ssao"] = "\"On\"" }, fingerprint), "stale config must refuse writes");
        string old = Path.Combine(root, "old-saves", "SPRJ0005"); Directory.CreateDirectory(old); File.WriteAllText(Path.Combine(old, "save.bin"), "fixture");
        SaveSafety.Import(Path.GetDirectoryName(old)!, Path.Combine(root, "import-target"));
        Check(File.Exists(Path.Combine(old, "save.bin")), "import preserves old saves");
        Throws(() => SaveSafety.Import(Path.GetDirectoryName(old)!, Path.Combine(root, "import-target")), "import refuses destination collision");
        Check(File.ReadAllText(Path.Combine(root, "import-target", "saves", "SPRJ0005", "save.bin")) == "fixture", "collision preserved destination");
        Check(File.Exists(SaveSafety.Backup(Path.Combine(root, "import-target"))), "save backup generated");
        var host = new NativeHost(root); var info = host.StartInfo("--app0", "C:\\Game folder with spaces\\Bloodborne");
        Check(!info.UseShellExecute && info.CreateNoWindow && info.ArgumentList.Contains("C:\\Game folder with spaces\\Bloodborne") && !info.FileName.EndsWith("cmd.exe"), "launch directly with safe arguments and no console");
        Environment.SetEnvironmentVariable("BBHOST_LAUNCHER_ROOT", root);
        Environment.SetEnvironmentVariable("BBHOST_CONFIG_DIR", Path.Combine(root, "config"));
        var app = new App(); app.InitializeComponent();
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(Dispatcher.CurrentDispatcher));
        var window = new MainWindow { ShowInTaskbar = false, ShowActivated = false, WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = -20000 };
        window.Show();
        Until(() => Find<Button>(window, "PlayButton").IsEnabled, "native state and preparation");
        Check(Find<Image>(window, "HeroImage").Source is BitmapSource, "embedded artwork");
        Check(Find<ComboBox>(window, "QuickUpscaler").Items.Count == 2, "only working native providers selectable");
        Check(Find<ComboBox>(window, "ServerCombo").Items.Cast<ComboBoxItem>().Select(x => x.Content.ToString()).SequenceEqual(new[] { "Offline", "Custom Server" }), "no Hunter's Dream preset");
        Check(TomlSettings.Read(config)["launcher.server_kind"] == "Offline" && TomlSettings.Read(config)["update.check"] == "false", "safe offline first start");
        Check(Find<StackPanel>(window, "GameOptions").Children.Count == 1, "engine enhancements from native schema");
        Find<ComboBox>(window, "QuickResolution").SelectedIndex = 1; Call(window, "SaveSettings");
        Check(TomlSettings.Read(options)["options.resolution"] == "2560x1440", "quick control writes bbhost settings");
        Check(File.ReadAllText(options).Contains("fixture-token-must-survive"), "WPF save preserves account");
        Render(window, Path.Combine(preview, "launcher-home.png"), 1500, 920);
        var nav = Find<StackPanel>(window, "Navigation");
        foreach (string page in new[] { "Game", "Graphics", "Online", "Upscaling" }) {
            var button = nav.Children.OfType<Button>().First(x => (string)x.CommandParameter == page); Call(window, "Nav_Click", button, new RoutedEventArgs());
            Render(window, Path.Combine(preview, "launcher-" + page.ToLowerInvariant() + ".png"), 1200, 760);
        }
        Call(window, "Play_Click", window, new RoutedEventArgs());
        Until(() => !window.IsVisible, "launcher disappears after game process starts"); Check(!window.IsVisible, "launcher hidden during session");
        Until(() => window.IsVisible && Find<Button>(window, "PlayButton").IsEnabled, "launcher reopens after child crash"); Check(window.IsVisible, "launcher restored after child crash");
        string log = File.ReadAllText(Directory.GetFiles(Path.Combine(root, "logs"), "bbhost-*.log").Single());
        Check(log.Contains("fixture crash reason") && log.Contains("Exit code: 3") && !log.Contains("fixture-token-must-survive"), "crash logs preserved without account token");
        window.Close();
    }
}
