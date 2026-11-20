using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace CompressorNext;

/// <summary>
/// Grafik des Equalizers: der Frequenzgang jedes Bandes farbig gefüllt, darüber die Summenkurve; dahinter das
/// Spektrum des Ausgangs als Fläche und als nach oben laufendes Spektrogramm; rechts der Ausgangspegel.
/// Bedienung: Punkt ziehen = Frequenz und Verstärkung (Umschalt = fein), Mausrad = Güte, Doppelklick ins Leere =
/// neues Band, Doppelklick auf einen Punkt = 0 dB, Rechtsklick = Typ, aus, entfernen; Entf entfernt das gewählte
/// Band, Pfeiltasten verschieben es, Bild auf/ab ändert die Güte.
/// </summary>
internal sealed class EqGraphControl : NextControl
{
    public const float DbRange = 18f;
    private const double FMin = 20, FMax = 20000;
    private const float SpecTop = 0f, SpecBottom = -90f;  // dBFS (mit Neigung) am oberen/unteren Rand
    private const float SgLow = -72f, SgHigh = -12f;      // Farbskala des Spektrogramms
    private const float TiltDbPerOctave = 3f;             // rosa Rauschen erscheint waagerecht (Bezug 1 kHz)
    private const float OutTop = 3f, OutBottom = -48f;    // Skala des Ausgangspegels
    private static readonly double[] GridMajor = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
    private static readonly double[] GridMinor = [30, 40, 60, 70, 80, 90, 300, 400, 600, 700, 800, 900, 3000, 4000, 6000, 7000, 8000, 9000];
    private static readonly string[] GridLabels = ["20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"];
    private static readonly Color PlotBack = Color.FromArgb(0x0E, 0x0E, 0x11);
    private static readonly int[] Palette = BuildPalette();

    private EqSettings _eq = EqSettings.Default;
    private bool _active = true;
    private double _fs = 48000;
    private int _selected = -1, _hover = -1, _drag = -1;
    private bool _dragFine;
    private PointF _anchorMouse, _anchorHandle;

    private double[] _colFreq = [];
    private readonly float[][] _bandDb = new float[EqSettings.SlotCount][];
    private float[] _totalDb = [];
    private bool _curvesDirty = true;

    private float[] _spec = [], _specRaw = [];
    private int[] _sgRow = [];
    private bool _specVisible;
    private Bitmap? _sgram;
    private int _sgHead;

    private float _outL = -120f, _outR = -120f, _outPeak = -120f;
    private long _outPeakUntil;
    private string? _flash;
    private long _flashUntil;
    private readonly Stopwatch _clock = Stopwatch.StartNew();

    private readonly Font _labelFont = Theme.Font(7f);
    private readonly Font _tipFont = Theme.Font(8.25f);
    private readonly Font _tipBold = Theme.Font(8.25f, FontStyle.Bold);
    private readonly Font _numFont = Theme.Font(6.75f, FontStyle.Bold);
    private readonly Font _hintFont = Theme.Font(9f);
    private readonly ContextMenuStrip _menu = new();

    /// <summary>Die Bedienung hat die Einstellungen geändert.</summary>
    public event Action<EqSettings>? Edited;

    /// <summary>Ein anderes Band wurde gewählt (−1 = keines).</summary>
    public event Action<int>? SelectionChanged;

    public EqGraphControl()
    {
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        AccessibleRole = AccessibleRole.Graphic;
        AccessibleName = "Equalizer-Kurve";
        _menu.Closed += (_, _) => Invalidate();
    }

    /// <summary>Gewähltes Band (Platz), ohne Ereignis.</summary>
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public int SelectedSlot
    {
        get => _selected;
        set
        {
            var v = value >= 0 && value < EqSettings.SlotCount && _eq.Slots[value] != null ? value : -1;
            if (v == _selected) return;
            _selected = v;
            Invalidate();
        }
    }

    /// <summary>Übernimmt die Einstellungen; active = false zeichnet die Kurve als "wirkt gerade nicht".</summary>
    public void SetSettings(EqSettings eq, bool active)
    {
        if (eq.Equals(_eq) && active == _active) return;
        _eq = eq;
        _active = active;
        _curvesDirty = true;
        if (_selected >= 0 && eq.Slots[_selected] == null) _selected = -1;
        if (_drag >= 0 && eq.Slots[_drag] == null) _drag = -1;
        if (_hover >= 0 && eq.Slots[_hover] == null) _hover = -1;
        Invalidate();
    }

