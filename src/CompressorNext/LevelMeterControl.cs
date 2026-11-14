using System.ComponentModel;
using System.Diagnostics;

namespace CompressorNext;

/// <summary>
/// Pegelanzeige nach Art von LoudMax, übernommen aus Player NEXT (LevelMeterView): Eingang L/R vor dem
/// Maximierer, seine Reduktion und Ausgang L/R danach.
/// Skala −60 … +3 dBFS, die oberen 20 dB über zwei Drittel der Breite. Die Reduktion wächst von der
/// 0-dB-Marke nach links auf derselben Skala. Die Schwelle ist auf dem Eingang markiert, der Ausgangspegel
/// auf dem Ausgang; Eingang über der Schwelle im kräftigen Akzent, über 0 dBFS rot.
/// Spitzenmarke hält 1,5 s, Balken fallen 20 dB/s.
/// </summary>
internal sealed class LevelMeterControl : NextControl
{
    private const float Floor = -60f, TopDb = 3f;
    private static readonly float[] Ticks = [-40f, -20f, -12f, -6f, -3f, 0f];
    private static readonly float[] TickLabels = [-40f, -20f, -12f, -6f, 0f];
    private const float FallDbPerSec = 20f, ReturnDbPerSec = 30f, HoldToleranceDb = 0.5f;
    private const long HoldMs = 1500, NoSignalMs = 2000;

    private readonly float[] _level = [Floor, Floor, Floor, Floor];
    private readonly float[] _peak = [Floor, Floor, Floor, Floor];
    private readonly long[] _peakUntil = new long[4];
    private float _reduction, _reductionPeak;
    private long _reductionPeakUntil;
    private long _lastTick, _lastData;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private bool _on = true;
    private float _thresholdDb = -18f, _ceilingDb = -1f;
    private bool _noSignal = true;

    private readonly Font _labelFont = Theme.Font(8.25f);
    private readonly Font _scaleFont = Theme.Font(7f);
    private readonly Font _messageFont = Theme.Font(9f);

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string NoSignalTitle { get; set; } = "Kein Signal";

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string NoSignalText { get; set; } = "Sobald Ton läuft, erscheinen hier die Pegel.";

    /// <summary>Höhe in logischen Pixeln (96 dpi).</summary>
    public const int LogicalHeight = 116;

    public LevelMeterControl()
    {
        _lastData = -NoSignalMs;
    }

    public void SetMaximizer(bool on, float thresholdDb, float ceilingDb)
    {
        _on = on;
        _thresholdDb = thresholdDb;
        _ceilingDb = ceilingDb;
        Invalidate();
    }

    private static float ToDb(float linear) => linear <= 1e-6f ? Floor : Math.Max(Floor, 20f * MathF.Log10(linear));

    /// <summary>Einmal pro Bild aufrufen; reading = null, wenn seit dem letzten Bild nichts Neues kam.</summary>
    public void Advance(MeterReading? reading, bool streamAlive)
    {
        var now = _clock.ElapsedMilliseconds;
        var dt = _lastTick == 0 ? 0f : Math.Clamp((now - _lastTick) / 1000f, 0f, 0.25f);
        _lastTick = now;
        if (streamAlive) _lastData = now;

        Span<float> db = stackalloc float[4];
        if (reading is { } r)
        {
            db[0] = ToDb(r.InL); db[1] = ToDb(r.InR); db[2] = ToDb(r.OutL); db[3] = ToDb(r.OutR);
        }
        else
        {
            db.Fill(Floor);
        }
        for (var i = 0; i < 4; i++)
        {
            _level[i] = Math.Max(db[i], _level[i] - FallDbPerSec * dt);
            if (db[i] > Floor && db[i] >= _peak[i] - HoldToleranceDb)
            {
                if (db[i] > _peak[i]) _peak[i] = db[i]; // ein gleichbleibender Pegel lässt die Marke stehen
                _peakUntil[i] = now + HoldMs;
            }
            else if (now > _peakUntil[i])
            {
                _peak[i] = Math.Max(_level[i], _peak[i] - FallDbPerSec * dt);
            }
        }
        var red = reading is { } rr ? -Math.Min(0f, ToDb(rr.MinGain)) : 0f;
        _reduction = Math.Max(red, _reduction - ReturnDbPerSec * dt);
        if (red > 0f && red >= _reductionPeak - HoldToleranceDb)
        {
            if (red > _reductionPeak) _reductionPeak = red;
            _reductionPeakUntil = now + HoldMs;
        }
        else if (now > _reductionPeakUntil)
        {
            _reductionPeak = Math.Max(_reduction, _reductionPeak - ReturnDbPerSec * dt);
        }
        _noSignal = now - _lastData > NoSignalMs;
        Invalidate();
    }

