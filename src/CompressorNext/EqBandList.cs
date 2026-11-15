using System.ComponentModel;

namespace CompressorNext;

/// <summary>
/// Dynamische Liste der Equalizer-Bänder: Es gibt nur Zeilen für vorhandene Bänder (höchstens
/// <see cref="EqSettings.MaxBands"/>), darunter "+ Band". Jede Zeile: Nummer (Klick = an/aus), Typ, Frequenz,
/// Verstärkung, Güte (Schieber und Feld), Entfernen.
/// </summary>
internal sealed class EqBandList : Panel
{
    public const int RowHeight = 30, RowGap = 2;

    /// <summary>Höhe in logischen Pixeln, wenn alle Bänder belegt sind (Platz wird freigehalten).</summary>
    public const int LogicalHeight = EqSettings.MaxBands * (RowHeight + RowGap) + 36;

    private readonly EqBandRow[] _rows = new EqBandRow[EqSettings.SlotCount];
    private readonly FlatButton _add = new() { Text = "+ Band", AccessibleName = "Band hinzufügen" };
    private readonly Label _hint = new() { AutoSize = false, Text = "Werte: ziehen, Mausrad oder Doppelklick zum Eintippen" };
    private EqSettings _eq = EqSettings.Default;
    private int _selected = -1;
    private bool _active = true;

    public event Action<EqSettings>? Edited;
    public event Action<int>? SelectionChanged;

    public EqBandList()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        BackColor = Theme.Card;
        _hint.Font = Theme.Font(8.25f);
        _hint.ForeColor = Theme.TextFaint;
        _hint.BackColor = Theme.Card;
        _hint.TextAlign = ContentAlignment.MiddleLeft;
        for (var i = 0; i < _rows.Length; i++)
        {
            var row = new EqBandRow(i) { Visible = false };
            row.Edited += (slot, band) => Apply(_eq.WithBand(slot, band), band == null ? -1 : slot);
            row.Activated += Select;
            _rows[i] = row;
            Controls.Add(row);
        }
        Controls.Add(_add);
        Controls.Add(_hint);
        _add.Click += (_, _) => AddDefault();
    }

    private float S(float v) => v * DeviceDpi / 96f;

    public void SetSettings(EqSettings eq, int selected, bool active)
    {
        _eq = eq;
        _active = active;
        _selected = selected >= 0 && eq.Slots[selected] != null ? selected : -1;
        for (var i = 0; i < _rows.Length; i++)
        {
            var band = eq.Slots[i];
            _rows[i].Visible = band != null;
            if (band is { } b) _rows[i].Show(b, i == _selected, active);
        }
        LayoutRows();
    }

    public void SetSelected(int slot)
    {
        _selected = slot;
        for (var i = 0; i < _rows.Length; i++)
            if (_eq.Slots[i] is { } b) _rows[i].Show(b, i == slot, _active);
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        LayoutRows();
    }

    private void LayoutRows()
    {
        var y = 0;
        for (var i = 0; i < _rows.Length; i++)
        {
            if (_eq.Slots[i] == null) continue;
            _rows[i].SetBounds(0, (int)S(y), Width, (int)S(RowHeight));
            y += RowHeight + RowGap;
        }
        var canAdd = _eq.FreeSlot() >= 0;
        _add.Visible = canAdd;
        _add.SetBounds(0, (int)S(y + 4), (int)S(84), (int)S(28));
        _hint.Visible = _eq.BandCount > 0;
        var hx = canAdd ? 96 : 0;
        _hint.SetBounds((int)S(hx), (int)S(y + 4), Width - (int)S(hx), (int)S(28));
    }

    private void Select(int slot)
    {
        if (slot == _selected) return;
        SetSelected(slot);
        SelectionChanged?.Invoke(slot);
    }

    private void Apply(EqSettings eq, int select)
    {
        _eq = eq;
        Edited?.Invoke(eq);
        if (select != _selected)
        {
            _selected = select;
            SelectionChanged?.Invoke(select);
        }
    }

    /// <summary>Neues Band (0 dB) an einer Frequenz, die von den vorhandenen Bändern möglichst weit weg liegt.</summary>
    private void AddDefault()
    {
        var slot = _eq.FreeSlot();
        if (slot < 0) return;
        double[] candidates = [1000, 100, 5000, 300, 3000, 60, 10000, 500, 2000, 150];
        var freq = 1000.0;
        foreach (var c in candidates)
        {
            if (_eq.Slots.All(b => b is not { } x || Math.Abs(Math.Log2(x.FreqHz / c)) >= 0.8))
            {
                freq = c;
                break;
            }
        }
        var eq = _eq.WithBand(slot, new EqBand(EqType.Peak, (float)freq, 0f, EqBand.DefaultQ(EqType.Peak)));
        Apply(eq, slot);
    }
}

