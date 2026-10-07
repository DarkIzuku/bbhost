using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace BloodborneLauncher;

public partial class MainWindow : Window
{
    readonly NativeHost host;
    readonly SessionRunner session = new();
    NativeState? state;
    PreparedGame? game;
    readonly Dictionary<string, List<ComboBox>> selectors = new();
    readonly Dictionary<string, CheckBox> plugins = new(), patches = new();
    readonly Dictionary<string, string> fingerprints = new();
    bool syncing = true, busy, dirty, initialized;

    public MainWindow()
    {
        InitializeComponent();
        host = new NativeHost(Environment.GetEnvironmentVariable("BBHOST_LAUNCHER_ROOT") ?? AppContext.BaseDirectory);
        var area = SystemParameters.WorkArea;
        MinWidth = Math.Min(MinWidth, Math.Max(640, area.Width - 24));
        MinHeight = Math.Min(MinHeight, Math.Max(480, area.Height - 24));
        Width = Math.Min(Width, area.Width - 24); Height = Math.Min(Height, area.Height - 24);
        SizeChanged += (_, _) => QuickSettingsGrid.Columns = ActualWidth < 1200 ? 2 : 4;
        Closing += (_, e) => { if (session.Running || busy) { e.Cancel = true; WindowState = WindowState.Minimized; } };
        Loaded += async (_, _) => {
            if (initialized) return;
            initialized = true; SetBusy(true);
            try {
                string configDir = Environment.GetEnvironmentVariable("BBHOST_CONFIG_DIR") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "bbhost");
                var main = TomlSettings.Read(Path.Combine(configDir, "bbhost.toml"));
                if (!main.ContainsKey("launcher.server_kind")) {
                    // New installs start offline, without contacting an upstream server.
                    TomlSettings.Write(Path.Combine(configDir, "profiles", "bloodborne-offline.toml"), new Dictionary<string, string> { ["online.offline"] = "true" });
                    TomlSettings.Write(Path.Combine(configDir, "bbhost.toml"), new Dictionary<string, string> {
                        ["launcher.server_kind"] = "\"Offline\"", ["launcher.server_profile"] = "\"bloodborne-offline\"", ["update.check"] = "false"
                    });
                }
                main = TomlSettings.Read(Path.Combine(configDir, "bbhost.toml"));
                string profile = main.GetValueOrDefault("launcher.server_profile", "");
                if (Regex.IsMatch(profile, @"^(bloodborne-offline|bloodborne-playtest|bloodborne-custom-[A-F0-9]{16})$")) {
                    string file = Path.Combine(configDir, "profiles", profile + ".toml"); if (File.Exists(file)) host.Profile = file;
                }
                await Reload();
            } catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
        };
    }

    async Task Reload()
    {
        syncing = true;
        if (!File.Exists(host.Executable)) {
            RuntimeStatus.Text = "bbhost.exe no está en esta carpeta"; PlayHint.Text = "Usa el paquete completo de Windows.";
            SaveButton.IsEnabled = false; syncing = false; return;
        }
        state = await host.Query<NativeState>("--launcher-state");
        if (!state.Ok) throw new IOException(state.Error);
        RuntimeStatus.Text = $"bbhost {state.Version} · {state.Commit}";
        BuildStatus.Text = RuntimeStatus.Text;
        AccountStatus.Text = state.Account;
        DataBox.Text = state.Data; PathBox.Text = state.App0;
        SavesPath.Text = Path.Combine(state.Data, "saves", "SPRJ0005");
        ModsFolderText.Text = state.Mods;
        var main = TomlSettings.Read(state.ConfigFile);
        var active = string.IsNullOrEmpty(host.Profile) ? main : TomlSettings.Read(host.Profile);
        QuickReentry.IsChecked = active.GetValueOrDefault("loading.quick_reentry", main.GetValueOrDefault("loading.quick_reentry", "true")) == "true";
        AllPostProcessors.IsChecked = active.GetValueOrDefault("streaming.all_post_processors", main.GetValueOrDefault("streaming.all_post_processors", "false")) == "true";
        P2pPortBox.Text = active.GetValueOrDefault("online.p2p_port", main.GetValueOrDefault("online.p2p_port", "9307"));
        string kind = main.GetValueOrDefault("launcher.server_kind", "Offline");
        ServerCombo.SelectedIndex = kind == "Custom Server" ? 1 : 0;
        string server = active.GetValueOrDefault("online.host", main.GetValueOrDefault("online.host", ""));
        CustomServerBox.Text = server.Contains("thehuntersdream.com", StringComparison.OrdinalIgnoreCase) || server.Length == 0 ? "" : active.GetValueOrDefault("online.scheme", "https") + "://" + server;
        AccountPageBox.Text = active.GetValueOrDefault("online.account_page", "");
        DetailedLogsCheck.IsChecked = main.GetValueOrDefault("launcher.detailed_logs", "false") == "true";
        DeveloperConsoleCheck.IsChecked = main.GetValueOrDefault("launcher.developer_console", "false") == "true";
        string language = main.GetValueOrDefault("system.language", "1");
        LanguageCombo.SelectedItem = LanguageCombo.Items.Cast<ComboBoxItem>().FirstOrDefault(x => (string?)x.Tag == language) ?? LanguageCombo.Items[0];
        BuildNativeOptions(); BuildMods();
        fingerprints.Clear();
        foreach (var file in new[] { state.ConfigFile, state.OptionsFile, host.Profile }.Where(x => x.Length > 0).Distinct()) fingerprints[file] = TomlSettings.Fingerprint(file);
        dirty = false; syncing = false;
        if (state.App0.Length > 0) await Prepare(state.App0);
        else { game = null; GameVersionStatus.Text = "Selecciona Bloodborne 1.09"; PlayHint.Text = "Selecciona una carpeta de Bloodborne. El ejecutable se encuentra y prepara automáticamente."; }
        // Native Vulkan probe, never infer provider availability from a brand name.
        try {
            var gpu = await host.Query<GpuInfo>("--launcher-gpu");
            GpuStatus.Text = gpu.Ok ? gpu.Name + " · " + gpu.Driver : "Vulkan: " + gpu.Error;
            state.GpuDescription = $"{GpuStatus.Text}; vendor={gpu.Vendor}; device={gpu.Device}; api={gpu.ApiVersion}; driver={gpu.DriverVersion}";
        } catch (Exception ex) { GpuStatus.Text = "GPU pendiente de consulta: " + ex.Message; }
        FooterGpu.Text = "   |   " + GpuStatus.Text;
        UpdateReady();
    }

    sealed class GpuInfo {
        public bool Ok { get; set; } public string Name { get; set; } = ""; public string Driver { get; set; } = ""; public string Error { get; set; } = "";
        public uint Vendor { get; set; } public uint Device { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("api_version")] public uint ApiVersion { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("driver_version")] public uint DriverVersion { get; set; }
    }

    void BuildNativeOptions()
    {
        selectors.Clear();
        foreach (var panel in new[] { GameOptions, DisplayOptions, GraphicsOptions, ControlsOptions, UpscalingOptions }) panel.Children.Clear();
        foreach (var option in state!.Options) {
            StackPanel target = option.Key == "resolution" || option.Section == "DISPLAY" ? DisplayOptions : option.Section == "GRAPHICS" ? GraphicsOptions : option.Section == "INPUT" ? ControlsOptions : option.Section == "UPSCALING" ? UpscalingOptions : GameOptions;
            var panel = new StackPanel();
            panel.Children.Add(new TextBlock { Text = option.Label + (option.Restart ? " · requiere reinicio" : ""), FontSize = 16, FontFamily = new System.Windows.Media.FontFamily("Georgia") });
            var combo = new ComboBox { ItemsSource = option.Values, SelectedIndex = option.Index, Tag = option.Key, MaxWidth = 440, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 10, 0, 8) };
            panel.Children.Add(combo);
            panel.Children.Add(new TextBlock { Text = option.Note, TextWrapping = TextWrapping.Wrap, Foreground = (System.Windows.Media.Brush)FindResource("MutedBrush") });
            target.Children.Add(new Border { Style = (Style)FindResource("CardStyle"), Child = panel, Margin = new Thickness(0, 0, 0, 12) });
            selectors[option.Key] = [combo]; combo.SelectionChanged += QuickSetting_Changed;
        }
        foreach (var quick in new[] { QuickResolution, QuickDisplay, QuickUpscaler, QuickFps }) {
            string key = (string)quick.Tag;
            var option = state.Options.FirstOrDefault(x => x.Key == key);
            if (option is null) { quick.IsEnabled = false; continue; }
            quick.ItemsSource = option.Values; quick.SelectedIndex = option.Index; quick.IsEnabled = true;
            selectors[key].Add(quick);
        }
        UpdateFps();
    }

    void BuildMods()
    {
        PluginOptions.Children.Clear(); PatchOptions.Children.Clear(); plugins.Clear(); patches.Clear();
        var config = TomlSettings.Read(string.IsNullOrEmpty(host.Profile) ? state!.ConfigFile : host.Profile);
        var names = new HashSet<string>();
        foreach (var folder in new[] { Path.Combine(host.Root, "plugins"), Path.Combine(Path.GetDirectoryName(state!.ConfigFile)!, "plugins"), Path.Combine(state.Data, "plugins") }) {
            if (!Directory.Exists(folder)) continue;
            foreach (var file in Directory.EnumerateFiles(folder, "*.dll")) names.Add(Path.GetFileNameWithoutExtension(file));
        }
        foreach (var name in names.Order()) {
            if (!Regex.IsMatch(name, @"^[A-Za-z0-9_-]+$")) continue;
            var check = new CheckBox { Content = name, IsChecked = config.GetValueOrDefault("plugins." + name, "false") == "true", Margin = new Thickness(0, 6, 0, 6) };
            plugins[name] = check; PluginOptions.Children.Add(check); check.Checked += Preference_Changed; check.Unchecked += Preference_Changed;
        }
        foreach (var patch in state.Patches) {
            var check = new CheckBox { Content = patch.Name, ToolTip = patch.Description, IsChecked = patch.Enabled, IsEnabled = patch.Option.Length == 0, Margin = new Thickness(0, 6, 0, 6) };
            if (patch.Option.Length > 0) check.Content = patch.Name + " · sigue la opción del juego";
            patches[patch.Name] = check; PatchOptions.Children.Add(check); check.Checked += Preference_Changed; check.Unchecked += Preference_Changed;
        }
    }

    void QuickSetting_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || sender is not ComboBox combo || combo.SelectedIndex < 0 || state is null) return;
        string key = (string)combo.Tag;
        var option = state.Options.First(x => x.Key == key); option.Index = combo.SelectedIndex;
        syncing = true; foreach (var other in selectors[key]) other.SelectedIndex = option.Index; syncing = false;
        Preference_Changed(sender, e); UpdateFps();
    }
    void Preference_Changed(object sender, RoutedEventArgs e) { if (!syncing) { dirty = true; FooterMessage.Text = "Opciones sin guardar"; } }
    void UpdateFps() { var o = state?.Options.FirstOrDefault(x => x.Key == "frame_cap"); if (o is not null) FooterFps.Text = "   |   " + (o.Values[o.Index] == "Off" ? "Sin límite" : o.Values[o.Index] + " FPS"); }

    void SaveSettings()
    {
        if (state is null || busy || session.Running) return;
        foreach (var entry in fingerprints) if (TomlSettings.Fingerprint(entry.Key) != entry.Value) throw new IOException("bbhost cambió las opciones. Recarga antes de guardar para conservar los cambios del juego.");
        var main = new Dictionary<string, string> {
            ["bbhost.config_version"] = "3", ["startup.setup_window"] = "false",
            ["paths.data"] = TomlSettings.Quote(DataBox.Text),
            ["system.language"] = ((ComboBoxItem)LanguageCombo.SelectedItem).Tag.ToString()!,
            ["launcher.detailed_logs"] = DetailedLogsCheck.IsChecked == true ? "true" : "false",
            ["launcher.developer_console"] = DeveloperConsoleCheck.IsChecked == true ? "true" : "false"
        };
        if (game?.Ok == true) { main["paths.app0"] = TomlSettings.Quote(game.App0); main["paths.eboot"] = TomlSettings.Quote(game.Eboot); }
        var options = state.Options.ToDictionary(x => "options." + x.Key, x => TomlSettings.Quote(x.Values[x.Index]));
        options["options.version"] = "\"2\"";
        TomlSettings.Write(state.OptionsFile, options, fingerprints[state.OptionsFile]);
        TomlSettings.Write(state.ConfigFile, main, fingerprints[state.ConfigFile]);
        string active = host.Profile.Length == 0 ? state.ConfigFile : host.Profile;
        var extras = new Dictionary<string, string>();
        foreach (var entry in plugins) extras["plugins." + entry.Key] = entry.Value.IsChecked == true ? "true" : "false";
        foreach (var entry in patches.Where(x => x.Value.IsEnabled)) extras["patches." + entry.Key] = entry.Value.IsChecked == true ? "true" : "false";
        var resolution = state.Options.First(x => x.Key == "resolution").Values[state.Options.First(x => x.Key == "resolution").Index].Split('x');
        extras["video.width"] = resolution[0]; extras["video.height"] = resolution[1];
        // Quick reentry and streaming remain native config options.
        extras["loading.quick_reentry"] = QuickReentry.IsChecked == true ? "true" : "false";
        extras["streaming.all_post_processors"] = AllPostProcessors.IsChecked == true ? "true" : "false";
        extras["online.account_page"] = TomlSettings.Quote(AccountPageBox.Text.Trim());
        TomlSettings.Write(active, extras);
        state.Data = DataBox.Text;
        foreach (var file in fingerprints.Keys.ToArray()) fingerprints[file] = TomlSettings.Fingerprint(file);
        dirty = false; FooterMessage.Text = "Opciones guardadas en bbhost";
    }
    void Save_Click(object sender, RoutedEventArgs e) { try { SaveSettings(); } catch (Exception ex) { Error(ex); } }

    async Task Prepare(string folder)
    {
        game = await host.Query<PreparedGame>("--prepare-game", folder, "--data", DataBox.Text);
        PathBox.Text = game.App0.Length > 0 ? game.App0 : folder;
        GamePathStatus.Text = PathBox.Text;
        GameVersionStatus.Text = game.Ok ? $"Bloodborne {game.Version} · {game.TitleId}" : "Instalación pendiente";
        InstallationDetail.Text = game.Ok ? "Instalación validada. Preparación automática completada." : game.Error;
        PlayHint.Text = InstallationDetail.Text;
        UpdateReady();
    }
    async void BrowseGame_Click(object sender, RoutedEventArgs e)
    {
        if (busy || session.Running) return;
        var picker = new OpenFolderDialog { Title = "Seleccionar carpeta de Bloodborne" };
        if (picker.ShowDialog(this) != true) return;
        try { SetBusy(true); await Prepare(picker.FolderName); dirty = true; } catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
    }
    async void BrowseData_Click(object sender, RoutedEventArgs e)
    {
        if (busy || session.Running) return;
        var picker = new OpenFolderDialog { Title = "Carpeta de datos de bbhost (fuera del dump original)" };
        if (picker.ShowDialog(this) != true) return;
        try { SetBusy(true); DataBox.Text = picker.FolderName; SavesPath.Text = Path.Combine(DataBox.Text, "saves", "SPRJ0005"); if (PathBox.Text.Length > 0) await Prepare(PathBox.Text); dirty = true; } catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
    }
    async void Play_Click(object sender, RoutedEventArgs e)
    {
        if (busy || session.Running || state is null) return;
        try {
            SaveSettings(); SetBusy(true);
            // Revalidate the selected dump and the cache at every launch.
            await Prepare(PathBox.Text);
            if (game?.Ok != true) return;
            FooterMessage.Text = "Bloodborne está abierto · logs activos";
            int exit = await session.Run(host, state, game, DetailedLogsCheck.IsChecked == true, DeveloperConsoleCheck.IsChecked == true, Hide);
            FooterMessage.Text = exit == 0 ? "Sesión finalizada" : $"Bloodborne terminó con código {exit}. Consulta el log.";
            await Reload();
        } catch (Exception ex) { Error(ex); } finally {
            SetBusy(false);
            if (!IsVisible) { Show(); WindowState = WindowState.Normal; Activate(); }
        }
    }
    void SetBusy(bool value) { busy = value; SettingsHost.IsEnabled = !value; QuickSettingsGrid.IsEnabled = !value; SaveButton.IsEnabled = !value; Navigation.IsEnabled = !value; UpdateReady(); }
    void UpdateReady() { PlayButton.IsEnabled = !busy && !session.Running && game?.Ok == true; }

    async void ApplyServer_Click(object sender, RoutedEventArgs e)
    {
        if (state is null || busy || session.Running) return;
        try {
            SaveSettings();
            if (!int.TryParse(P2pPortBox.Text, out int port) || port < 1 || port > 65535) throw new IOException("Puerto P2P inválido.");
            string kind = ((ComboBoxItem)ServerCombo.SelectedItem).Content.ToString()!;
            string profile = ""; var online = new Dictionary<string, string> { ["online.p2p_port"] = port.ToString() };
            if (kind == "Offline") { profile = "bloodborne-offline"; online["online.offline"] = "true"; }
            else {
                var uri = new Uri(CustomServerBox.Text.Trim());
                if ((uri.Scheme != "http" && uri.Scheme != "https") || uri.UserInfo.Length > 0 || uri.AbsolutePath != "/" || uri.Query.Length > 0 || uri.Fragment.Length > 0) throw new IOException("Introduce una URL de servidor http/https sin credenciales ni ruta.");
                online["online.offline"] = "false"; online["online.host"] = TomlSettings.Quote(uri.Authority); online["online.scheme"] = TomlSettings.Quote(uri.Scheme);
                online["online.verify_tls"] = "true"; online["online.require_account"] = "true"; online["online.auth_server"] = TomlSettings.Quote(uri.GetLeftPart(UriPartial.Authority));
                profile = "bloodborne-custom-" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(uri.GetLeftPart(UriPartial.Authority))))[..16];
            }
            host.Profile = profile.Length == 0 ? "" : Path.Combine(Path.GetDirectoryName(state.ConfigFile)!, "profiles", profile + ".toml");
            string file = host.Profile.Length == 0 ? state.ConfigFile : host.Profile;
            TomlSettings.Write(file, online);
            TomlSettings.Write(state.ConfigFile, new Dictionary<string, string> { ["launcher.server_kind"] = TomlSettings.Quote(kind), ["launcher.server_profile"] = TomlSettings.Quote(profile) });
            SetBusy(true); await Reload(); FooterMessage.Text = "Perfil de servidor aplicado";
        } catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
    }
    async Task AccountAction(string action)
    {
        if (busy || session.Running) return;
        if (((ComboBoxItem)ServerCombo.SelectedItem).Content.ToString() == "Offline") { FooterMessage.Text = "Aplica primero un servidor compatible."; return; }
        try { SaveSettings(); SetBusy(true); AccountDetail.Text = "Abriendo el proceso de cuenta de bbhost..."; await host.AccountAction(action, AccountNameBox.Text.Trim(), RecoveryCodeBox.Password, result => { AccountStatus.Text = result.Account; AccountDetail.Text = result.Outcome + "\n" + result.Detail; }); foreach (var file in fingerprints.Keys.ToArray()) fingerprints[file] = TomlSettings.Fingerprint(file); }
        catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
    }
    async void RecoverAccount_Click(object sender, RoutedEventArgs e) { await AccountAction("recover"); RecoveryCodeBox.Clear(); }
    async void SignOut_Click(object sender, RoutedEventArgs e) => await AccountAction("signout");
    void AccountPage_Changed(object sender, TextChangedEventArgs e) => Preference_Changed(sender, e);
    void AccountPage_Click(object sender, RoutedEventArgs e) {
        try {
            if (!Uri.TryCreate(AccountPageBox.Text.Trim(), UriKind.Absolute, out var uri) || (uri.Scheme != "https" && uri.Scheme != "http") || uri.UserInfo.Length > 0) throw new IOException("Configura la URL de la página de cuentas de tu servidor.");
            Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });
        } catch (Exception ex) { Error(ex); }
    }
    void CopyAccountCode_Click(object sender, RoutedEventArgs e) {
        var match = Regex.Match(AccountDetail.Text, @"(?:Recovery code|Website code) ([^\s]+)");
        if (match.Success) Clipboard.SetText(match.Groups[1].Value);
    }

    void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;
        foreach (var nav in Navigation.Children.OfType<Button>()) nav.Tag = ""; button.Tag = "active";
        string page = (string)button.CommandParameter;
        var pages = new Dictionary<string, ScrollViewer> { ["Home"] = HomePage, ["Game"] = GamePage, ["Display"] = DisplayPage, ["Graphics"] = GraphicsPage, ["Upscaling"] = UpscalingPage, ["Controls"] = ControlsPage, ["Online"] = OnlinePage, ["Mods"] = ModsPage, ["Logs"] = LogsPage };
        foreach (var entry in pages) entry.Value.Visibility = page == entry.Key ? Visibility.Visible : Visibility.Collapsed;
        SettingsScrim.Visibility = page == "Home" ? Visibility.Collapsed : Visibility.Visible;
    }
    async void Reload_Click(object sender, RoutedEventArgs e) {
        if (busy || session.Running) return;
        if (dirty && MessageBox.Show(this, "Recargar descartará las opciones sin guardar.", "Recargar opciones", MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes) return;
        try { SetBusy(true); await Reload(); FooterMessage.Text = "Opciones leídas de bbhost"; } catch (Exception ex) { Error(ex); } finally { SetBusy(false); }
    }
    void OpenFolder(string path) { try { Directory.CreateDirectory(path); Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); } catch (Exception ex) { Error(ex); } }
    void OpenSaves_Click(object sender, RoutedEventArgs e) => OpenFolder(Path.Combine(DataBox.Text, "saves"));
    void OpenBackups_Click(object sender, RoutedEventArgs e) => OpenFolder(Path.Combine(DataBox.Text, "save-backups"));
    void OpenMods_Click(object sender, RoutedEventArgs e) { if (state is not null) OpenFolder(state.Mods); }
    void OpenPlugins_Click(object sender, RoutedEventArgs e) => OpenFolder(Path.Combine(host.Root, "plugins"));
    void OpenLogs_Click(object sender, RoutedEventArgs e) => OpenFolder(Path.Combine(host.Root, "logs"));
    void Backup_Click(object sender, RoutedEventArgs e) { if (busy || session.Running) return; try { FooterMessage.Text = "Backup creado: " + SaveSafety.Backup(DataBox.Text); } catch (Exception ex) { Error(ex); } }
    void ImportSave_Click(object sender, RoutedEventArgs e) { if (busy || session.Running) return; var picker = new OpenFolderDialog { Title = "Seleccionar saves antiguos (carpeta con SPRJ0005)" }; if (picker.ShowDialog(this) != true) return; try { SaveSafety.Import(picker.FolderName, DataBox.Text); FooterMessage.Text = "Save importado; el original se conserva"; } catch (Exception ex) { Error(ex); } }
    void Error(Exception ex) { FooterMessage.Text = ex.Message; PlayHint.Text = ex.Message; syncing = false; }
    void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e) { if (e.ClickCount == 2) Maximize_Click(sender, e); else DragMove(); }
    void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    void Maximize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    void Close_Click(object sender, RoutedEventArgs e) => Close();
}
