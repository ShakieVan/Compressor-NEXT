namespace CompressorNext;

/// <summary>Zeile der Geräteliste.</summary>
internal sealed record DeviceRow(OutputDevice Device, bool Excluded, bool Streaming);

/// <summary>Liste der Ausgabegeräte: Name, Zustand und ein Schalter "hier verwenden".</summary>
internal sealed class DeviceListControl : NextControl
{
    private List<DeviceRow> _rows = [];
    private readonly Font _nameFont = Theme.Font(9f);
    private readonly Font _statusFont = Theme.Font(8.25f);
    private int _hover = -1;

    /// <summary>Der Schalter eines Geräts wurde betätigt (Gerät, neuer Zustand "verwenden").</summary>
    public event Action<OutputDevice, bool>? UseChanged;

    public const int RowHeight = 40;

    public int LogicalHeight => Math.Max(1, _rows.Count) * RowHeight;

    public void SetRows(List<DeviceRow> rows)
    {
        var same = rows.Count == _rows.Count && rows.Zip(_rows).All(p => p.First == p.Second);
        if (same) return;
        _rows = rows;
        Invalidate();
    }

    public IReadOnlyList<DeviceRow> Rows => _rows;

    private RectangleF ToggleRect(int index)
    {
        float w = S(38), h = S(20);
        return new RectangleF(Width - w - S(2), index * S(RowHeight) + (S(RowHeight) - h) / 2, w, h);
    }

    private int HitToggle(Point p)
    {
        for (var i = 0; i < _rows.Count; i++)
        {
            var r = ToggleRect(i);
            r.Inflate(S(6), S(8));
            if (r.Contains(p)) return i;
        }
        return -1;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        var hit = HitToggle(e.Location);
        Cursor = hit >= 0 ? Cursors.Hand : Cursors.Default;
        if (hit != _hover) { _hover = hit; Invalidate(); }
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        if (_hover != -1) { _hover = -1; Invalidate(); }
    }

    protected override void OnMouseClick(MouseEventArgs e)
    {
        base.OnMouseClick(e);
        var hit = HitToggle(e.Location);
        if (hit >= 0 && e.Button == MouseButtons.Left) UseChanged?.Invoke(_rows[hit].Device, _rows[hit].Excluded);
    }

    public static (string Text, Color Color) Status(DeviceRow row)
    {
        var d = row.Device;
        if (!d.Bound) return ("nicht eingebunden", Theme.Error);
        if (d.EnhancementsOff) return ("Windows-Audioverbesserungen aus", Theme.Error);
        if (row.Excluded) return ("ausgenommen", Theme.TextFaint);
        if (row.Streaming) return ("aktiv", Theme.Ok);
        return ("bereit", Theme.TextDim);
    }

    protected override void Draw(Graphics g)
    {
        if (_rows.Count == 0)
        {
            TextRenderer.DrawText(g, "Kein Ausgabegerät angeschlossen.", _nameFont, new Rectangle(0, 0, Width, (int)S(RowHeight)), Theme.TextDim,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            return;
        }
        for (var i = 0; i < _rows.Count; i++)
        {
            var row = _rows[i];
            var y = i * S(RowHeight);
            if (i > 0)
            {
                using var line = new Pen(Theme.CardBorder, 1f);
                g.DrawLine(line, 0, y, Width, y);
            }
            var (status, color) = Status(row);
            var dot = S(8);
            using (var b = new SolidBrush(color)) g.FillEllipse(b, S(2), y + S(10), dot, dot);

            var toggle = ToggleRect(i);
            var textRight = toggle.X - S(10);
            var name = row.Device.IsDefault ? row.Device.DisplayName + "  ·  Standard" : row.Device.DisplayName;
            const TextFormatFlags flags = TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding;
            TextRenderer.DrawText(g, name, _nameFont, Rectangle.Round(new RectangleF(S(18), y + S(3), textRight - S(18), S(18))), Theme.Text, flags);
            TextRenderer.DrawText(g, status, _statusFont, Rectangle.Round(new RectangleF(S(18), y + S(20), textRight - S(18), S(16))), color, flags);

            var on = !row.Excluded;
            var radius = toggle.Height / 2;
            Theme.FillRoundRect(g, on ? Theme.AccentStrong : Theme.Track, toggle, radius);
            Theme.DrawRoundRect(g, on ? Theme.AccentStrong : Theme.Off, toggle, radius, _hover == i ? S(1.6f) : S(1f));
            var d = toggle.Height - S(6);
            using var knob = new SolidBrush(on ? Color.White : Theme.TextDim);
            g.FillEllipse(knob, on ? toggle.Right - d - S(3) : toggle.X + S(3), toggle.Y + S(3), d, d);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _nameFont.Dispose();
            _statusFont.Dispose();
        }
        base.Dispose(disposing);
    }
}