/// <summary>Eine Zeile der Bandliste.</summary>
internal sealed class EqBandRow : Panel
{
    private readonly int _slot;
    private readonly BandBadge _badge;
    private readonly FlatButton _type = new() { AccessibleName = "Filtertyp" };
    private readonly ValueField _freq = new() { AccessibleName = "Frequenz" };
    private readonly ValueField _gain = new() { AccessibleName = "Verstärkung" };
    private readonly LogSlider _q = new() { AccessibleName = "Güte (Schärfe)" };
    private readonly ValueField _qValue = new() { AccessibleName = "Güte" };
    private readonly FlatButton _remove = new() { Text = "×", AccessibleName = "Band entfernen" };
    private readonly ContextMenuStrip _typeMenu = new();
    private readonly ToolTip _tips = new();
    private EqBand _band;
    private bool _selected;

    /// <summary>Band geändert (null = entfernt).</summary>
    public event Action<int, EqBand?>? Edited;

    /// <summary>Zeile angeklickt (wählt das Band).</summary>
    public event Action<int>? Activated;

    public EqBandRow(int slot)
    {
        _slot = slot;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Card;
        _badge = new BandBadge(slot);
        _remove.Font = Theme.Font(11f);

        _freq.Format = v => EqBand.FormatFreq(v);
        _freq.Nudge = (v, steps, fine) => Math.Clamp(v * Math.Pow(2, steps * (fine ? 1 / 96.0 : 1 / 24.0)), EqBand.FreqMin, EqBand.FreqMax);
        _freq.Parse = s => EqBand.TryParseFreq(s, out var f) ? f : null;
        _gain.Format = v => EqBand.FormatGain(v);
        _gain.Nudge = (v, steps, fine) => Math.Clamp(Math.Round((v + steps * (fine ? 0.1 : 0.5)) * 10) / 10, -EqBand.GainMax, EqBand.GainMax);
        _gain.Parse = s => EqBand.TryParseNumber(s, out var g) ? g : null;
        _qValue.Format = v => EqBand.FormatQ(v);
        _qValue.Nudge = (v, steps, fine) => Math.Clamp(v * Math.Pow(fine ? 1.02 : 1.08, steps), EqBand.QMin, EqBand.QMax(_band.Type));
        _qValue.Parse = s => EqBand.TryParseNumber(s, out var q) ? q : null;

        Controls.AddRange([_badge, _type, _freq, _gain, _q, _qValue, _remove]);

        _badge.Click += (_, _) => { Activated?.Invoke(_slot); Edited?.Invoke(_slot, _band with { On = !_band.On }); };
        _type.Click += (_, _) => { Activated?.Invoke(_slot); ShowTypeMenu(); };
        _freq.ValueEdited += v => Edited?.Invoke(_slot, _band with { FreqHz = (float)v });
        _gain.ValueEdited += v => Edited?.Invoke(_slot, _band with { GainDb = (float)v });
        _qValue.ValueEdited += v => Edited?.Invoke(_slot, _band with { Q = (float)v });
        _q.ValueChanged += (_, _) => Edited?.Invoke(_slot, _band with { Q = _q.Value });
        _remove.Click += (_, _) => Edited?.Invoke(_slot, null);
        foreach (Control c in Controls) c.MouseDown += (_, _) => Activated?.Invoke(_slot);

        _tips.SetToolTip(_badge, "Band ein/aus");
        _tips.SetToolTip(_remove, "Band entfernen");
        _tips.SetToolTip(_q, "Güte: links breit, rechts schmal (Mausrad auf dem Punkt in der Grafik geht auch)");
    }

    private float S(float v) => v * DeviceDpi / 96f;

