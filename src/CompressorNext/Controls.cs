using System.ComponentModel;

namespace CompressorNext;

/// <summary>Ein/Aus-Schalter.</summary>
internal sealed class ToggleSwitch : NextControl
{
    private bool _checked;

    public event EventHandler? CheckedChanged;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Checked
    {
        get => _checked;
        set
        {
            if (_checked == value) return;
            _checked = value;
            Invalidate();
            CheckedChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public ToggleSwitch()
    {
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        Cursor = Cursors.Hand;
        AccessibleRole = AccessibleRole.CheckButton;
    }

    /// <summary>Setzt den Zustand, ohne das Ereignis auszulösen.</summary>
    public void SetSilently(bool value)
    {
        if (_checked == value) return;
        _checked = value;
        Invalidate();
    }

    protected override void OnClick(EventArgs e)
    {
        base.OnClick(e);
        Focus();
        Checked = !Checked;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.KeyCode is Keys.Space or Keys.Enter)
        {
            Checked = !Checked;
            e.Handled = true;
        }
    }

    protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

    protected override void Draw(Graphics g)
    {
        var r = new RectangleF(1, 1, Width - 2, Height - 2);
        var radius = r.Height / 2;
        Theme.FillRoundRect(g, _checked ? Theme.AccentStrong : Theme.Track, r, radius);
        Theme.DrawRoundRect(g, _checked ? Theme.AccentStrong : Theme.Off, r, radius, S(1.2f));
        var d = r.Height - S(8);
        var x = _checked ? r.Right - d - S(4) : r.X + S(4);
        using var knob = new SolidBrush(_checked ? Color.White : Theme.TextDim);
        g.FillEllipse(knob, x, r.Y + S(4), d, d);
        if (Focused && ShowFocusCues) Theme.DrawRoundRect(g, Theme.Text, new RectangleF(0.5f, 0.5f, Width - 1, Height - 1), radius, 1f);
    }
}

/// <summary>Schieberegler für dB-Werte in ganzen Schritten.</summary>
internal sealed class DbSlider : NextControl
{
    private int _value;
    private bool _dragging;

    public event EventHandler? ValueChanged;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Minimum { get; set; } = -30;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Maximum { get; set; }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value
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

    public DbSlider()
    {
        SetStyle(ControlStyles.Selectable, true);
        TabStop = true;
        Cursor = Cursors.Hand;
        AccessibleRole = AccessibleRole.Slider;
    }

    public void SetSilently(int value)
    {
        _value = Math.Clamp(value, Minimum, Maximum);
        Invalidate();
    }

    private float Pad => S(10);
    private float XOf(int v) => Pad + (Width - 2 * Pad) * (v - Minimum) / (float)Math.Max(1, Maximum - Minimum);

    private int ValueAt(int x)
    {
        var f = (x - Pad) / Math.Max(1f, Width - 2 * Pad);
        return (int)Math.Round(Minimum + Math.Clamp(f, 0f, 1f) * (Maximum - Minimum));
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
        Value += Math.Sign(e.Delta);
    }

    protected override bool IsInputKey(Keys keyData) =>
        keyData is Keys.Left or Keys.Right or Keys.Up or Keys.Down or Keys.Home or Keys.End || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        switch (e.KeyCode)
        {
            case Keys.Left or Keys.Down: Value--; break;
            case Keys.Right or Keys.Up: Value++; break;
            case Keys.PageDown: Value -= 3; break;
            case Keys.PageUp: Value += 3; break;
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
        var h = S(6);
        var track = new RectangleF(Pad, cy - h / 2, Width - 2 * Pad, h);
        Theme.FillRoundRect(g, Theme.Track, track, h / 2);
        Theme.DrawRoundRect(g, Theme.CardBorder, track, h / 2);
        var x = XOf(_value);
        // Gefüllt wird vom Regler bis zum rechten Ende: Der Bereich über der Schwelle wird begrenzt.
        var color = Enabled ? Theme.Accent : Theme.Off;
        if (track.Right - x > 1) Theme.FillRoundRect(g, color, new RectangleF(x, track.Y, track.Right - x, h), h / 2);
        var d = S(18);
        using var knob = new SolidBrush(Enabled ? Theme.AccentStrong : Theme.Off);
        g.FillEllipse(knob, x - d / 2, cy - d / 2, d, d);
        if (Focused && ShowFocusCues)
        {
            using var pen = new Pen(Theme.Text, 1f);
            g.DrawEllipse(pen, x - d / 2 - 2, cy - d / 2 - 2, d + 4, d + 4);
        }
    }
}

/// <summary>Flache Schaltfläche; "Selected" hebt die aktive Voreinstellung hervor.</summary>
internal sealed class FlatButton : NextControl
{
    private bool _hover, _down, _selected;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Selected
    {
        get => _selected;
        set { if (_selected != value) { _selected = value; Invalidate(); } }
    }

    public FlatButton()
    {
        SetStyle(ControlStyles.Selectable | ControlStyles.StandardClick, true);
        TabStop = true;
        Cursor = Cursors.Hand;
        Font = Theme.Font(9f);
        AccessibleRole = AccessibleRole.PushButton;
    }

    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
    protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); _hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); _hover = _down = false; Invalidate(); }
    protected override void OnMouseDown(MouseEventArgs e) { base.OnMouseDown(e); Focus(); _down = true; Invalidate(); }
    protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e); _down = false; Invalidate(); }
    protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

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
        var r = new RectangleF(0.5f, 0.5f, Width - 1, Height - 1);
        var radius = S(6);
        var back = _selected ? Color.FromArgb(0x2B, 0x1A, 0x12) : _down ? Theme.CardBorder : _hover ? Color.FromArgb(0x24, 0x24, 0x28) : Theme.Card;
        Theme.FillRoundRect(g, back, r, radius);
        Theme.DrawRoundRect(g, _selected ? Theme.AccentStrong : Focused && ShowFocusCues ? Theme.Text : Theme.CardBorder, r, radius);
        TextRenderer.DrawText(g, Text, Font, Rectangle.Round(r), _selected ? Theme.Accent : Theme.Text,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
    }
}

/// <summary>Fläche mit abgerundetem Rahmen als Träger einer Gruppe.</summary>
internal sealed class CardPanel : Panel
{
    public CardPanel()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Card;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(Theme.Background);
        var r = new RectangleF(0.5f, 0.5f, Width - 1, Height - 1);
        var radius = 10f * DeviceDpi / 96f;
        Theme.FillRoundRect(g, Theme.Card, r, radius);
        Theme.DrawRoundRect(g, Theme.CardBorder, r, radius);
    }
}
