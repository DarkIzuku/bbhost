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
        if (args.Contains("--launcher-account")) {
            File.AppendAllText(Path.Combine(root, "fixture-account-requests.txt"), "request\n");
            Console.WriteLine("{\"account\":\"Fixture\",\"outcome\":\"ok\",\"detail\":\"fixture account only\"}"); return 0;
        }
        if (args.Contains("--launcher-state")) {
            Console.WriteLine(JsonSerializer.Serialize(new NativeState {
                Ok = true, Version = "test-fixture", Commit = "fixture", ConfigFile = config, OptionsFile = options,
                App0 = Path.Combine(root, "Test game"), Data = data, Mods = Path.Combine(data, "mods"), Account = "Not signed in", LegacyDebugMenu = true,
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
        Thread.Sleep(600); Console.Out.WriteLine(new string('o', 80000)); Console.Error.WriteLine("fixture crash reason");
        Console.WriteLine("fixture frame stats=" + Environment.GetEnvironmentVariable("BBHOST_FRAME_STATS"));
        Console.WriteLine("fixture stall threshold=" + Environment.GetEnvironmentVariable("BBHOST_STALL_MS"));
        Thread.Sleep(200); return 3;
    }
    [STAThread] static int Main(string[] args) {
        if (args.Length == 4 && args[0] == "integration") {
            try { Integration(args[1], args[2], args[3]); return 0; } catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
        }
        if (args.Any(x => x.StartsWith("--"))) return FakeHost(args);
        try { Run(args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "bbhost-launcher-previews")); Console.WriteLine($"launcher checks: {checks} passed"); return 0; }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    static void Integration(string runtime, string gameFolder, string testRoot) {
        runtime = Path.GetFullPath(runtime); testRoot = Path.GetFullPath(testRoot);
        // Opt-in local game check. All writable state is in the specified test
        // root; the supplied game folder is only read by native preparation.
        Environment.SetEnvironmentVariable("BBHOST_LAUNCHER_ROOT", runtime);
        Environment.SetEnvironmentVariable("BBHOST_CONFIG_DIR", Path.Combine(testRoot, "config"));
        Environment.SetEnvironmentVariable("BBHOST_EXIT_FLIP", "240");
        Environment.SetEnvironmentVariable("BBHOST_DUMP_FRAME", "180,220");
        Environment.SetEnvironmentVariable("BBHOST_NP_SIGNED_OUT", "1");
        Environment.SetEnvironmentVariable("BBHOST_SKIP_INTRO", "1");
        var config = new Dictionary<string, string> { ["paths.app0"] = TomlSettings.Quote(gameFolder), ["paths.data"] = TomlSettings.Quote(Path.Combine(testRoot, "data")), ["startup.skip_intro"] = "true" };
        TomlSettings.Write(Path.Combine(testRoot, "config", "bbhost.toml"), config);
        var app = new App(); app.InitializeComponent();
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(Dispatcher.CurrentDispatcher));
        var window = new MainWindow { ShowInTaskbar = false, ShowActivated = false, WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = -20000 };
        window.Show(); Until(() => Find<Button>(window, "PlayButton").IsEnabled, "real native preparation and GPU query");
        if (Environment.GetEnvironmentVariable("BBHOST_TEST_RES") is { } size) {
            var resolution = Find<ComboBox>(window, "QuickResolution");
            resolution.SelectedItem = size;
            if (resolution.SelectedIndex < 0) throw new Exception("Unsupported test resolution.");
        }
        if (Environment.GetEnvironmentVariable("BBHOST_TEST_RCAS") is { } rcas) {
            var field = typeof(MainWindow).GetField("state", BindingFlags.NonPublic | BindingFlags.Instance)!;
            var nativeState = (NativeState)field.GetValue(window)!;
            nativeState.Options.First(x => x.Key == "upscale_rcas").Index = rcas == "0" ? 0 : 1;
        }
        if (Environment.GetEnvironmentVariable("BBHOST_TEST_FULLSCREEN") == "1") Find<ComboBox>(window, "QuickDisplay").SelectedIndex = 1;
        if (Environment.GetEnvironmentVariable("BBHOST_TEST_FPS") is { } fps) Find<ComboBox>(window, "QuickFps").SelectedItem = fps;
        Console.WriteLine(Find<TextBlock>(window, "GameVersionStatus").Text);
        Console.WriteLine(Find<TextBlock>(window, "GpuStatus").Text);
        Console.WriteLine("Native option cards: " + new[] { "GameOptions", "DisplayOptions", "GraphicsOptions", "UpscalingOptions", "ControlsOptions" }.Sum(x => Find<StackPanel>(window, x).Children.Count));
        var nav = Find<StackPanel>(window, "Navigation").Children.OfType<Button>().First(x => (string)x.CommandParameter == "Graphics");
        Call(window, "Nav_Click", nav, new RoutedEventArgs()); Directory.CreateDirectory(testRoot);
        Render(window, Path.Combine(testRoot, "real-launcher-graphics.png"), 1500, 920);
        Call(window, "Play_Click", window, new RoutedEventArgs()); Until(() => !window.IsVisible, "real game starts and launcher hides");
        var deadline = DateTime.UtcNow.AddSeconds(60);
        while (!window.IsVisible || !Find<Button>(window, "PlayButton").IsEnabled) { if (DateTime.UtcNow > deadline) throw new Exception("Real game did not exit at the requested flip count."); Pump(); Thread.Sleep(10); }
        var logFile = Directory.GetFiles(Path.Combine(runtime, "logs"), "bbhost-*.log").OrderByDescending(File.GetLastWriteTimeUtc).First();
        var log = File.ReadAllText(logFile);
        Console.WriteLine("Launcher restored; log: " + logFile);
        if (!log.Contains("Exit code: 0")) throw new Exception("Real runtime reported a failure; inspect " + logFile);
        window.Close(); Console.WriteLine("real launcher/runtime integration: passed (240 flips, exit 0)");
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
        Directory.CreateDirectory(Path.Combine(root, "plugins")); File.WriteAllText(Path.Combine(root, "plugins", "debug_menu.dll"), "fixture only, never loaded");
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
        Check(Find<StackPanel>(window, "PluginOptions").Children.OfType<CheckBox>().Single(x => x.Content.ToString()!.StartsWith("Debug Menu")).IsChecked == true,
              "old debug menu choice migrates to official plugin before saving");
        Find<ComboBox>(window, "QuickResolution").SelectedIndex = 1; Call(window, "SaveSettings");
        Check(TomlSettings.Read(options)["options.resolution"] == "2560x1440", "quick control writes bbhost settings");
        Check(File.ReadAllText(options).Contains("fixture-token-must-survive"), "WPF save preserves account");
        Find<TextBox>(window, "CustomServerBox").Text = "http://192.0.2.15:31315";
        Call(window, "ShadNetPage_Click", window, new RoutedEventArgs()); Call(window, "SaveSettings");
        var profile = Directory.GetFiles(Path.Combine(root, "config", "profiles"), "bloodborne-offline.toml").Single();
        Check(TomlSettings.Read(profile)["plugins.debug_menu"] == "true", "debug menu plugin enable persisted in native profile");
        Find<StackPanel>(window, "PluginOptions").Children.OfType<CheckBox>().Single().IsChecked = false; Call(window, "SaveSettings"); Call(window, "BuildMods");
        Check(Find<StackPanel>(window, "PluginOptions").Children.OfType<CheckBox>().Single().IsChecked == false,
              "explicit plugin disable takes precedence over legacy debug choice");
        Check(TomlSettings.Read(profile)["online.account_page"] == "http://192.0.2.15:31316/register", "integrated shadNet registration uses separate website listener and native TOML");
        Check(TomlSettings.Read(profile)["online.offline"] == "true", "website link cannot enable incompatible game protocol");
        Check(ServerLinks.ShadNetRegistration("https://[2001:db8::1]:31315").AbsoluteUri == "https://[2001:db8::1]:31316/register", "website link preserves IPv6 and explicit TLS scheme");
        Throws(() => ServerLinks.ShadNetRegistration("https://user:password@example.com"), "website helper rejects embedded credentials");
        var custom = ServerProfile.Build(false, "http://192.0.2.15:31315", "https://api.example.com/bbhost/", "https://auth.example.com/",
                                         "https://accounts.example.com/register?from=launcher", "Fixture", "9308", "192.0.2.100", "stun.example.com:3479", true, true);
        Check(custom.Values["online.np_server"] == "\"https://api.example.com/bbhost\"" && custom.Values["online.auth_server"] == "\"https://auth.example.com\"",
              "independent native account and matchmaking bases preserve proxy path and custom ports");
        var direct = ServerProfile.Build(false, "http://192.0.2.15:31315", "", "", "", "Fixture", "9307", "", "off", false, false);
        Check(direct.Values["online.np_server"] == "\"http://192.0.2.15:31315\"", "native API fallback cannot append a second port");
        var differentAuth = ServerProfile.Build(false, "http://192.0.2.15:31315", "https://api.example.com/bbhost/", "https://other-auth.example.com",
                                               "", "Fixture", "9308", "", "", true, true);
        Check(custom.Name != differentAuth.Name, "different account services cannot reuse a native account profile");
        Throws(() => ServerProfile.Build(false, "http://user:password@example.com", "", "", "", "Fixture", "9307", "", "", true, true), "server profile rejects URL credentials");
        Throws(() => ServerProfile.Build(false, "http://example.com", "", "", "", "Fixture", "65536", "", "", true, true), "server profile rejects invalid P2P port");
        Throws(() => ServerProfile.Build(false, "http://example.com", "", "", "", "Fixture", "9307", "", "stun.example.com:65536", true, true), "server profile rejects invalid STUN port");
        string overrideFile = Path.Combine(root, "fixture-host-overrides.json");
        File.WriteAllText(overrideFile, "{\"https://ss4.scej-network.jp:20443\":\"http://192.0.2.15:31315\"}");
        string originalOverride = TomlSettings.Fingerprint(overrideFile);
        Call(window, "ImportHostOverride", overrideFile);
        Check(Find<TextBox>(window, "CustomServerBox").Text == "http://192.0.2.15:31315" && Find<TextBox>(window, "AccountPageBox").Text == "http://192.0.2.15:31316/register", "host override imports WebAPI and integrated account page");
        Check(TomlSettings.Fingerprint(overrideFile) == originalOverride && TomlSettings.Read(profile)["online.offline"] == "true", "host override import is read only and does not enable online");
        Find<ComboBox>(window, "ServerCombo").SelectedIndex = 1;
        Find<TextBox>(window, "NativeApiBox").Text = "https://api.example.com/bbhost";
        Find<TextBox>(window, "AuthServerBox").Text = "https://auth.example.com";
        Find<TextBox>(window, "OnlineIdBox").Text = "Fixture";
        Find<TextBox>(window, "P2pPortBox").Text = "9308";
        Find<TextBox>(window, "P2pAddressBox").Text = "192.0.2.100";
        Find<TextBox>(window, "StunServerBox").Text = "stun.example.com:3479";
        string accountRequests = Path.Combine(root, "fixture-account-requests.txt");
        Call(window, "AccountAction", "recover");
        Check(!File.Exists(accountRequests) && Find<TextBlock>(window, "FooterMessage").Text.StartsWith("Aplica primero este servidor"),
              "unapplied custom server cannot submit account data through the old offline profile");
        Call(window, "ApplyServer_Click", window, new RoutedEventArgs());
        Until(() => Find<TextBlock>(window, "FooterMessage").Text.StartsWith("Perfil de servidor aplicado"), "native server profile applied and reloaded");
        var selectedProfile = Path.Combine(root, "config", "profiles", TomlSettings.Read(config)["launcher.server_profile"] + ".toml");
        var network = TomlSettings.Read(selectedProfile);
        Check(network["online.np_server"] == "https://api.example.com/bbhost" && network["online.auth_server"] == "https://auth.example.com" && network["online.host"] == "192.0.2.15:31315", "WPF applies separate endpoints to native TOML");
        Check(network["online.online_id"] == "Fixture" && network["online.p2p_port"] == "9308" && network["online.p2p_addr"] == "192.0.2.100" && network["online.stun_server"] == "stun.example.com:3479", "WPF persists native identity and P2P/STUN settings");
        Check(Find<TextBox>(window, "NativeApiBox").Text == network["online.np_server"] && Find<TextBox>(window, "P2pAddressBox").Text == network["online.p2p_addr"], "network settings round trip through reload");
        Call(window, "AccountAction", "recover");
        Until(() => File.Exists(accountRequests) && Find<Button>(window, "PlayButton").IsEnabled, "account access through applied server profile");
        Check(File.ReadAllLines(accountRequests).Length == 1, "applied profile still supports native account requests");
        Find<TextBox>(window, "AuthServerBox").Text = "https://other-auth.example.com";
        Call(window, "AccountAction", "recover");
        Check(File.ReadAllLines(accountRequests).Length == 1 && Find<TextBlock>(window, "FooterMessage").Text.StartsWith("Aplica primero este servidor"),
              "editing the account service cannot submit credentials to the previous server");
        Find<TextBox>(window, "AuthServerBox").Text = network["online.auth_server"];
        Find<CheckBox>(window, "VerifyTlsCheck").IsChecked = false;
        Call(window, "AccountAction", "recover");
        Check(File.ReadAllLines(accountRequests).Length == 1 && Find<TextBlock>(window, "FooterMessage").Text.StartsWith("Aplica primero este servidor"),
              "unapplied network security settings block account requests");
        Find<CheckBox>(window, "VerifyTlsCheck").IsChecked = true;
        Find<ComboBox>(window, "ServerCombo").SelectedIndex = 0;
        Call(window, "ApplyServer_Click", window, new RoutedEventArgs());
        Until(() => TomlSettings.Read(config)["launcher.server_kind"] == "Offline" && Find<Button>(window, "PlayButton").IsEnabled, "return to offline profile");
        Check(TomlSettings.Read(profile)["online.offline"] == "true", "offline profile still applies after custom server configuration");
        Render(window, Path.Combine(preview, "launcher-home.png"), 1500, 920);
        var nav = Find<StackPanel>(window, "Navigation");
        foreach (string page in new[] { "Game", "Graphics", "Online", "Upscaling" }) {
            var button = nav.Children.OfType<Button>().First(x => (string)x.CommandParameter == page); Call(window, "Nav_Click", button, new RoutedEventArgs());
            Render(window, Path.Combine(preview, "launcher-" + page.ToLowerInvariant() + ".png"), 1200, 760);
        }
        Find<CheckBox>(window, "DetailedLogsCheck").IsChecked = true;
        Call(window, "Play_Click", window, new RoutedEventArgs());
        Until(() => !window.IsVisible, "launcher disappears after game process starts"); Check(!window.IsVisible, "launcher hidden during session");
        Until(() => window.IsVisible && Find<Button>(window, "PlayButton").IsEnabled, "launcher reopens after child crash"); Check(window.IsVisible, "launcher restored after child crash");
        string log = File.ReadAllText(Directory.GetFiles(Path.Combine(root, "logs"), "bbhost-*.log").Single());
        Check(log.Contains("fixture crash reason") && log.Contains("Exit code: 3") && !log.Contains("fixture-token-must-survive"), "crash logs preserved without account token");
        Check(log.Contains("fixture frame stats=1") && log.Contains("fixture stall threshold=40"), "detailed logs request native frame and stall counters from the child");
        window.Close();
    }
}