    public void Show(EqBand band, bool selected, bool active)
    {
        _band = band;
        _badge.SetState(band.On, active);
        _type.Text = EqBand.TypeName(band.Type) + "  ▾";
        _freq.SetValue(band.FreqHz);
        _gain.Enabled = band.HasGainValue;
        _gain.SetValue(band.GainDb);
        _q.Minimum = EqBand.QMin;
        _q.Maximum = EqBand.QMax(band.Type);
        _q.SetSilently(band.Q);
        _qValue.SetValue(band.Q);
        if (_selected != selected)
        {
            _selected = selected;
            var back = selected ? RowSelected : Theme.Card;
            BackColor = back;
            foreach (Control c in Controls) c.BackColor = back;
            Invalidate();
        }
    }

    private static readonly Color RowSelected = Color.FromArgb(0x23, 0x23, 0x27);

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Activated?.Invoke(_slot);
    }

    protected override void OnLayout(LayoutEventArgs levent)
    {
        base.OnLayout(levent);
        var w = Width / (DeviceDpi / 96f);
        int H(float v) => (int)Math.Round(S(v));
        var cy = 15f;
        _badge.SetBounds(H(3), H(cy - 11), H(22), H(22));
        _type.SetBounds(H(32), H(cy - 13), H(106), H(26));
        _freq.SetBounds(H(144), H(cy - 12), H(74), H(24));
        _gain.SetBounds(H(222), H(cy - 12), H(66), H(24));
        var qRight = w - 32 - 48;
        _q.SetBounds(H(306), H(cy - 13), H(Math.Max(40, qRight - 306)), H(26));
        _qValue.SetBounds(H(w - 32 - 46), H(cy - 12), H(44), H(24));
        _remove.SetBounds(H(w - 28), H(cy - 13), H(26), H(26));
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(Theme.Card);
        if (_selected)
        {
            var r = new RectangleF(0.5f, 0.5f, Width - 1, Height - 1);
            Theme.FillRoundRect(g, RowSelected, r, S(6));
            Theme.DrawRoundRect(g, Color.FromArgb(0x80, EqBand.SlotColor(_slot)), r, S(6));
        }
        using var font = Theme.Font(8.25f, FontStyle.Bold);
        TextRenderer.DrawText(g, "Q", font, Rectangle.Round(new RectangleF(S(292), 0, S(16), Height)), Theme.TextDim,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
    }

    private void ShowTypeMenu()
    {
        _typeMenu.Items.ClearAndDispose();
        foreach (var t in EqBand.Types)
        {
            var item = new ToolStripMenuItem(EqBand.TypeDescription(t)) { Checked = t == _band.Type };
            item.Click += (_, _) => Edited?.Invoke(_slot, _band.WithType(t));
            _typeMenu.Items.Add(item);
        }
        _typeMenu.Show(_type, new Point(0, _type.Height));
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _typeMenu.Dispose();
            _tips.Dispose();
        }
        base.Dispose(disposing);
    }
}

/// <summary>Nummer eines Bandes in seiner Farbe; Klick schaltet das Band an und aus.</summary>
internal sealed class BandBadge : NextControl
{
    private readonly int _slot;
    private bool _on = true, _active = true, _hover;
    private readonly Font _font = Theme.Font(8f, FontStyle.Bold);

    public BandBadge(int slot)
    {
        _slot = slot;
        Cursor = Cursors.Hand;
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        AccessibleRole = AccessibleRole.CheckButton;
        AccessibleName = $"Band {slot + 1} ein/aus";
    }

    public void SetState(bool on, bool active)
    {
        if (on == _on && active == _active) return;
        _on = on;
        _active = active;
        Invalidate();
    }

    protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); _hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); _hover = false; Invalidate(); }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.KeyCode is Keys.Space or Keys.Enter)
        {
            OnClick(EventArgs.Empty);
            e.Handled = true;
        }
    }

    protected override void Draw(Graphics g)
    {
        var color = EqBand.SlotColor(_slot);
        var d = Math.Min(Width, Height) - S(2);
        var r = new RectangleF((Width - d) / 2, (Height - d) / 2, d, d);
        if (_on)
        {
            using var b = new SolidBrush(_active ? color : Theme.Off);
            g.FillEllipse(b, r);
        }
        using (var pen = new Pen(_hover || (Focused && ShowFocusCues) ? Theme.Text : color, S(1.5f))) g.DrawEllipse(pen, r);
        TextRenderer.DrawText(g, (_slot + 1).ToString(), _font, Rectangle.Round(r), _on ? Color.FromArgb(0x16, 0x16, 0x19) : color,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _font.Dispose();
        base.Dispose(disposing);
    }
}

