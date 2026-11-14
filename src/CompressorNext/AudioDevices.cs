using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace CompressorNext;

/// <summary>Ein aktives Windows-Ausgabegerät und sein Zustand in Bezug auf Compressor NEXT.</summary>
internal sealed record OutputDevice(string Guid, string Name, string Adapter, bool IsDefault, bool Bound, bool EnhancementsOff)
{
    public uint Hash => SharedState.HashEndpoint(Guid);
    public string DisplayName => string.IsNullOrEmpty(Adapter) ? Name : $"{Name} ({Adapter})";
}

internal static class AudioDevices
{
    public const string ApoClsid = "{CE339205-A997-4182-B027-A9637647F8CA}";
    private const string RenderKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render";
    private const string FxGuid = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
    private const string NameValue = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
    private const string AdapterValue = "{b3f8fa53-0004-438e-9003-51a46e139bfc},6";
    private const string DisableSysFxValue = "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

    /// <summary>Alle angeschlossenen, eingeschalteten Ausgabegeräte; das Standardgerät zuerst.</summary>
    public static List<OutputDevice> GetActive()
    {
        var result = new List<OutputDevice>();
        var defaultGuid = GetDefaultRenderGuid();
        try
        {
            using var root = Registry.LocalMachine.OpenSubKey(RenderKey);
            if (root == null) return result;
            foreach (var guid in root.GetSubKeyNames())
            {
                using var ep = root.OpenSubKey(guid);
                if (ep?.GetValue("DeviceState") is not int state || state != 1) continue;
                using var props = ep.OpenSubKey("Properties");
                using var fx = ep.OpenSubKey("FxProperties");
                var name = props?.GetValue(NameValue) as string ?? "Ausgabegerät";
                var adapter = props?.GetValue(AdapterValue) as string ?? "";
                var off = props?.GetValue(DisableSysFxValue) is int d && d != 0;
                result.Add(new OutputDevice(guid, name, adapter, string.Equals(guid, defaultGuid, StringComparison.OrdinalIgnoreCase), IsBound(fx), off));
            }
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            // Ohne Leserecht gibt es keine Liste; der Maximierer selbst arbeitet trotzdem.
        }
        result.Sort((a, b) => a.IsDefault != b.IsDefault ? (a.IsDefault ? -1 : 1) : string.Compare(a.DisplayName, b.DisplayName, StringComparison.CurrentCultureIgnoreCase));
        return result;
    }

    /// <summary>Steht das Audio-Modul in einem der Effekt-Einträge des Geräts?</summary>
    private static bool IsBound(RegistryKey? fx)
    {
        if (fx == null) return false;
        foreach (var pid in new[] { 5, 13, 6, 14, 7, 15, 1, 2 })
        {
            switch (fx.GetValue($"{FxGuid},{pid}"))
            {
                case string s when s.Equals(ApoClsid, StringComparison.OrdinalIgnoreCase):
                    return true;
                case string[] list when list.Any(s => s.Equals(ApoClsid, StringComparison.OrdinalIgnoreCase)):
                    return true;
            }
        }
        return false;
    }

    /// <summary>GUID des Standard-Ausgabegeräts, z. B. "{f5e3dddb-…}", oder null.</summary>
    public static string? GetDefaultRenderGuid()
    {
        try
        {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            try
            {
                if (enumerator.GetDefaultAudioEndpoint(0 /* eRender */, 1 /* eMultimedia */, out var device) != 0 || device == null) return null;
                try
                {
                    if (device.GetId(out var id) != 0 || id == null) return null;
                    var dot = id.LastIndexOf('.');
                    return dot >= 0 ? id[(dot + 1)..] : id;
                }
                finally
                {
                    Marshal.ReleaseComObject(device);
                }
            }
            finally
            {
                Marshal.ReleaseComObject(enumerator);
            }
        }
        catch (Exception e) when (e is COMException or InvalidCastException)
        {
            return null;
        }
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    private class MMDeviceEnumerator;

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice? device);
    }

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    private interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, out IntPtr iface);
        [PreserveSig] int OpenPropertyStore(int access, out IntPtr store);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string? id);
    }
}

/// <summary>Start mit Windows (pro Benutzer, ohne Adminrechte).</summary>
internal static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "Compressor NEXT";

    private static string Command => $"\"{Environment.ProcessPath}\" --tray";

    public static bool IsEnabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is string;
        }
    }

    public static void Set(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKey);
        if (enabled) key.SetValue(ValueName, Command);
        else key.DeleteValue(ValueName, throwOnMissingValue: false);
    }

    /// <summary>Hält den Eintrag aktuell, falls das Programm an einen anderen Ort installiert wurde.</summary>
    public static void RefreshPath()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
        if (key?.GetValue(ValueName) is string current && !current.Equals(Command, StringComparison.OrdinalIgnoreCase))
            key.SetValue(ValueName, Command);
    }
}