    private float BarLeft => S(84);
    private float BarRight => Width - S(48);

    /// <summary>dBFS → x: −60 … −20 dB über die ersten 35 %, −20 … +3 dB über den Rest.</summary>
    private float X(float db)
    {
        var d = Math.Clamp(db, Floor, TopDb);
        var f = d >= -20f ? 0.35f + 0.65f * (d + 20f) / (TopDb + 20f) : 0.35f * (d - Floor) / (-20f - Floor);
        return BarLeft + (BarRight - BarLeft) * f;
    }

    protected override void Draw(Graphics g)
    {
        float rowH = S(12), pairGap = S(5), groupGap = S(12);
        var yInL = S(4);
        var yInR = yInL + rowH + pairGap;
        var yGr = yInR + rowH + groupGap;
        var yOutL = yGr + rowH + groupGap;
        var yOutR = yOutL + rowH + pairGap;
        var yScale = yOutR + rowH + S(4);
        float l = BarLeft, r = BarRight;

        using (var grid = new Pen(Color.FromArgb(0x22, 0xFF, 0xFF, 0xFF), 1f))
            foreach (var t in Ticks) g.DrawLine(grid, X(t), yInL, X(t), yOutR + rowH);

        var split = _on ? Math.Min(_thresholdDb, 0f) : 0f;
        DrawLevel(g, yInL, rowH, 0, split);
        DrawLevel(g, yInR, rowH, 1, split);
        DrawReduction(g, yGr, rowH);
        DrawLevel(g, yOutL, rowH, 2, 0f);
        DrawLevel(g, yOutR, rowH, 3, 0f);
        if (_on)
        {
            using var marker = new Pen(Theme.Text, S(2));
            g.DrawLine(marker, X(_thresholdDb), yInL - pairGap + 1, X(_thresholdDb), yInR + rowH + pairGap - 1);
            g.DrawLine(marker, X(_ceilingDb), yOutL - pairGap + 1, X(_ceilingDb), yOutR + rowH + pairGap - 1);
        }

        const TextFormatFlags left = TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        const TextFormatFlags right = TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        TextRenderer.DrawText(g, "Eingang", _labelFont, Rect(0, yInL, S(60), yInR + rowH - yInL), Theme.TextDim, left);
        TextRenderer.DrawText(g, "Reduktion", _labelFont, Rect(0, yGr - S(2), S(64), rowH + S(4)), Theme.TextDim, left);
        TextRenderer.DrawText(g, "Ausgang", _labelFont, Rect(0, yOutL, S(60), yOutR + rowH - yOutL), Theme.TextDim, left);

        float[] rows = [yInL, yInR, yOutL, yOutR];
        string[] names = ["L", "R", "L", "R"];
        for (var i = 0; i < 4; i++)
        {
            TextRenderer.DrawText(g, names[i], _labelFont, Rect(l - S(22), rows[i] - S(2), S(16), rowH + S(4)), Theme.TextFaint, right);
            TextRenderer.DrawText(g, LevelText(_peak[i]), _labelFont, Rect(r, rows[i] - S(2), Width - r, rowH + S(4)), Theme.Text, right);
        }
        TextRenderer.DrawText(g, ReductionText(_reductionPeak), _labelFont, Rect(r, yGr - S(2), Width - r, rowH + S(4)), Theme.Text, right);

        const TextFormatFlags center = TextFormatFlags.HorizontalCenter | TextFormatFlags.Top | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
        foreach (var t in TickLabels)
            TextRenderer.DrawText(g, t < 0 ? "−" + (int)-t : ((int)t).ToString(), _scaleFont, Rect(X(t) - S(15), yScale, S(30), S(14)), Theme.TextFaint, center);
        TextRenderer.DrawText(g, "dB", _scaleFont, Rect(r, yScale, Width - r, S(14)), Theme.TextFaint, right);

        if (_noSignal)
        {
            using var veil = new SolidBrush(Color.FromArgb(0xCC, Theme.Card));
            g.FillRectangle(veil, l, yInL - pairGap, r - l, yOutR + rowH + 2 * pairGap - yInL);
            var mid = (yInL + yOutR + rowH) / 2;
            const TextFormatFlags mc = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
            TextRenderer.DrawText(g, NoSignalTitle, _messageFont, Rect(l, mid - S(20), r - l, S(20)), Theme.Text, mc);
            TextRenderer.DrawText(g, NoSignalText, _labelFont, Rect(l, mid, r - l, S(20)), Theme.TextDim, mc | TextFormatFlags.EndEllipsis);
        }
    }

