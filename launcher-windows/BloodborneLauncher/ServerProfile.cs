using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher;

public sealed record ServerProfile(string Name, Dictionary<string, string> Values)
{
    public static Uri HttpUrl(string text, string label, bool originOnly = false, bool page = false)
    {
        if (!Uri.TryCreate(text.Trim(), UriKind.Absolute, out var uri) ||
            (uri.Scheme != "http" && uri.Scheme != "https") || uri.UserInfo.Length > 0 ||
            (!page && (uri.Query.Length > 0 || uri.Fragment.Length > 0)) || (originOnly && uri.AbsolutePath != "/"))
            throw new IOException(label + ": introduce una URL http/https válida, sin credenciales.");
        return uri;
    }

    // A bridge import is only a WebAPI address. It is not proof that the
    // server implements bbhost's account/matching protocol or is reachable.
    public static string ReadHostOverride(string file)
    {
        if (new FileInfo(file).Length > 1024 * 1024) throw new IOException("Archivo de red demasiado grande.");
        using var json = JsonDocument.Parse(File.ReadAllText(file));
        if (json.RootElement.ValueKind != JsonValueKind.Object ||
            !json.RootElement.TryGetProperty("https://ss4.scej-network.jp:20443", out var target) ||
            target.ValueKind != JsonValueKind.String)
            throw new IOException("El archivo no contiene la redirección WebAPI de Bloodborne.");
        return HttpUrl(target.GetString()!, "WebAPI", true).GetLeftPart(UriPartial.Authority);
    }

    public static ServerProfile Build(bool offline, string webApi, string api, string auth, string accountPage,
                                      string onlineId, string p2pPort, string p2pAddress, string stun,
                                      bool verifyTls, bool requireAccount, string transport = "native", string shadNetServer = "")
    {
        if (!int.TryParse(p2pPort.Trim(), out int port) || port < 1 || port > 65535)
            throw new IOException("El puerto P2P debe estar entre 1 y 65535.");
        p2pAddress = p2pAddress.Trim(); stun = stun.Trim(); onlineId = onlineId.Trim();
        if (p2pAddress.Length > 0 && Uri.CheckHostName(p2pAddress) != UriHostNameType.IPv4)
            throw new IOException("La dirección P2P anunciada debe ser una IPv4; vacía usa el valor nativo.");
        // Native STUN parsing currently accepts host[:port], not bracketed IPv6.
        if (stun.Length > 0 && stun != "off") {
            var parts = stun.Split(':');
            if (parts.Length > 2 || Uri.CheckHostName(parts[0]) is UriHostNameType.Unknown or UriHostNameType.IPv6 ||
                (parts.Length == 2 && (!int.TryParse(parts[1], out int sport) || sport < 1 || sport > 65535)))
                throw new IOException("STUN: usa host:puerto, vacío para automático u off para desactivarlo.");
        }
        var values = new Dictionary<string, string> {
            ["online.transport"] = TomlSettings.Quote(transport),
            ["online.shadnet_server"] = TomlSettings.Quote(shadNetServer.Trim()),
            ["online.offline"] = offline ? "true" : "false",
            ["online.online_id"] = TomlSettings.Quote(onlineId),
            ["online.p2p_port"] = port.ToString(), ["online.p2p_addr"] = TomlSettings.Quote(p2pAddress),
            ["online.stun_server"] = TomlSettings.Quote(stun),
            ["online.verify_tls"] = verifyTls ? "true" : "false",
            ["online.require_account"] = requireAccount ? "true" : "false",
            ["online.account_page"] = TomlSettings.Quote(accountPage.Trim()),
        };
        if (transport != "native" && transport != "shadnet") throw new IOException("Protocolo de servidor desconocido.");
        if (transport == "shadnet" && !offline) {
            if (!Uri.TryCreate(shadNetServer.Trim(), UriKind.Absolute, out var tcp) ||
                (tcp.Scheme != "tcp" && tcp.Scheme != "tls") || tcp.Host.Length == 0 || tcp.Port < 1 || tcp.Port > 65535 ||
                tcp.UserInfo.Length > 0 || (tcp.AbsolutePath != "/" && tcp.AbsolutePath != "") || tcp.Query.Length > 0 || tcp.Fragment.Length > 0)
                throw new IOException("shadNet: usa tcp://servidor:31313 o tls://servidor:puerto, sin credenciales.");
        }
        if (accountPage.Trim().Length > 0) HttpUrl(accountPage, "Página de cuentas", page: true);
        Uri? web = webApi.Trim().Length > 0 ? HttpUrl(webApi, "WebAPI", true) : null;
        if (!offline && web is null) throw new IOException("Introduce la dirección WebAPI del servidor.");
        string apiBase = api.Trim().Length > 0 ? HttpUrl(api, "API de cuentas/matching").AbsoluteUri.TrimEnd('/') :
                         web?.GetLeftPart(UriPartial.Authority) ?? "";
        string authBase = auth.Trim().Length > 0 ? HttpUrl(auth, "Servidor de cuentas").AbsoluteUri.TrimEnd('/') : apiBase;
        // Persist explicit bases: native fallback would append :18671 to
        // a WebAPI authority that might already include a custom port.
        values["online.host"] = TomlSettings.Quote(web?.Authority ?? "");
        values["online.scheme"] = TomlSettings.Quote(web?.Scheme ?? "https");
        values["online.np_server"] = TomlSettings.Quote(apiBase);
        values["online.auth_server"] = TomlSettings.Quote(authBase);
        string identity = (web?.GetLeftPart(UriPartial.Authority) ?? "") + "\n" + apiBase + "\n" + authBase;
        if (transport == "shadnet") identity += "\nshadnet\n" + shadNetServer.Trim();
        string name = offline ? "bloodborne-offline" : "bloodborne-custom-" +
                      Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity)))[..16];
        return new(name, values);
    }
}
