using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace CompressorNext;

/// <summary>Symbol im Infobereich mit Menü; öffnet das Hauptfenster bei Bedarf.</summary>
internal sealed class TrayApp : ApplicationContext
{
    private readonly AppModel _model;
    private readonly NotifyIcon _icon = new();
    private readonly ToolStripMenuItem _toggleItem = new("Eingeschaltet");
    private readonly ToolStripMenuItem[] _presetItems;
    private readonly System.Windows.Forms.Timer _poll = new() { Interval = 2000 };
    private readonly EventWaitHandle _showEvent;
    private readonly EventWaitHandle _exitEvent;
    private readonly SynchronizationContext _ui;
    private MainForm? _form;
    private Icon? _current;
    private bool _shownOn;
    private bool _iconValid;

    public TrayApp(AppModel model, bool startHidden, EventWaitHandle showEvent, EventWaitHandle exitEvent)
    {
        _model = model;
        _showEvent = showEvent;
        _exitEvent = exitEvent;
        _ui = SynchronizationContext.Current ?? new WindowsFormsSynchronizationContext();

        var menu = new ContextMenuStrip();
        var open = new ToolStripMenuItem("Compressor NEXT öffnen") { Font = new Font(menu.Font, FontStyle.Bold) };
        open.Click += (_, _) => ShowWindow();
        _toggleItem.Click += (_, _) => _model.Update(_model.Settings with { Enabled = !_model.Settings.Enabled });
        _presetItems =
        [
            new ToolStripMenuItem("Leicht (Schwelle −9 dB)") { Tag = -9 },
            new ToolStripMenuItem("Film (Schwelle −18 dB)") { Tag = -18 },
            new ToolStripMenuItem("Nacht (Schwelle −27 dB)") { Tag = -27 },
        ];
        foreach (var item in _presetItems)
            item.Click += (s, _) => _model.Update(_model.Settings with { Enabled = true, ThresholdDb = (int)((ToolStripMenuItem)s!).Tag! });
        var exit = new ToolStripMenuItem("Beenden");
        exit.Click += (_, _) => Exit();
        menu.Items.AddRange([open, new ToolStripSeparator(), _toggleItem, new ToolStripSeparator(), .. _presetItems, new ToolStripSeparator(), exit]);

        _icon.ContextMenuStrip = menu;
        _icon.MouseClick += (_, e) => { if (e.Button == MouseButtons.Left) ToggleWindow(); };
        _model.Changed += UpdateTray;
        UpdateTray();
        _icon.Visible = true;

        // Auch ohne offenes Fenster Änderungen von außen bemerken (z. B. "--set").
        _poll.Tick += (_, _) => _model.PollExternalChanges();
        _poll.Start();

        // Ein zweiter Programmstart holt das Fenster nach vorn; "--shutdown" beendet.
        var waiter = new Thread(() =>
        {
            WaitHandle[] handles = [_showEvent, _exitEvent];
            while (true)
            {
                var i = WaitHandle.WaitAny(handles);
                if (i == 1) { _ui.Post(_ => Exit(), null); return; }
                _ui.Post(_ => ShowWindow(), null);
            }
        }) { IsBackground = true, Name = "Signale" };
        waiter.Start();

        if (!startHidden) ShowWindow();
    }

    private void ShowWindow()
    {
        _form ??= new MainForm(_model);
        _form.ShowNearTray();
    }

    private void ToggleWindow()
    {
        if (_form is { Visible: true }) _form.Hide();
        else ShowWindow();
    }

    private void UpdateTray()
    {
        var s = _model.Settings;
        _toggleItem.Checked = s.Enabled;
        foreach (var item in _presetItems) item.Checked = s.Enabled && (int)item.Tag! == (int)MathF.Round(s.ThresholdDb);
        var text = s.Enabled
            ? $"Compressor NEXT – Ein (Schwelle {Theme.Db(MathF.Round(s.ThresholdDb))} dB)"
            : "Compressor NEXT – Aus";
        _icon.Text = text.Length > 63 ? text[..63] : text;
        if (!_iconValid || _shownOn != s.Enabled)
        {
            var old = _current;
            _current = RenderIcon(s.Enabled, SystemInformation.SmallIconSize.Width);
            _icon.Icon = _current;
            old?.Dispose();
            _shownOn = s.Enabled;
            _iconValid = true;
        }
    }

    private void Exit()
    {
        _model.Exiting = true;
        _poll.Stop();
        _icon.Visible = false;
        _form?.Close();
        ExitThread();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _icon.Dispose();
            _poll.Dispose();
            _form?.Dispose();
            _current?.Dispose();
        }
        base.Dispose(disposing);
    }

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr handle);

    /// <summary>Zeichnet das Symbol: Pegelbalken, die an einer Obergrenze enden; ausgeschaltet grau.</summary>
    public static Icon RenderIcon(bool on, int size)
    {
        using var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp))
        {
            DrawLogo(g, size, on);
        }
        var handle = bmp.GetHicon();
        try
        {
            using var temp = Icon.FromHandle(handle);
            return (Icon)temp.Clone();
        }
        finally
        {
            DestroyIcon(handle);
        }
    }

    public static void DrawLogo(Graphics g, int size, bool on)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Color.Transparent);
        float s = size;
        Theme.FillRoundRect(g, Color.FromArgb(0x1B, 0x1B, 0x1E), new RectangleF(0, 0, s, s), s * 0.22f);
        // Fünf Balken; die mittleren würden über die Obergrenze ragen und enden dort.
        float[] heights = [0.34f, 0.62f, 0.62f, 0.62f, 0.44f];
        float left = s * 0.17f, right = s * 0.83f, bottom = s * 0.80f, top = s * 0.24f;
        var gap = s * 0.045f;
        var w = (right - left - 4 * gap) / 5;
        for (var i = 0; i < 5; i++)
        {
            var h = (bottom - top) * heights[i] / 0.62f;
            var x = left + i * (w + gap);
            var capped = heights[i] >= 0.62f;
            using var brush = new SolidBrush(on ? (capped ? Theme.AccentStrong : Theme.Accent) : Color.FromArgb(0x8A, 0x86, 0x90));
            g.FillRectangle(brush, x, bottom - h, w, h);
        }
        using var pen = new Pen(on ? Color.White : Color.FromArgb(0xB0, 0xAC, 0xB4), Math.Max(1f, s * 0.07f));
        g.DrawLine(pen, s * 0.12f, top - s * 0.05f, s * 0.88f, top - s * 0.05f);
    }
}