    private static Rectangle Rect(float x, float y, float w, float h) => Rectangle.Round(new RectangleF(x, y, w, h));

    private void DrawTrack(Graphics g, float y, float rowH)
    {
        var rect = new RectangleF(BarLeft, y, BarRight - BarLeft, rowH);
        Theme.FillRoundRect(g, Theme.Track, rect, S(3));
        Theme.DrawRoundRect(g, Theme.CardBorder, rect, S(3));
    }

    private void DrawLevel(Graphics g, float y, float rowH, int i, float splitDb)
    {
        DrawTrack(g, y, rowH);
        float top = y + 1, h = rowH - 2;
        var lv = _level[i];
        if (lv > Floor)
        {
            float x0 = BarLeft + 1, xl = X(lv), xs = X(splitDb), xz = X(0f);
            using var b = new SolidBrush(Theme.Accent);
            g.FillRectangle(b, x0, top, Math.Max(0, Math.Min(xl, xs) - x0), h);
            if (xl > xs && xz > xs)
            {
                b.Color = Theme.AccentStrong;
                g.FillRectangle(b, xs, top, Math.Min(xl, xz) - xs, h);
            }
            if (xl > xz)
            {
                b.Color = Theme.Error;
                g.FillRectangle(b, xz, top, xl - xz, h);
            }
        }
        var p = _peak[i];
        if (p > Floor)
        {
            using var b = new SolidBrush(p > 0f ? Theme.Error : Theme.Text);
            g.FillRectangle(b, X(p) - 1.5f, y, 3f, rowH);
        }
    }

    private void DrawReduction(Graphics g, float y, float rowH)
    {
        DrawTrack(g, y, rowH);
        var xz = X(0f);
        if (_reduction > 0.05f)
        {
            using var b = new SolidBrush(Theme.AccentStrong);
            var x = X(-_reduction);
            g.FillRectangle(b, x, y + 1, xz - x, rowH - 2);
        }
        if (_reductionPeak > 0.05f)
        {
            using var b = new SolidBrush(Theme.Text);
            g.FillRectangle(b, X(-_reductionPeak) - 1.5f, y, 3f, rowH);
        }
    }

    private static string LevelText(float db) => db <= Floor ? "−∞" : Theme.Db(MathF.Round(db * 10) / 10, 1);

    private static string ReductionText(float db)
    {
        var v = MathF.Round(db * 10) / 10;
        return v == 0 ? "0,0" : Theme.Db(-v, 1);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _labelFont.Dispose();
            _scaleFont.Dispose();
            _messageFont.Dispose();
        }
        base.Dispose(disposing);
    }
}