/// <summary>
/// Zahlenfeld: senkrecht ziehen oder Mausrad (Umschalt = fein), Pfeiltasten, Doppelklick zum Eintippen.
/// </summary>
internal sealed class ValueField : NextControl
{
    private double _value;
    private bool _dragging, _hover;
    private int _dragStartY;
    private double _dragStartValue;
    private TextBox? _edit;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Func<double, string> Format { get; set; } = v => v.ToString("0.##");

    /// <summary>Wert um Schritte verändern (dritter Wert: fein) und begrenzen.</summary>
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Func<double, int, bool, double> Nudge { get; set; } = (v, steps, _) => v + steps;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Func<string, double?> Parse { get; set; } = _ => null;

    /// <summary>Wert durch die Bedienung geändert.</summary>
    public event Action<double>? ValueEdited;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public double Value => _value;

    public ValueField()
    {
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        Cursor = Cursors.SizeNS;
        AccessibleRole = AccessibleRole.SpinButton;
    }

    public void SetValue(double v)
    {
        if (_value == v) return;
        _value = v;
        Invalidate();
    }

    private void Commit(double v)
    {
        if (v == _value) return;
        _value = v;
        Invalidate();
        ValueEdited?.Invoke(v);
    }

    protected override void OnEnabledChanged(EventArgs e)
    {
        base.OnEnabledChanged(e);
        Cursor = Enabled ? Cursors.SizeNS : Cursors.Default;
        Invalidate();
    }

    protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); _hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); _hover = false; Invalidate(); }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button != MouseButtons.Left || !Enabled) return;
        Focus();
        _dragging = true;
        _dragStartY = e.Y;
        _dragStartValue = _value;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!_dragging) return;
        var steps = (int)((_dragStartY - e.Y) / S(4));
        Commit(Nudge(_dragStartValue, steps, (ModifierKeys & Keys.Shift) != 0));
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _dragging = false;
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        if (!Enabled) return;
        Commit(Nudge(_value, Math.Sign(e.Delta), (ModifierKeys & Keys.Shift) != 0));
        if (e is HandledMouseEventArgs h) h.Handled = true;
    }

    protected override void OnMouseDoubleClick(MouseEventArgs e)
    {
        base.OnMouseDoubleClick(e);
        if (Enabled) BeginEdit();
    }

    protected override bool IsInputKey(Keys keyData) => (keyData & Keys.KeyCode) is Keys.Up or Keys.Down || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (!Enabled) return;
        switch (e.KeyCode)
        {
            case Keys.Up: Commit(Nudge(_value, 1, e.Shift)); break;
            case Keys.Down: Commit(Nudge(_value, -1, e.Shift)); break;
            case Keys.Enter or Keys.F2: BeginEdit(); break;
            default: return;
        }
        e.Handled = true;
    }

    private void BeginEdit()
    {
        if (_edit != null) return;
        var box = new TextBox
        {
            BorderStyle = BorderStyle.None,
            BackColor = Theme.Track,
            ForeColor = Theme.Text,
            Font = Theme.Font(9f),
            TextAlign = HorizontalAlignment.Center,
            Text = Format(_value),
        };
        var h = box.PreferredHeight;
        box.SetBounds((int)S(4), (Height - h) / 2, Width - (int)S(8), h);
        var done = false;
        void Finish(bool accept)
        {
            if (done) return;
            done = true;
            if (accept && Parse(box.Text) is { } v) Commit(Nudge(v, 0, false)); // Nudge mit 0 Schritten begrenzt den Wert
            _edit = null;
            Controls.Remove(box);
            box.Dispose();
            Invalidate();
        }
        box.KeyDown += (_, ke) =>
        {
            if (ke.KeyCode == Keys.Enter) { Finish(true); ke.SuppressKeyPress = true; }
            else if (ke.KeyCode == Keys.Escape) { Finish(false); ke.SuppressKeyPress = true; }
        };
        box.LostFocus += (_, _) => BeginInvoke(() => Finish(true));
        _edit = box;
        Controls.Add(box);
        box.Focus();
        box.SelectAll();
        Invalidate();
    }

    protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

    protected override void Draw(Graphics g)
    {
        var r = new RectangleF(0.5f, 0.5f, Width - 1, Height - 1);
        Theme.FillRoundRect(g, Theme.Track, r, S(5));
        var border = !Enabled ? Theme.CardBorder : _dragging || _edit != null ? Theme.AccentStrong : _hover || (Focused && ShowFocusCues) ? Theme.TextFaint : Theme.CardBorder;
        Theme.DrawRoundRect(g, border, r, S(5));
        if (_edit != null) return;
        TextRenderer.DrawText(g, Enabled ? Format(_value) : "–", Font, Rectangle.Round(r), Enabled ? Theme.Text : Theme.TextFaint,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
    }
}

