using System.Drawing.Imaging;
using System.Globalization;

namespace CompressorNext;

internal static class Program
{
    private const string InstanceName = @"Local\CompressorNext.Instance";
    private const string ShowName = @"Local\CompressorNext.Show";
    private const string ExitName = @"Local\CompressorNext.Exit";

    /// <summary>
    /// Aufrufe:
    ///   CompressorNext.exe                 Fenster öffnen (oder das der laufenden Instanz nach vorn holen)
    ///   CompressorNext.exe --tray          nur ins Infofeld starten (Autostart)
    ///   CompressorNext.exe --shutdown      laufende Instanz beenden (Setup)
    ///   CompressorNext.exe --set enabled=1|0|toggle threshold=-18 output=-1 compressor=1|0|toggle eq=1|0|toggle
    ///   CompressorNext.exe --first-run     nach der Installation: Autostart einrichten und starten
    ///   CompressorNext.exe --remove-autostart
    ///   CompressorNext.exe --make-icon Datei.ico | --screenshot Datei.png [--tab equalizer]   (Entwicklung)
    /// </summary>
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);

        if (args.Contains("--make-icon")) return MakeIcon(args);
        if (args.Contains("--shutdown")) return Shutdown();
        if (args.Contains("--remove-autostart")) { Autostart.Set(false); return 0; }
        if (args.Contains("--set")) return Set(args);

        var firstRun = args.Contains("--first-run");
        if (firstRun) Autostart.Set(true);
        else if (!args.Contains("--screenshot")) Autostart.RefreshPath(); // ein Entwicklungsstand soll den Autostart nicht umbiegen

        using var instance = new Mutex(true, InstanceName, out var isFirst);
        using var showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowName);
        using var exitEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ExitName);
        if (!isFirst && !args.Contains("--screenshot"))
        {
            if (!args.Contains("--tray")) showEvent.Set();
            return 0;
        }

        SharedState state;
        try
        {
            state = new SharedState();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show("Die Einstellungsdatei lässt sich nicht öffnen:\n" + SharedState.DefaultPath + "\n\n" + e.Message +
                            "\n\nBitte Compressor NEXT neu installieren.", "Compressor NEXT", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
        using var model = new AppModel(state);

        if (args.Contains("--screenshot")) return Screenshot(model, args);

        using var app = new TrayApp(model, startHidden: args.Contains("--tray"), showEvent, exitEvent);
        Application.Run(app);
        return 0;
    }

    private static int Shutdown()
    {
        if (!EventWaitHandle.TryOpenExisting(ExitName, out var exit)) return 0; // läuft nicht
        using (exit) exit.Set();
        // Warten, bis die Instanz beendet ist (ihr Mutex wird frei).
        for (var i = 0; i < 50; i++)
        {
            if (!Mutex.TryOpenExisting(InstanceName, out var m)) return 0;
            m.Dispose();
            Thread.Sleep(100);
        }
        return 1;
    }

    private static int Set(string[] args)
    {
        using var state = new SharedState();
        var s = state.ReadSettings();
        var eq = state.ReadEq();
        var eqBefore = eq;
        static bool Switch(string value, bool current) => value switch { "toggle" => !current, "1" or "true" or "on" or "ein" => true, _ => false };
        foreach (var arg in args)
        {
            var parts = arg.Split('=', 2);
            if (parts.Length != 2) continue;
            var value = parts[1].Trim().ToLowerInvariant();
            switch (parts[0].Trim().ToLowerInvariant())
            {
                case "enabled":
                    s = s with { Enabled = Switch(value, s.Enabled) };
                    break;
                case "compressor":
                    s = s with { CompressorOn = Switch(value, s.CompressorOn) };
                    break;
                case "eq":
                    eq = eq with { Enabled = Switch(value, eq.Enabled) };
                    break;
                case "threshold" when float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var t):
                    s = s with { ThresholdDb = t };
                    break;
                case "output" when float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var o):
                    s = s with { OutputDb = o };
                    break;
            }
        }
        state.WriteSettings(s, state.ReadExcluded());
        if (!eq.Equals(eqBefore)) state.WriteEq(eq, EqMath.AutoLevelDb(eq));
        return 0;
    }

    /// <summary>Schreibt das Programmsymbol als .ico (mehrere Größen, PNG-komprimiert).</summary>
    private static int MakeIcon(string[] args)
    {
        var path = args[Array.IndexOf(args, "--make-icon") + 1];
        int[] sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
        var images = new List<byte[]>();
        foreach (var size in sizes)
        {
            using var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bmp)) TrayApp.DrawLogo(g, size, on: true);
            using var ms = new MemoryStream();
            bmp.Save(ms, ImageFormat.Png);
            images.Add(ms.ToArray());
        }
        using var file = new BinaryWriter(File.Create(path));
        file.Write((short)0);
        file.Write((short)1);
        file.Write((short)sizes.Length);
        var offset = 6 + 16 * sizes.Length;
        for (var i = 0; i < sizes.Length; i++)
        {
            file.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
            file.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
            file.Write((byte)0);
            file.Write((byte)0);
            file.Write((short)1);
            file.Write((short)32);
            file.Write(images[i].Length);
            file.Write(offset);
            offset += images[i].Length;
        }
        foreach (var image in images) file.Write(image);
        return 0;
    }

    /// <summary>Zeichnet das Hauptfenster in eine PNG-Datei (für die Dokumentation und zum Prüfen der Oberfläche).</summary>
    private static int Screenshot(AppModel model, string[] args)
    {
        var path = args[Array.IndexOf(args, "--screenshot") + 1];
        using var form = new MainForm(model) { Passive = true };
        var tab = Array.IndexOf(args, "--tab");
        if (tab >= 0 && tab + 1 < args.Length && args[tab + 1].StartsWith("eq", StringComparison.OrdinalIgnoreCase)) form.SelectTab(1);
        form.ShowNearTray();
        // Pegelanzeige einschwingen lassen; "--wait ms" für ein längeres Spektrogramm
        var wait = Array.IndexOf(args, "--wait");
        var ms = wait >= 0 && wait + 1 < args.Length && int.TryParse(args[wait + 1], out var w) ? w : 2500;
        var end = Environment.TickCount64 + ms;
        while (Environment.TickCount64 < end)
        {
            Application.DoEvents();
            Thread.Sleep(15);
        }
        using var bmp = new Bitmap(form.Width, form.Height);
        form.DrawToBitmap(bmp, new Rectangle(Point.Empty, form.Size));
        bmp.Save(path, ImageFormat.Png);
        model.Exiting = true;
        form.Close();
        return 0;
    }
}