    /// <summary>Abtastrate des laufenden Audiostroms (für die exakte Kurve nahe der Nyquist-Frequenz).</summary>
    public void SetSampleRate(double fs)
    {
        if (fs < 8000 || Math.Abs(fs - _fs) < 1) return;
        _fs = fs;
        _curvesDirty = true;
        Invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Geometrie
    // ------------------------------------------------------------------------------------------
    private Rectangle PlotRect
    {
        get
        {
            int l = (int)S(30), t = (int)S(6), r = Width - (int)S(24), b = Height - (int)S(18);
            return new Rectangle(l, t, Math.Max(1, r - l), Math.Max(1, b - t));
        }
    }

    private float X(double f)
    {
        var p = PlotRect;
        return p.Left + p.Width * (float)(Math.Log(f / FMin) / Math.Log(FMax / FMin));
    }

    private double FreqAt(float x)
    {
        var p = PlotRect;
        return FMin * Math.Pow(FMax / FMin, (x - p.Left) / p.Width);
    }

    private float Y(double db)
    {
        var p = PlotRect;
        return p.Top + p.Height * (float)((DbRange - db) / (2 * DbRange));
    }

    private double DbAt(float y)
    {
        var p = PlotRect;
        return DbRange - (y - p.Top) / p.Height * 2 * DbRange;
    }

    private float YSpec(float db)
    {
        var p = PlotRect;
        return p.Top + p.Height * (SpecTop - db) / (SpecTop - SpecBottom);
    }

    private PointF HandlePos(int slot)
    {
        var b = _eq.Slots[slot]!.Value;
        return new PointF(X(b.FreqHz), Y(b.HasGainValue ? b.GainDb : 0));
    }

    private void EnsureColumns()
    {
        var p = PlotRect;
        var n = p.Width;
        if (_colFreq.Length != n)
        {
            _colFreq = new double[n];
            for (var i = 0; i < n; i++) _colFreq[i] = FreqAt(p.Left + i + 0.5f);
            for (var s = 0; s < _bandDb.Length; s++) _bandDb[s] = new float[n];
            _totalDb = new float[n];
            _spec = new float[n];
            _specRaw = new float[n];
            _sgRow = new int[n];
            Array.Fill(_spec, -140f);
            Array.Fill(_specRaw, -140f);
            _curvesDirty = true;
        }
        if (_sgram == null || _sgram.Width != n || _sgram.Height != p.Height)
        {
            _sgram?.Dispose();
            _sgram = new Bitmap(n, p.Height, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(_sgram)) g.Clear(PlotBack);
            _sgHead = 0;
        }
    }

    private void RecomputeCurves()
    {
        _curvesDirty = false;
        Array.Clear(_totalDb);
        for (var s = 0; s < EqSettings.SlotCount; s++)
        {
            if (_eq.Slots[s] is not { } band) continue;
            EqMath.Curve(band, _fs, _colFreq, _bandDb[s]);
            if (!band.On) continue;
            var db = _bandDb[s];
            for (var i = 0; i < db.Length; i++) _totalDb[i] += db[i];
        }
    }

    // ------------------------------------------------------------------------------------------
    // Spektrum und Ausgangspegel (einmal je Bild von außen)
    // ------------------------------------------------------------------------------------------

    /// <summary>Neues Spektrum (null = kein Ton); schiebt das Spektrogramm um eine Zeile weiter.</summary>
    public void PushSpectrum(SpectrumAnalyzer? analyzer, float dt)
    {
        if (Width < 10 || Height < 10) return;
        EnsureColumns();
        var p = PlotRect;
        var n = _spec.Length;
        var visible = false;
        for (var i = 0; i < n; i++)
        {
            float raw;
            if (analyzer != null)
            {
                var f0 = FreqAt(p.Left + i);
                var f1 = FreqAt(p.Left + i + 1);
                raw = analyzer.LevelBetween(f0, f1) + TiltDbPerOctave * (float)Math.Log2(_colFreq[i] / 1000.0);
            }
            else
            {
                raw = -140f;
            }
            _specRaw[i] = raw;
            var s = _spec[i];
            s = raw > s ? s + (raw - s) * 0.6f : Math.Max(raw, s - 40f * dt);
            _spec[i] = s;
            if (s > SpecBottom) visible = true;
            var t = (raw - SgLow) / (SgHigh - SgLow);
            _sgRow[i] = Palette[Math.Clamp((int)(t * 255f), 0, 255)];
        }
        _specVisible = visible;
        if (_sgram != null)
        {
            var data = _sgram.LockBits(new Rectangle(0, _sgHead, n, 1), ImageLockMode.WriteOnly, PixelFormat.Format32bppPArgb);
            Marshal.Copy(_sgRow, 0, data.Scan0, n);
            _sgram.UnlockBits(data);
            _sgHead = (_sgHead + 1) % _sgram.Height;
        }
        Invalidate();
    }

    /// <summary>Spitzen am Ausgang seit dem letzten Bild (linear) oder null.</summary>
    public void SetOutput(float? linL, float? linR, float dt)
    {
        static float Db(float v) => v > 1e-6f ? 20f * MathF.Log10(v) : -120f;
        var l = linL is { } a ? Db(a) : -120f;
        var r = linR is { } b ? Db(b) : -120f;
        _outL = Math.Max(l, _outL - 20f * dt);
        _outR = Math.Max(r, _outR - 20f * dt);
        var now = _clock.ElapsedMilliseconds;
        var top = Math.Max(l, r);
        if (top >= _outPeak - 0.5f && top > -120f)
        {
            _outPeak = Math.Max(_outPeak, top);
            _outPeakUntil = now + 1500;
        }
        else if (now > _outPeakUntil)
        {
            _outPeak = Math.Max(Math.Max(_outL, _outR), _outPeak - 20f * dt);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Zeichnen
    // ------------------------------------------------------------------------------------------
    protected override void Draw(Graphics g)
    {
        if (Width < 40 || Height < 40) return;
        EnsureColumns();
        if (_curvesDirty) RecomputeCurves();
        var p = PlotRect;
        var pf = (RectangleF)p;

        using (var back = new SolidBrush(PlotBack)) g.FillRectangle(back, p);
        DrawSpectrogram(g, p);
        DrawGrid(g, pf);

        var state = g.Save();
        g.SetClip(p);
        if (_specVisible) DrawSpectrum(g, pf);
        DrawBands(g, pf);
        DrawTotal(g);
        g.Restore(state);

        using (var border = new Pen(Theme.CardBorder, 1f)) g.DrawRectangle(border, p.X - 0.5f, p.Y - 0.5f, p.Width, p.Height);
        DrawOutputMeter(g, pf);
        DrawHandles(g);
        DrawHints(g, pf);
        DrawTooltip(g);
    }

    private void DrawSpectrogram(Graphics g, Rectangle p)
    {
        var bmp = _sgram;
        if (bmp == null) return;
        var oldMode = g.InterpolationMode;
        var oldOffset = g.PixelOffsetMode;
        g.InterpolationMode = InterpolationMode.NearestNeighbor;
        g.PixelOffsetMode = PixelOffsetMode.Half;
        int h = bmp.Height, w = bmp.Width, head = _sgHead;
        // Älteste Zeile (head) oben, neueste (head − 1) unten: Das Bild läuft nach oben.
        var topRows = h - head;
        if (topRows > 0) g.DrawImage(bmp, new Rectangle(p.X, p.Y, w, topRows), new Rectangle(0, head, w, topRows), GraphicsUnit.Pixel);
        if (head > 0) g.DrawImage(bmp, new Rectangle(p.X, p.Y + topRows, w, head), new Rectangle(0, 0, w, head), GraphicsUnit.Pixel);
        g.InterpolationMode = oldMode;
        g.PixelOffsetMode = oldOffset;
    }

    private void DrawGrid(Graphics g, RectangleF p)
    {
        using var minor = new Pen(Color.FromArgb(0x10, 0xFF, 0xFF, 0xFF), 1f);
        using var major = new Pen(Color.FromArgb(0x24, 0xFF, 0xFF, 0xFF), 1f);
        using var zero = new Pen(Color.FromArgb(0x55, 0xFF, 0xFF, 0xFF), 1f);
        foreach (var f in GridMinor) g.DrawLine(minor, X(f), p.Top, X(f), p.Bottom);
        foreach (var f in GridMajor) g.DrawLine(major, X(f), p.Top, X(f), p.Bottom);
        for (var db = -DbRange + 6; db < DbRange; db += 6) g.DrawLine(db == 0 ? zero : major, p.Left, Y(db), p.Right, Y(db));

        const TextFormatFlags right = TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        for (var db = -DbRange + 6; db < DbRange; db += 6)
        {
            var text = db > 0 ? "+" + db : db < 0 ? "−" + -db : "0";
            TextRenderer.DrawText(g, text, _labelFont, Rect(0, Y(db) - S(7), p.Left - S(5), S(14)), Theme.TextFaint, right);
        }
        TextRenderer.DrawText(g, "dB", _labelFont, Rect(0, p.Top - S(1), p.Left - S(5), S(12)), Theme.TextFaint, right);
        const TextFormatFlags center = TextFormatFlags.HorizontalCenter | TextFormatFlags.Top | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        for (var i = 0; i < GridMajor.Length; i++)
        {
            var x = X(GridMajor[i]);
            if (i == 0) x += S(6);
            if (i == GridMajor.Length - 1) x -= S(8);
            TextRenderer.DrawText(g, GridLabels[i], _labelFont, Rect(x - S(16), p.Bottom + S(3), S(32), S(12)), Theme.TextFaint, center);
        }
        TextRenderer.DrawText(g, "Hz", _labelFont, Rect(p.Right + S(2), p.Bottom + S(3), S(22), S(12)), Theme.TextFaint,
            TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
    }

    private void DrawSpectrum(Graphics g, RectangleF p)
    {
        var n = _spec.Length;
        var pts = new PointF[n + 2];
        for (var i = 0; i < n; i++) pts[i] = new PointF(p.Left + i + 0.5f, Math.Min(p.Bottom + 2, YSpec(_spec[i])));
        pts[n] = new PointF(p.Right, p.Bottom + 2);
        pts[n + 1] = new PointF(p.Left, p.Bottom + 2);
        using (var fill = new LinearGradientBrush(new PointF(0, p.Top), new PointF(0, p.Bottom + 1), Color.FromArgb(0x40, 0xE8, 0xE4, 0xEC), Color.FromArgb(0x10, 0xE8, 0xE4, 0xEC)))
            g.FillPolygon(fill, pts);
        using var line = new Pen(Color.FromArgb(0x90, 0xE8, 0xE4, 0xEC), 1f);
        g.DrawLines(line, pts.AsSpan(0, n).ToArray());
    }

    private void DrawBands(Graphics g, RectangleF p)
    {
        var y0 = Y(0);
        var n = _colFreq.Length;
        for (var pass = 0; pass < 2; pass++)
        {
            for (var s = 0; s < EqSettings.SlotCount; s++)
            {
                if (_eq.Slots[s] is not { } band) continue;
                var isSel = s == _selected || s == _drag;
                if ((pass == 1) != isSel) continue; // gewähltes Band zuletzt, also obenauf
                var db = _bandDb[s];
                var pts = new PointF[n + 2];
                for (var i = 0; i < n; i++) pts[i] = new PointF(p.Left + i + 0.5f, Y(Math.Clamp(db[i], -DbRange - 8, DbRange + 8)));
                pts[n] = new PointF(p.Right, y0);
                pts[n + 1] = new PointF(p.Left, y0);
                var color = EqBand.SlotColor(s);
                if (band.On && _active)
                {
                    using var fill = new SolidBrush(Color.FromArgb(isSel ? 0x55 : 0x2C, color));
                    g.FillPolygon(fill, pts);
                }
                else if (band.On)
                {
                    using var fill = new SolidBrush(Color.FromArgb(0x14, color));
                    g.FillPolygon(fill, pts);
                }
                using var pen = new Pen(Color.FromArgb(band.On ? (isSel ? 0xD0 : 0x70) : 0x60, color), isSel ? S(1.4f) : 1f);
                if (!band.On) pen.DashStyle = DashStyle.Dash;
                g.DrawLines(pen, pts.AsSpan(0, n).ToArray());
            }
        }
    }

    private void DrawTotal(Graphics g)
    {
        var n = _colFreq.Length;
        var p = PlotRect;
        var pts = new PointF[n];
        for (var i = 0; i < n; i++) pts[i] = new PointF(p.Left + i + 0.5f, Y(Math.Clamp(_totalDb[i], -DbRange - 8, DbRange + 8)));
        if (_active && _eq.BandCount > 0)
        {
            using var glow = new Pen(Color.FromArgb(0x40, Theme.AccentStrong), S(5f)) { LineJoin = LineJoin.Round };
            g.DrawLines(glow, pts);
        }
        using var pen = new Pen(_active ? Theme.Accent : Theme.Off, S(2f)) { LineJoin = LineJoin.Round };
        if (!_active) pen.DashStyle = DashStyle.Dash;
        g.DrawLines(pen, pts);
    }

    private void DrawHandles(Graphics g)
    {
        var order = Enumerable.Range(0, EqSettings.SlotCount).Where(s => _eq.Slots[s] != null).OrderBy(s => s == _selected || s == _drag ? 1 : 0);
        foreach (var s in order)
        {
            var band = _eq.Slots[s]!.Value;
            var c = HandlePos(s);
            var color = EqBand.SlotColor(s);
            var r = S(7.5f) + (s == _hover || s == _drag ? S(1) : 0);
            if (s == _selected || s == _drag)
            {
                using var ring = new Pen(Color.White, S(1.6f));
                g.DrawEllipse(ring, c.X - r - S(2.5f), c.Y - r - S(2.5f), 2 * (r + S(2.5f)), 2 * (r + S(2.5f)));
            }
            var fillColor = band.On && _active ? color : Color.FromArgb(0x2A, 0x2A, 0x2E);
            using (var fill = new SolidBrush(fillColor)) g.FillEllipse(fill, c.X - r, c.Y - r, 2 * r, 2 * r);
            using (var edge = new Pen(band.On ? Color.FromArgb(0x60, 0, 0, 0) : color, S(1.2f))) g.DrawEllipse(edge, c.X - r, c.Y - r, 2 * r, 2 * r);
            var textColor = band.On && _active ? Color.FromArgb(0x16, 0x16, 0x19) : color;
            TextRenderer.DrawText(g, (s + 1).ToString(), _numFont, Rect(c.X - r, c.Y - r, 2 * r, 2 * r + S(0.5f)), textColor,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
        }
    }

    private void DrawTooltip(Graphics g)
    {
        var s = _drag >= 0 ? _drag : _hover;
        if (s < 0 || _eq.Slots[s] is not { } band || _menu.Visible) return;
        var title = $"{s + 1} · {EqBand.TypeName(band.Type)}" + (band.On ? "" : " (aus)");
        var line = EqBand.FormatFreq(band.FreqHz) + (band.HasGainValue ? "   " + EqBand.FormatGain(band.GainDb) : "") + "   Q " + EqBand.FormatQ(band.Q);
        var flags = TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        var s1 = TextRenderer.MeasureText(g, title, _tipBold, Size.Empty, flags);
        var s2 = TextRenderer.MeasureText(g, line, _tipFont, Size.Empty, flags);
        var w = Math.Max(s1.Width, s2.Width) + S(16);
        var h = s1.Height + s2.Height + S(10);
        var c = HandlePos(s);
        var x = Math.Clamp(c.X - w / 2, S(2), Width - w - S(2));
        var y = c.Y - S(16) - h;
        if (y < S(2)) y = c.Y + S(16);
        var box = new RectangleF(x, y, w, h);
        Theme.FillRoundRect(g, Color.FromArgb(0xEE, 0x1B, 0x1B, 0x1E), box, S(5));
        Theme.DrawRoundRect(g, EqBand.SlotColor(s), box, S(5));
        TextRenderer.DrawText(g, title, _tipBold, new Point((int)(x + S(8)), (int)(y + S(4))), Theme.Text, flags);
        TextRenderer.DrawText(g, line, _tipFont, new Point((int)(x + S(8)), (int)(y + S(5) + s1.Height)), Theme.TextDim, flags);
    }

    private void DrawOutputMeter(Graphics g, RectangleF p)
    {
        float w = S(4), gap = S(2.5f), x0 = p.Right + S(7);
        float YOut(float db) => p.Top + p.Height * (OutTop - Math.Clamp(db, OutBottom, OutTop)) / (OutTop - OutBottom);
        var y0 = YOut(0);
        float[] levels = [_outL, _outR];
        for (var i = 0; i < 2; i++)
        {
            var x = x0 + i * (w + gap);
            using (var track = new SolidBrush(Theme.Track)) g.FillRectangle(track, x, p.Top, w, p.Height);
            var lv = levels[i];
            if (lv <= OutBottom) continue;
            var yl = YOut(lv);
            using var b = new SolidBrush(Theme.Accent);
            g.FillRectangle(b, x, Math.Max(yl, y0), w, p.Bottom - Math.Max(yl, y0));
            if (yl < y0)
            {
                b.Color = Theme.Error; // über 0 dBFS: übersteuert
                g.FillRectangle(b, x, yl, w, y0 - yl);
            }
        }
        if (_outPeak > OutBottom)
        {
            using var pk = new SolidBrush(_outPeak > 0 ? Theme.Error : Theme.Text);
            g.FillRectangle(pk, x0, YOut(_outPeak) - 1, 2 * w + gap, 2);
        }
        using var tick = new Pen(Color.FromArgb(0x90, 0xFF, 0xFF, 0xFF), 1f);
        g.DrawLine(tick, x0 - S(2), y0, x0 + 2 * w + gap + S(2), y0);
    }

    private void DrawHints(Graphics g, RectangleF p)
    {
        const TextFormatFlags mc = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        if (_eq.BandCount == 0)
        {
            var mid = p.Top + p.Height * 0.32f;
            TextRenderer.DrawText(g, "Doppelklick in die Grafik fügt ein Band hinzu", _hintFont, Rect(p.Left, mid - S(11), p.Width, S(20)), Theme.Text, mc);
            TextRenderer.DrawText(g, "Ziehen: Frequenz und Stärke · Mausrad: Güte · Rechtsklick: Typ", _labelFont, Rect(p.Left, mid + S(9), p.Width, S(16)), Theme.TextDim, mc);
        }
        if (_flash != null && _clock.ElapsedMilliseconds < _flashUntil)
        {
            var size = TextRenderer.MeasureText(g, _flash, _tipFont);
            var box = new RectangleF(p.Left + (p.Width - size.Width) / 2 - S(10), p.Top + S(10), size.Width + S(20), size.Height + S(10));
            Theme.FillRoundRect(g, Color.FromArgb(0xF0, 0x2B, 0x1A, 0x12), box, S(5));
            Theme.DrawRoundRect(g, Theme.AccentStrong, box, S(5));
            TextRenderer.DrawText(g, _flash, _tipFont, Rectangle.Round(box), Theme.Accent, mc);
        }
    }

    private static Rectangle Rect(float x, float y, float w, float h) => Rectangle.Round(new RectangleF(x, y, w, h));

    /// <summary>Farbverlauf des Spektrogramms, deckend auf dem Grund der Grafik vorberechnet.</summary>
    private static int[] BuildPalette()
    {
        (float t, Color c)[] stops =
        [
            // gedämpft: Das Spektrogramm ist Hintergrund, Kurven und Punkte sollen davor klar bleiben
            (0.00f, PlotBack), (0.35f, Color.FromArgb(0x1A, 0x12, 0x10)), (0.60f, Color.FromArgb(0x38, 0x1D, 0x12)),
            (0.80f, Color.FromArgb(0x68, 0x31, 0x18)), (0.93f, Color.FromArgb(0x9C, 0x4A, 0x20)), (1.00f, Color.FromArgb(0xC8, 0x6A, 0x34)),
        ];
        var pal = new int[256];
        for (var i = 0; i < 256; i++)
        {
            var t = i / 255f;
            var k = 1;
            while (k < stops.Length - 1 && stops[k].t < t) k++;
            var (t0, c0) = stops[k - 1];
            var (t1, c1) = stops[k];
            var f = Math.Clamp((t - t0) / (t1 - t0), 0f, 1f);
            int r = (int)(c0.R + (c1.R - c0.R) * f), gg = (int)(c0.G + (c1.G - c0.G) * f), b = (int)(c0.B + (c1.B - c0.B) * f);
            pal[i] = unchecked((int)0xFF000000) | (r << 16) | (gg << 8) | b;
        }
        return pal;
    }

    // ------------------------------------------------------------------------------------------
    // Bedienung
    // ------------------------------------------------------------------------------------------
    private int HitHandle(Point pt)
    {
        var best = -1;
        var bestDist = S(11);
        // Das gewählte Band zuerst: Bei übereinanderliegenden Punkten gewinnt es (es liegt auch obenauf).
        foreach (var s in Enumerable.Range(0, EqSettings.SlotCount).Where(s => _eq.Slots[s] != null).OrderByDescending(s => s == _selected ? 1 : 0))
        {
            var c = HandlePos(s);
            var d = MathF.Sqrt((c.X - pt.X) * (c.X - pt.X) + (c.Y - pt.Y) * (c.Y - pt.Y));
            if (d < bestDist)
            {
                bestDist = d;
                best = s;
            }
        }
        return best;
    }

    private void Select(int slot)
    {
        if (slot == _selected) return;
        _selected = slot;
        Invalidate();
        SelectionChanged?.Invoke(slot);
    }

    private void Apply(EqSettings eq)
    {
        _eq = eq;
        _curvesDirty = true;
        Invalidate();
        Edited?.Invoke(eq);
    }

    private void EditBand(int slot, EqBand? band) => Apply(_eq.WithBand(slot, band));

    private void Flash(string text)
    {
        _flash = text;
        _flashUntil = _clock.ElapsedMilliseconds + 1800;
        Invalidate();
        var t = new System.Windows.Forms.Timer { Interval = 1900 };
        t.Tick += (_, _) => { t.Dispose(); Invalidate(); };
        t.Start();
    }

    private void AddBandAt(PointF pt, EqType type)
    {
        var slot = _eq.FreeSlot();
        if (slot < 0)
        {
            Flash($"Höchstens {EqSettings.MaxBands} Bänder – erst eines entfernen");
            return;
        }
        var f = (float)FreqAt(pt.X);
        var gain = EqBand.HasGain(type) ? (float)Math.Round(DbAt(pt.Y) * 2) / 2 : 0f;
        _selected = slot;
        EditBand(slot, new EqBand(type, f, gain, EqBand.DefaultQ(type)));
        SelectionChanged?.Invoke(slot);
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Focus();
        var hit = HitHandle(e.Location);
        if (e.Button == MouseButtons.Left)
        {
            if (hit >= 0)
            {
                Select(hit);
                _drag = hit;
                _dragFine = (ModifierKeys & Keys.Shift) != 0;
                _anchorMouse = e.Location;
                _anchorHandle = HandlePos(hit);
                Capture = true;
            }
            else
            {
                Select(-1);
            }
        }
        else if (e.Button == MouseButtons.Right)
        {
            if (hit >= 0) Select(hit);
            ShowMenu(hit, e.Location);
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_drag >= 0 && _eq.Slots[_drag] is { } band)
        {
            var fine = (ModifierKeys & Keys.Shift) != 0;
            if (fine != _dragFine)
            {
                // Beim Umschalten fein/grob neu ansetzen, damit der Punkt nicht springt.
                _dragFine = fine;
                _anchorMouse = e.Location;
                _anchorHandle = HandlePos(_drag);
            }
            var k = fine ? 0.15f : 1f;
            var hx = _anchorHandle.X + (e.X - _anchorMouse.X) * k;
            var hy = _anchorHandle.Y + (e.Y - _anchorMouse.Y) * k;
            var f = (float)Math.Clamp(FreqAt(hx), EqBand.FreqMin, EqBand.FreqMax);
            var updated = band with { FreqHz = f };
            if (band.HasGainValue)
            {
                var gdb = (float)Math.Clamp(DbAt(hy), -EqBand.GainMax, EqBand.GainMax);
                gdb = MathF.Round(gdb * 10) / 10;
                if (!fine && Math.Abs(gdb) < 0.35f) gdb = 0; // leichter Fang bei 0 dB
                updated = updated with { GainDb = gdb };
            }
            if (updated != band) EditBand(_drag, updated);
            return;
        }
        var hit = HitHandle(e.Location);
        Cursor = hit >= 0 ? Cursors.SizeAll : PlotRect.Contains(e.Location) ? Cursors.Cross : Cursors.Default;
        if (hit != _hover)
        {
            _hover = hit;
            Invalidate();
        }
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (_drag >= 0)
        {
            _drag = -1;
            Capture = false;
            Invalidate();
        }
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        if (_hover >= 0 && _drag < 0)
        {
            _hover = -1;
            Invalidate();
        }
    }

    protected override void OnMouseDoubleClick(MouseEventArgs e)
    {
        base.OnMouseDoubleClick(e);
        if (e.Button != MouseButtons.Left) return;
        var hit = HitHandle(e.Location);
        if (hit >= 0)
        {
            if (_eq.Slots[hit] is { HasGainValue: true } band) EditBand(hit, band with { GainDb = 0 });
        }
        else if (PlotRect.Contains(e.Location))
        {
            AddBandAt(e.Location, EqType.Peak);
        }
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        var hit = HitHandle(e.Location);  // der Punkt unter dem Zeiger, sonst das gewählte Band
        var slot = hit >= 0 ? hit : _selected;
        if (slot < 0 || _eq.Slots[slot] is not { } band) return;
        var fine = (ModifierKeys & Keys.Shift) != 0;
        var factor = Math.Pow(fine ? 1.03 : 1.12, e.Delta / 120.0);
        EditBand(slot, band with { Q = (float)(band.Q * factor) });
        if (e is HandledMouseEventArgs h) h.Handled = true;
    }

    protected override bool IsInputKey(Keys keyData) =>
        (keyData & Keys.KeyCode) is Keys.Left or Keys.Right or Keys.Up or Keys.Down || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (_selected < 0 || _eq.Slots[_selected] is not { } band) return;
        var fine = e.Shift;
        EqBand? updated = e.KeyCode switch
        {
            Keys.Left => band with { FreqHz = (float)(band.FreqHz / Math.Pow(2, fine ? 1 / 48.0 : 1 / 12.0)) },
            Keys.Right => band with { FreqHz = (float)(band.FreqHz * Math.Pow(2, fine ? 1 / 48.0 : 1 / 12.0)) },
            Keys.Up when band.HasGainValue => band with { GainDb = band.GainDb + (fine ? 0.1f : 0.5f) },
            Keys.Down when band.HasGainValue => band with { GainDb = band.GainDb - (fine ? 0.1f : 0.5f) },
            Keys.PageUp => band with { Q = band.Q * 1.12f },
            Keys.PageDown => band with { Q = band.Q / 1.12f },
            _ => null,
        };
        if (e.KeyCode is Keys.Delete or Keys.Back)
        {
            EditBand(_selected, null);
            Select(-1);
            e.Handled = true;
        }
        else if (updated is { } u)
        {
            EditBand(_selected, u);
            e.Handled = true;
        }
    }

    private void ShowMenu(int slot, Point at)
    {
        _menu.Items.ClearAndDispose();
        if (slot >= 0 && _eq.Slots[slot] is { } band)
        {
            _menu.Items.Add(new ToolStripMenuItem($"Band {slot + 1}") { Enabled = false });
            foreach (var t in EqBand.Types)
            {
                var item = new ToolStripMenuItem(EqBand.TypeDescription(t)) { Checked = t == band.Type };
                item.Click += (_, _) => { if (_eq.Slots[slot] is { } b) EditBand(slot, b.WithType(t)); };
                _menu.Items.Add(item);
            }
            _menu.Items.Add(new ToolStripSeparator());
            var off = new ToolStripMenuItem("Vorübergehend aus") { Checked = !band.On };
            off.Click += (_, _) => { if (_eq.Slots[slot] is { } b) EditBand(slot, b with { On = !b.On }); };
            _menu.Items.Add(off);
            if (band.HasGainValue)
            {
                var flat = new ToolStripMenuItem("Auf 0 dB") { Enabled = band.GainDb != 0 };
                flat.Click += (_, _) => { if (_eq.Slots[slot] is { } b) EditBand(slot, b with { GainDb = 0 }); };
                _menu.Items.Add(flat);
            }
            var remove = new ToolStripMenuItem("Entfernen") { ShortcutKeyDisplayString = "Entf" };
            remove.Click += (_, _) => { EditBand(slot, null); Select(-1); };
            _menu.Items.Add(remove);
        }
        else if (PlotRect.Contains(at))
        {
            if (_eq.FreeSlot() < 0)
            {
                _menu.Items.Add(new ToolStripMenuItem($"Höchstens {EqSettings.MaxBands} Bänder – erst eines entfernen") { Enabled = false });
            }
            else
            {
                _menu.Items.Add(new ToolStripMenuItem($"Band hier hinzufügen ({EqBand.FormatFreq(FreqAt(at.X))})") { Enabled = false });
                foreach (var t in EqBand.Types)
                {
                    var item = new ToolStripMenuItem(EqBand.TypeDescription(t));
                    item.Click += (_, _) => AddBandAt(at, t);
                    _menu.Items.Add(item);
                }
            }
        }
        else
        {
            return;
        }
        _hover = -1;
        _menu.Show(this, at);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _sgram?.Dispose();
            _menu.Dispose();
            _labelFont.Dispose();
            _tipFont.Dispose();
            _tipBold.Dispose();
            _numFont.Dispose();
            _hintFont.Dispose();
        }
        base.Dispose(disposing);
    }
}