/// <summary>Schieberegler mit logarithmischer Skala (für die Güte).</summary>
internal sealed class LogSlider : NextControl
{
    private float _value = 1f;
    private bool _dragging;

    public event EventHandler? ValueChanged;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public float Minimum { get; set; } = 0.1f;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public float Maximum { get; set; } = 10f;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public float Value
    {
        get => _value;
        set
        {
            var v = Math.Clamp(value, Minimum, Maximum);
            if (v == _value) return;
            _value = v;
            Invalidate();
            ValueChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public LogSlider()
    {
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        Cursor = Cursors.Hand;
        AccessibleRole = AccessibleRole.Slider;
    }

    public void SetSilently(float value)
    {
        var v = Math.Clamp(value, Minimum, Maximum);
        if (v == _value) return;
        _value = v;
        Invalidate();
    }

    private float Pad => S(9);

    private float XOf(float v)
    {
        var f = MathF.Log(v / Minimum) / MathF.Log(Maximum / Minimum);
        return Pad + (Width - 2 * Pad) * Math.Clamp(f, 0f, 1f);
    }

    private float ValueAt(int x)
    {
        var f = Math.Clamp((x - Pad) / Math.Max(1f, Width - 2 * Pad), 0f, 1f);
        var v = Minimum * MathF.Pow(Maximum / Minimum, f);
        return MathF.Round(v * 100) / 100;
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button != MouseButtons.Left) return;
        Focus();
        _dragging = true;
        Value = ValueAt(e.X);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_dragging) Value = ValueAt(e.X);
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _dragging = false;
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        Value *= MathF.Pow((ModifierKeys & Keys.Shift) != 0 ? 1.02f : 1.08f, Math.Sign(e.Delta));
        if (e is HandledMouseEventArgs h) h.Handled = true;
    }

    protected override bool IsInputKey(Keys keyData) =>
        (keyData & Keys.KeyCode) is Keys.Left or Keys.Right or Keys.Home or Keys.End || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        switch (e.KeyCode)
        {
            case Keys.Left or Keys.Down: Value /= e.Shift ? 1.02f : 1.08f; break;
            case Keys.Right or Keys.Up: Value *= e.Shift ? 1.02f : 1.08f; break;
            case Keys.Home: Value = Minimum; break;
            case Keys.End: Value = Maximum; break;
            default: return;
        }
        e.Handled = true;
    }

    protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

    protected override void Draw(Graphics g)
    {
        var cy = Height / 2f;
        var h = S(4);
        var track = new RectangleF(Pad, cy - h / 2, Width - 2 * Pad, h);
        Theme.FillRoundRect(g, Theme.Track, track, h / 2);
        Theme.DrawRoundRect(g, Theme.CardBorder, track, h / 2);
        var x = XOf(_value);
        Theme.FillRoundRect(g, Theme.Accent, new RectangleF(track.X, track.Y, Math.Max(h, x - track.X), h), h / 2);
        var d = S(14);
        using var knob = new SolidBrush(Theme.AccentStrong);
        g.FillEllipse(knob, x - d / 2, cy - d / 2, d, d);
        if (Focused && ShowFocusCues)
        {
            using var pen = new Pen(Theme.Text, 1f);
            g.DrawEllipse(pen, x - d / 2 - 2, cy - d / 2 - 2, d + 4, d + 4);
        }
    }
}
