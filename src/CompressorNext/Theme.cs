using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace CompressorNext;

/// <summary>Farben und Zeichenhilfen im Stil der NEXT-Apps (dunkle Flächen, Orange-Akzent).</summary>
internal static class Theme
{
    public static readonly Color Background = Color.FromArgb(0x13, 0x13, 0x16);
    public static readonly Color Card = Color.FromArgb(0x1B, 0x1B, 0x1E);
    public static readonly Color CardBorder = Color.FromArgb(0x2A, 0x2A, 0x2D);
    public static readonly Color Track = Color.FromArgb(0x0B, 0x0B, 0x0D);
    public static readonly Color Text = Color.FromArgb(0xE4, 0xE1, 0xE6);
    public static readonly Color TextDim = Color.FromArgb(0xA9, 0xA5, 0xAD);
    public static readonly Color TextFaint = Color.FromArgb(0x77, 0x73, 0x7B);
    public static readonly Color Accent = Color.FromArgb(0xFF, 0xB3, 0x6B);
    public static readonly Color AccentStrong = Color.FromArgb(0xFF, 0x7A, 0x3D);
    public static readonly Color Error = Color.FromArgb(0xFF, 0x6B, 0x6B);
    public static readonly Color Ok = Color.FromArgb(0x8F, 0xDA, 0x4A);
    public static readonly Color Off = Color.FromArgb(0x5A, 0x56, 0x60);

    public const string FontName = "Segoe UI";

    public static Font Font(float size, FontStyle style = FontStyle.Regular) => new(FontName, size, style, GraphicsUnit.Point);

    public static GraphicsPath RoundRect(RectangleF r, float radius)
    {
        var path = new GraphicsPath();
        var d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
        if (d <= 0.5f)
        {
            path.AddRectangle(r);
            return path;
        }
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    public static void FillRoundRect(Graphics g, Color color, RectangleF r, float radius)
    {
        using var path = RoundRect(r, radius);
        using var brush = new SolidBrush(color);
        g.FillPath(brush, path);
    }

    public static void DrawRoundRect(Graphics g, Color color, RectangleF r, float radius, float width = 1f)
    {
        using var path = RoundRect(r, radius);
        using var pen = new Pen(color, width);
        g.DrawPath(pen, path);
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    /// <summary>Dunkle Titelleiste (Windows 10 2004 und neuer).</summary>
    public static void UseDarkTitleBar(IntPtr handle)
    {
        var on = 1;
        _ = DwmSetWindowAttribute(handle, 20, ref on, sizeof(int));
        var caption = Background.R | (Background.G << 8) | (Background.B << 16);
        _ = DwmSetWindowAttribute(handle, 35, ref caption, sizeof(int)); // Windows 11: Farbe der Titelleiste
    }

    public static string Db(float value, int decimals = 0)
    {
        var s = Math.Abs(value).ToString(decimals == 0 ? "0" : "0.0", System.Globalization.CultureInfo.GetCultureInfo("de-DE"));
        return value > 0 ? "+" + s : value < 0 ? "−" + s : s;
    }
}

/// <summary>Grundlage für selbst gezeichnete Bedienelemente: flimmerfrei, mit DPI-Hilfe.</summary>
internal abstract class NextControl : Control
{
    protected NextControl()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
        BackColor = Theme.Card;
        ForeColor = Theme.Text;
    }

    /// <summary>Rechnet logische Pixel (96 dpi) in Gerätepixel um.</summary>
    protected float S(float v) => v * DeviceDpi / 96f;

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        e.Graphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        Draw(e.Graphics);
    }

    protected abstract void Draw(Graphics g);
}
