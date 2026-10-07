using System.IO;

namespace BloodborneLauncher;

public static class ServerLinks
{
    // The Hunter's Requiem is served by shadNet's separate website listener.
    // This only builds a browser link; web cookies never become game tokens.
    public static Uri ShadNetRegistration(string server)
    {
        if (!Uri.TryCreate(server.Trim(), UriKind.Absolute, out var origin) ||
            (origin.Scheme != "http" && origin.Scheme != "https") || origin.UserInfo.Length > 0 ||
            origin.AbsolutePath != "/" || origin.Query.Length > 0 || origin.Fragment.Length > 0)
            throw new IOException("Introduce primero la URL de tu servidor shadNet, sin credenciales ni ruta.");
        return new UriBuilder(origin) { Port = 31316, Path = "/register" }.Uri;
    }
}
