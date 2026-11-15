using System.Diagnostics;

namespace CompressorNext;

/// <summary>
/// Hauptfenster: oben Hauptschalter und Zustand, darunter zwei Reiter – "Kompressor" (Schwelle, Ausgang,
/// Pegelanzeige, Ausgabegeräte) und "Equalizer" (Grafik mit Spektrum, Bandliste).
/// </summary>
internal sealed class MainForm : Form
{
    private const int TabCompressor = 0, TabEqualizer = 1;

    private readonly AppModel _model;

    private readonly Label _title = new() { Text = "Compressor NEXT", AutoSize = false };
    private readonly Label _status = new() { AutoSize = false, AutoEllipsis = true };
    private readonly ToggleSwitch _power = new() { AccessibleName = "Compressor NEXT ein/aus" };
    private readonly TabStrip _tabs = new() { AccessibleName = "Bereich" };
    private readonly Panel _pageComp = new() { BackColor = Theme.Background };
    private readonly Panel _pageEq = new() { BackColor = Theme.Background, Visible = false };

    // Reiter "Kompressor"
    private readonly CardPanel _cardMax = new();
    private readonly Label _compTitle = new() { Text = "Kompressor", AutoSize = false };
    private readonly ToggleSwitch _compToggle = new() { AccessibleName = "Kompressor ein/aus" };
    private readonly Label _thresholdLabel = new() { Text = "Schwelle", AutoSize = false };
    private readonly Label _thresholdValue = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleRight };
    private readonly DbSlider _threshold = new() { Minimum = (int)MaximizerSettings.ThresholdMin, Maximum = 0, AccessibleName = "Schwelle in dB" };
    private readonly Label _thresholdHint = new() { Text = "Je tiefer, desto lauter werden leise Stellen und desto gleichmäßiger wird die Lautstärke.", AutoSize = false };
    private readonly Label _outputLabel = new() { Text = "Ausgang", AutoSize = false };
    private readonly Label _outputValue = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleRight };
    private readonly DbSlider _output = new() { Minimum = (int)MaximizerSettings.OutputMin, Maximum = 0, AccessibleName = "Ausgangspegel in dB" };
    private readonly FlatButton[] _presets;

    private readonly CardPanel _cardMeter = new();
    private readonly LevelMeterControl _meter = new();

    private readonly CardPanel _cardDevices = new();
    private readonly Label _devicesTitle = new() { Text = "Ausgabegeräte", AutoSize = false };
    private readonly DeviceListControl _devices = new();
    private readonly Label _problem = new() { AutoSize = false, Visible = false };
    private readonly FlatButton _repair = new() { Text = "Beheben", Visible = false };

    // Reiter "Equalizer"
    private readonly CardPanel _cardEq = new();
    private readonly Label _eqTitle = new() { Text = "Equalizer", AutoSize = false };
    private readonly ToggleSwitch _eqToggle = new() { AccessibleName = "Equalizer ein/aus" };
    private readonly ToggleSwitch _autoLevel = new() { AccessibleName = "Auto-Pegel" };
    private readonly Label _autoLevelLabel = new() { Text = "Auto-Pegel", AutoSize = false };
    private readonly EqGraphControl _graph = new();
    private readonly EqBandList _bands = new();
    private readonly ToolTip _tips = new();
    private readonly SpectrumAnalyzer _analyzer = new();
    private uint _lastSpectrumPos;

    private readonly ToggleSwitch _autostart = new() { AccessibleName = "Mit Windows starten" };
    private readonly Label _autostartLabel = new() { Text = "Mit Windows starten", AutoSize = false };
    private readonly Label _version = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleRight };

    private readonly System.Windows.Forms.Timer _frame = new() { Interval = 33 };
    private readonly List<StreamInfo> _streams = [];
    private long _lastDeviceRefresh, _lastFrame;
    private bool _hasProblem;
    private readonly Stopwatch _clock = Stopwatch.StartNew();

    public MainForm(AppModel model)
    {
        _model = model;
        Text = "Compressor NEXT";
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
        BackColor = Theme.Background;
        ForeColor = Theme.Text;
        Font = Theme.Font(9f);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.Manual;
        AutoScaleMode = AutoScaleMode.None;
        KeyPreview = true;

        _presets =
        [
            new FlatButton { Text = "Leicht  −9", Tag = -9 },
            new FlatButton { Text = "Film  −18", Tag = -18 },
            new FlatButton { Text = "Nacht  −27", Tag = -27 },
        ];

        _title.Font = Theme.Font(14f, FontStyle.Bold);
        _title.ForeColor = Theme.Text;
        _status.ForeColor = Theme.TextDim;
        foreach (var l in new[] { _compTitle, _eqTitle }) { l.Font = Theme.Font(10.5f, FontStyle.Bold); l.ForeColor = Theme.Text; l.BackColor = Theme.Card; }
        foreach (var l in new[] { _thresholdLabel, _outputLabel, _devicesTitle }) { l.Font = Theme.Font(9.5f, FontStyle.Bold); l.ForeColor = Theme.Text; l.BackColor = Theme.Card; }
        foreach (var l in new[] { _thresholdValue, _outputValue }) { l.Font = Theme.Font(9.5f, FontStyle.Bold); l.ForeColor = Theme.Accent; l.BackColor = Theme.Card; }
        _thresholdHint.Font = Theme.Font(8.25f);
        _thresholdHint.ForeColor = Theme.TextFaint;
        _thresholdHint.BackColor = Theme.Card;
        _problem.Font = Theme.Font(8.25f);
        _problem.ForeColor = Theme.Error;
        _problem.BackColor = Theme.Card;
        _autoLevelLabel.Font = Theme.Font(8.75f);
        _autoLevelLabel.ForeColor = Theme.TextDim;
        _autoLevelLabel.BackColor = Theme.Card;
        _autoLevelLabel.TextAlign = ContentAlignment.MiddleLeft;
        _autostartLabel.ForeColor = Theme.TextDim;
        _version.ForeColor = Theme.TextFaint;
        _version.Text = "Version " + (typeof(MainForm).Assembly.GetName().Version?.ToString(3) ?? "");
        _power.BackColor = Theme.Background;
        _autostart.BackColor = Theme.Background;
        _compToggle.BackColor = _eqToggle.BackColor = _autoLevel.BackColor = Theme.Card;
        _tabs.SetItems("Kompressor", "Equalizer");

        _cardMax.Controls.AddRange([_compTitle, _compToggle, _thresholdLabel, _thresholdValue, _threshold, _thresholdHint, _outputLabel, _outputValue, _output, .. _presets]);
        _cardMeter.Controls.Add(_meter);
        _cardDevices.Controls.AddRange([_devicesTitle, _devices, _problem, _repair]);
        _pageComp.Controls.AddRange([_cardMax, _cardMeter, _cardDevices]);
        _cardEq.Controls.AddRange([_eqTitle, _autoLevel, _autoLevelLabel, _eqToggle, _graph, _bands]);
        _pageEq.Controls.Add(_cardEq);
        Controls.AddRange([_title, _status, _power, _tabs, _pageComp, _pageEq, _autostart, _autostartLabel, _version]);

        _tips.SetToolTip(_autoLevel, "Senkt den Pegel hinter dem Equalizer um seine stärkste Anhebung, damit nichts übersteuert.");
        _tips.SetToolTip(_autoLevelLabel, "Senkt den Pegel hinter dem Equalizer um seine stärkste Anhebung, damit nichts übersteuert.");
        _tips.SetToolTip(_power, "Schaltet Kompressor und Equalizer zusammen ein oder aus.");

        _power.CheckedChanged += (_, _) => _model.Update(_model.Settings with { Enabled = _power.Checked });
        _compToggle.CheckedChanged += (_, _) => _model.Update(_model.Settings with { CompressorOn = _compToggle.Checked, Enabled = _model.Settings.Enabled || _compToggle.Checked });
        _threshold.ValueChanged += (_, _) => _model.Update(_model.Settings with { ThresholdDb = _threshold.Value });
        _output.ValueChanged += (_, _) => _model.Update(_model.Settings with { OutputDb = _output.Value });
        foreach (var b in _presets)
            b.Click += (s, _) => _model.Update(_model.Settings with { Enabled = true, CompressorOn = true, ThresholdDb = (int)((FlatButton)s!).Tag! });
        _eqToggle.CheckedChanged += (_, _) => EditEq(_model.Eq with { Enabled = _eqToggle.Checked });
        _autoLevel.CheckedChanged += (_, _) => EditEq(_model.Eq with { AutoLevel = _autoLevel.Checked });
        _autoLevelLabel.Click += (_, _) => _autoLevel.Checked = !_autoLevel.Checked;
        _graph.Edited += EditEq;
        _graph.SelectionChanged += slot => _bands.SetSelected(slot);
        _bands.Edited += EditEq;
        _bands.SelectionChanged += slot => _graph.SelectedSlot = slot;
        _tabs.SelectedIndexChanged += (_, _) => ShowTab();
        _devices.UseChanged += (device, use) => { _model.SetExcluded(device.Hash, !use); RefreshDevices(force: true); };
        _autostart.CheckedChanged += (_, _) => Autostart.Set(_autostart.Checked);
        _autostartLabel.Click += (_, _) => _autostart.Checked = !_autostart.Checked;
        _repair.Click += (_, _) => Repair();
        _frame.Tick += (_, _) => Tick();
        _model.Changed += ShowSettings;

        _autostart.SetSilently(Autostart.IsEnabled);
        ShowSettings();
        RefreshDevices(force: true);
        LayoutControls();
    }

    private int S(float v) => (int)Math.Round(v * DeviceDpi / 96f);

    /// <summary>Wechselt den Reiter (0 = Kompressor, 1 = Equalizer).</summary>
    public void SelectTab(int index) => _tabs.SelectedIndex = index;

    private void ShowTab()
    {
        var eq = _tabs.SelectedIndex == TabEqualizer;
        _pageEq.Visible = eq;
        _pageComp.Visible = !eq;
    }

    private void LayoutControls()
    {
        const int width = 560, pad = 16, inner = 16;
        var cardW = width - 2 * pad;
        var y = 14;
        _title.SetBounds(S(pad), S(y), S(300), S(30));
        _power.SetBounds(S(width - pad - 58), S(y + 1), S(58), S(28));
        y += 34;
        _status.SetBounds(S(pad), S(y), S(cardW), S(20));
        y += 28;
        _tabs.SetBounds(S(pad), S(y), S(cardW), S(36));
        y += 36 + 10;

        // --- Reiter "Kompressor" ---
        var cy = 12;
        _compTitle.SetBounds(S(inner), S(cy), S(200), S(24));
        _compToggle.SetBounds(S(cardW - inner - 38), S(cy + 2), S(38), S(20));
        cy += 34;
        _thresholdLabel.SetBounds(S(inner), S(cy), S(200), S(20));
        _thresholdValue.SetBounds(S(cardW - inner - 120), S(cy), S(120), S(20));
        cy += 22;
        _threshold.SetBounds(S(inner - 10), S(cy), S(cardW - 2 * inner + 20), S(28));
        cy += 30;
        _thresholdHint.SetBounds(S(inner), S(cy), S(cardW - 2 * inner), S(32));
        cy += 40;
        _outputLabel.SetBounds(S(inner), S(cy), S(200), S(20));
        _outputValue.SetBounds(S(cardW - inner - 120), S(cy), S(120), S(20));
        cy += 22;
        _output.SetBounds(S(inner - 10), S(cy), S(cardW - 2 * inner + 20), S(28));
        cy += 38;
        const int gap = 8;
        var bw = (cardW - 2 * inner - 2 * gap) / 3;
        for (var i = 0; i < _presets.Length; i++) _presets[i].SetBounds(S(inner + i * (bw + gap)), S(cy), S(bw), S(30));
        cy += 30 + 14;
        _cardMax.SetBounds(0, 0, S(cardW), S(cy));
        var py = cy + 12;

        _meter.SetBounds(S(inner), S(14), S(cardW - 2 * inner), S(LevelMeterControl.LogicalHeight));
        _cardMeter.SetBounds(0, S(py), S(cardW), S(LevelMeterControl.LogicalHeight + 26));
        py += LevelMeterControl.LogicalHeight + 26 + 12;

        var dy = 12;
        _devicesTitle.SetBounds(S(inner), S(dy), S(cardW - 2 * inner), S(20));
        dy += 24;
        var listH = Math.Min(_devices.LogicalHeight, 5 * DeviceListControl.RowHeight);
        _devices.SetBounds(S(inner), S(dy), S(cardW - 2 * inner), S(listH));
        dy += listH + 6;
        if (_hasProblem)
        {
            _problem.SetBounds(S(inner), S(dy), S(cardW - 2 * inner - 100), S(34));
            _repair.SetBounds(S(cardW - inner - 92), S(dy + 2), S(92), S(28));
            dy += 40;
        }
        _cardDevices.SetBounds(0, S(py), S(cardW), S(dy + 6));
        py += dy + 6;
        var pageH = py;

        // --- Reiter "Equalizer": gleiche Höhe; die Grafik nimmt, was die Bandliste übrig lässt ---
        var ey = 12;
        _eqTitle.SetBounds(S(inner), S(ey), S(110), S(24));
        _autoLevel.SetBounds(S(inner + 120), S(ey + 2), S(38), S(20));
        _autoLevelLabel.SetBounds(S(inner + 164), S(ey), S(200), S(24));
        _eqToggle.SetBounds(S(cardW - inner - 38), S(ey + 2), S(38), S(20));
        ey += 32;
        var graphH = pageH - ey - 8 - EqBandList.LogicalHeight - 10;
        _graph.SetBounds(S(inner - 8), S(ey), S(cardW - 2 * inner + 12), S(graphH));
        ey += graphH + 8;
        _bands.SetBounds(S(inner), S(ey), S(cardW - 2 * inner), S(EqBandList.LogicalHeight));
        _cardEq.SetBounds(0, 0, S(cardW), S(pageH));

        _pageComp.SetBounds(S(pad), S(y), S(cardW), S(pageH));
        _pageEq.SetBounds(S(pad), S(y), S(cardW), S(pageH));
        y += pageH + 12;

        _autostart.SetBounds(S(pad), S(y + 1), S(38), S(20));
        _autostartLabel.SetBounds(S(pad + 46), S(y), S(200), S(22));
        _version.SetBounds(S(width - pad - 160), S(y), S(160), S(22));
        y += 22 + 14;

        var oldBottom = Bottom;
        ClientSize = new Size(S(width), S(y));
        // Unten am Infobereich verankert: wächst das Fenster (neues Gerät), wächst es nach oben.
        if (Visible && oldBottom != Bottom) Top -= Bottom - oldBottom;
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        Theme.UseDarkTitleBar(Handle);
    }

    protected override void OnDpiChanged(DpiChangedEventArgs e)
    {
        base.OnDpiChanged(e);
        LayoutControls();
    }

    protected override void OnVisibleChanged(EventArgs e)
    {
        base.OnVisibleChanged(e);
        if (Visible)
        {
            RefreshDevices(force: true);
            _frame.Start();
        }
        else
        {
            _frame.Stop();
        }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.KeyCode == Keys.Escape) Hide();
        else if (e.Control && e.KeyCode == Keys.Tab) { _tabs.SelectedIndex = 1 - _tabs.SelectedIndex; e.Handled = true; }
    }

    /// <summary>Schließen blendet nur aus; beendet wird über das Tray-Menü.</summary>
    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (e.CloseReason == CloseReason.UserClosing && !_model.Exiting)
        {
            e.Cancel = true;
            Hide();
        }
        base.OnFormClosing(e);
    }

    /// <summary>
    /// Für Bildschirmfotos und Bedientests: Fenster außerhalb des Bildschirms, ohne den Fokus zu nehmen
    /// (sonst landen Tastendrücke des Nutzers darin).
    /// </summary>
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public bool Passive { get; init; }

    protected override bool ShowWithoutActivation => Passive;

    /// <summary>Zeigt das Fenster unten rechts über dem Infobereich.</summary>
    public void ShowNearTray()
    {
        if (Passive)
        {
            Location = new Point(SystemInformation.VirtualScreen.Left - Width - 200, SystemInformation.VirtualScreen.Top);
            Show();
            return;
        }
        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        if (!Visible) Location = new Point(area.Right - Width - S(12), Math.Max(area.Top, area.Bottom - Height - S(12)));
        Show();
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        Activate();
    }

    private void EditEq(EqSettings eq)
    {
        // Das erste Band bei ausgeschaltetem Equalizer: einschalten, sonst hört man nichts davon.
        if (!eq.Enabled && eq.BandCount > _model.Eq.BandCount && _model.Eq.BandCount == 0) eq = eq with { Enabled = true };
        // Wer den Equalizer einschaltet, will ihn hören: dann auch den Hauptschalter.
        if (eq.Enabled && !_model.Eq.Enabled && !_model.Settings.Enabled) _model.Update(_model.Settings with { Enabled = true });
        _model.UpdateEq(eq);
    }

    private void ShowSettings()
    {
        var s = _model.Settings;
        var eq = _model.Eq;
        var comp = s.CompressorActive;
        _power.SetSilently(s.Enabled);
        _compToggle.SetSilently(s.CompressorOn);
        _threshold.SetSilently((int)MathF.Round(s.ThresholdDb));
        _output.SetSilently((int)MathF.Round(s.OutputDb));
        _threshold.Enabled = _output.Enabled = comp;
        _thresholdValue.Text = Theme.Db(MathF.Round(s.ThresholdDb)) + " dB";
        _outputValue.Text = Theme.Db(MathF.Round(s.OutputDb)) + " dB";
        _thresholdValue.ForeColor = _outputValue.ForeColor = comp ? Theme.Accent : Theme.TextFaint;
        foreach (var b in _presets) b.Selected = comp && (int)b.Tag! == (int)MathF.Round(s.ThresholdDb);
        _meter.SetMaximizer(comp, s.ThresholdDb, s.OutputDb);

        var eqActive = s.Enabled && eq.IsActive;
        _eqToggle.SetSilently(eq.Enabled);
        _autoLevel.SetSilently(eq.AutoLevel);
        _autoLevelLabel.Text = eq.AutoLevel && eq.Enabled && _model.EqLevelDb < 0
            ? $"Auto-Pegel  {Theme.Db(_model.EqLevelDb, 1)} dB"
            : "Auto-Pegel";
        _graph.SetSettings(eq, s.Enabled && eq.Enabled);
        _bands.SetSettings(eq, _graph.SelectedSlot, s.Enabled && eq.Enabled);
        _tabs.SetState(TabCompressor, comp, _hasProblem);
        _tabs.SetState(TabEqualizer, eqActive, false);

        if (!s.Enabled)
        {
            _status.Text = "Aus – der Ton läuft unverändert durch";
            return;
        }
        var parts = new List<string>();
        parts.Add(comp
            ? $"leise Stellen {Theme.Db(MathF.Round(s.BoostDb))} dB, Spitzen höchstens {Theme.Db(MathF.Round(s.OutputDb))} dB"
            : "Kompressor aus");
        if (eqActive)
        {
            var n = eq.Slots.Count(b => b is { On: true });
            parts.Add(n == 1 ? "EQ mit 1 Band" : $"EQ mit {n} Bändern");
        }
        _status.Text = (comp || eqActive ? "Ein – " : "Ein – nichts aktiv: ") + string.Join(" · ", parts);
    }

    private void Tick()
    {
        var now = _clock.ElapsedMilliseconds;
        var dt = _lastFrame == 0 ? 0f : Math.Clamp((now - _lastFrame) / 1000f, 0f, 0.25f);
        _lastFrame = now;
        _model.PollExternalChanges();
        _streams.Clear();
        var fresh = _model.State.ReadMeters(out var reading, _streams);
        _meter.Advance(fresh ? reading : null, _streams.Count > 0);
        if (_tabs.SelectedIndex == TabEqualizer) UpdateSpectrum(fresh ? reading : null, dt);
        if (now - _lastDeviceRefresh > 1000) RefreshDevices(force: false);
    }

    private void UpdateSpectrum(MeterReading? reading, float dt)
    {
        var state = _model.State;
        state.RequestSpectrum();
        if (state.TryGetSpectrum(out var rate, out var pos))
        {
            _analyzer.Configure(rate);
            if (pos != _lastSpectrumPos)
            {
                state.CopySpectrum(pos, _analyzer.Input);
                _analyzer.Compute();
                _lastSpectrumPos = pos;
            }
            _graph.SetSampleRate(rate);
            _graph.PushSpectrum(_analyzer, dt);
        }
        else
        {
            if (_streams.Count > 0) _graph.SetSampleRate(_streams[0].SampleRate);
            _graph.PushSpectrum(null, dt);
        }
        _graph.SetOutput(reading?.OutL, reading?.OutR, dt);
    }

    private void RefreshDevices(bool force)
    {
        _lastDeviceRefresh = _clock.ElapsedMilliseconds;
        if (force)
        {
            _streams.Clear();
            _model.State.ReadMeters(out _, _streams);
        }
        var excluded = _model.Excluded;
        var rows = AudioDevices.GetActive()
            .Select(d => new DeviceRow(d, excluded.Contains(d.Hash), _streams.Any(s => s.EndpointHash == d.Hash && s.Engaged)))
            .ToList();
        var countBefore = _devices.Rows.Count;
        _devices.SetRows(rows);

        var unbound = rows.Count(r => !r.Device.Bound);
        var off = rows.Count(r => r.Device.Bound && r.Device.EnhancementsOff);
        var problem = unbound > 0
            ? (unbound == 1 ? "Ein Gerät ist noch nicht eingebunden." : $"{unbound} Geräte sind noch nicht eingebunden.") + " „Beheben“ richtet das ein (Adminrechte, der Ton setzt kurz aus)."
            : off > 0 ? "Bei einem Gerät sind die Windows-Audioverbesserungen ausgeschaltet; dort kann Compressor NEXT nicht arbeiten. „Beheben“ schaltet sie ein."
            : null;
        // Eigenes Merkfeld: Control.Visible meldet "false", solange das Fenster selbst noch nicht sichtbar ist.
        var hadProblem = _hasProblem;
        _hasProblem = problem != null;
        _problem.Text = problem ?? "";
        _problem.Visible = _repair.Visible = _hasProblem;
        _tabs.SetState(TabCompressor, _model.Settings.CompressorActive, _hasProblem);
        if (countBefore != rows.Count || hadProblem != _hasProblem) LayoutControls();
    }

    private void Repair()
    {
        var helper = Path.Combine(AppContext.BaseDirectory, "CompressorNextSvc.exe");
        if (!File.Exists(helper))
        {
            MessageBox.Show(this, "Das Hilfsprogramm CompressorNextSvc.exe fehlt. Bitte Compressor NEXT neu installieren.", "Compressor NEXT", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        try
        {
            using var p = Process.Start(new ProcessStartInfo(helper, "repair") { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden });
            p?.WaitForExit(30000);
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // UAC-Abfrage abgelehnt
        }
        RefreshDevices(force: true);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _frame.Dispose();
            _tips.Dispose();
            _model.Changed -= ShowSettings;
        }
        base.Dispose(disposing);
    }
}
