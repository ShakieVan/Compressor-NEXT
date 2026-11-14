using System.Diagnostics;

namespace CompressorNext;

/// <summary>Hauptfenster: Ein/Aus, Schwelle, Ausgang, Pegelanzeige, Ausgabegeräte.</summary>
internal sealed class MainForm : Form
{
    private readonly AppModel _model;

    private readonly Label _title = new() { Text = "Compressor NEXT", AutoSize = false };
    private readonly Label _status = new() { AutoSize = false, AutoEllipsis = true };
    private readonly ToggleSwitch _power = new() { AccessibleName = "Maximierer ein/aus" };

    private readonly CardPanel _cardMax = new();
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

    private readonly ToggleSwitch _autostart = new() { AccessibleName = "Mit Windows starten" };
    private readonly Label _autostartLabel = new() { Text = "Mit Windows starten", AutoSize = false };
    private readonly Label _version = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleRight };

    private readonly System.Windows.Forms.Timer _frame = new() { Interval = 33 };
    private readonly List<StreamInfo> _streams = [];
    private long _lastDeviceRefresh;
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
        foreach (var l in new[] { _thresholdLabel, _outputLabel, _devicesTitle }) { l.Font = Theme.Font(9.5f, FontStyle.Bold); l.ForeColor = Theme.Text; l.BackColor = Theme.Card; }
        foreach (var l in new[] { _thresholdValue, _outputValue }) { l.Font = Theme.Font(9.5f, FontStyle.Bold); l.ForeColor = Theme.Accent; l.BackColor = Theme.Card; }
        _thresholdHint.Font = Theme.Font(8.25f);
        _thresholdHint.ForeColor = Theme.TextFaint;
        _thresholdHint.BackColor = Theme.Card;
        _problem.Font = Theme.Font(8.25f);
        _problem.ForeColor = Theme.Error;
        _problem.BackColor = Theme.Card;
        _autostartLabel.ForeColor = Theme.TextDim;
        _version.ForeColor = Theme.TextFaint;
        _version.Text = "Version " + (typeof(MainForm).Assembly.GetName().Version?.ToString(3) ?? "");
        _power.BackColor = Theme.Background;
        _autostart.BackColor = Theme.Background;

        _cardMax.Controls.AddRange([_thresholdLabel, _thresholdValue, _threshold, _thresholdHint, _outputLabel, _outputValue, _output, .. _presets]);
        _cardMeter.Controls.Add(_meter);
        _cardDevices.Controls.AddRange([_devicesTitle, _devices, _problem, _repair]);
        Controls.AddRange([_title, _status, _power, _cardMax, _cardMeter, _cardDevices, _autostart, _autostartLabel, _version]);

        _power.CheckedChanged += (_, _) => _model.Update(_model.Settings with { Enabled = _power.Checked });
        _threshold.ValueChanged += (_, _) => _model.Update(_model.Settings with { ThresholdDb = _threshold.Value });
        _output.ValueChanged += (_, _) => _model.Update(_model.Settings with { OutputDb = _output.Value });
        foreach (var b in _presets) b.Click += (s, _) => _model.Update(_model.Settings with { Enabled = true, ThresholdDb = (int)((FlatButton)s!).Tag! });
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

    private void LayoutControls()
    {
        const int width = 480, pad = 16, inner = 16;
        var cardW = width - 2 * pad;
        var y = 14;
        _title.SetBounds(S(pad), S(y), S(300), S(30));
        _power.SetBounds(S(width - pad - 58), S(y + 1), S(58), S(28));
        y += 34;
        _status.SetBounds(S(pad), S(y), S(cardW), S(20));
        y += 30;

        // Maximierer
        var cy = 14;
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
        _cardMax.SetBounds(S(pad), S(y), S(cardW), S(cy));
        y += cy + 12;

        // Pegel
        _meter.SetBounds(S(inner), S(14), S(cardW - 2 * inner), S(LevelMeterControl.LogicalHeight));
        _cardMeter.SetBounds(S(pad), S(y), S(cardW), S(LevelMeterControl.LogicalHeight + 26));
        y += LevelMeterControl.LogicalHeight + 26 + 12;

        // Geräte
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
        _cardDevices.SetBounds(S(pad), S(y), S(cardW), S(dy + 6));
        y += dy + 6 + 12;

        _autostart.SetBounds(S(pad), S(y + 1), S(38), S(20));
        _autostartLabel.SetBounds(S(pad + 46), S(y), S(200), S(22));
        _version.SetBounds(S(width - pad - 160), S(y), S(160), S(22));
        y += 22 + 14;

        ClientSize = new Size(S(width), S(y));
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

    /// <summary>Zeigt das Fenster unten rechts über dem Infobereich.</summary>
    public void ShowNearTray()
    {
        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        if (!Visible) Location = new Point(area.Right - Width - S(12), area.Bottom - Height - S(12));
        Show();
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        Activate();
    }

    private void ShowSettings()
    {
        var s = _model.Settings;
        _power.SetSilently(s.Enabled);
        _threshold.SetSilently((int)MathF.Round(s.ThresholdDb));
        _output.SetSilently((int)MathF.Round(s.OutputDb));
        _threshold.Enabled = _output.Enabled = s.Enabled;
        _thresholdValue.Text = Theme.Db(MathF.Round(s.ThresholdDb)) + " dB";
        _outputValue.Text = Theme.Db(MathF.Round(s.OutputDb)) + " dB";
        _thresholdValue.ForeColor = _outputValue.ForeColor = s.Enabled ? Theme.Accent : Theme.TextFaint;
        foreach (var b in _presets) b.Selected = s.Enabled && (int)b.Tag! == (int)MathF.Round(s.ThresholdDb);
        _status.Text = s.Enabled
            ? $"Ein – leise Stellen {Theme.Db(MathF.Round(s.BoostDb))} dB, Spitzen höchstens {Theme.Db(MathF.Round(s.OutputDb))} dB"
            : "Aus – der Ton läuft unverändert durch";
        _meter.SetMaximizer(s.Enabled, s.ThresholdDb, s.OutputDb);
    }

    private void Tick()
    {
        _model.PollExternalChanges();
        _streams.Clear();
        var fresh = _model.State.ReadMeters(out var reading, _streams);
        _meter.Advance(fresh ? reading : null, _streams.Count > 0);
        if (_clock.ElapsedMilliseconds - _lastDeviceRefresh > 1000) RefreshDevices(force: false);
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
            _model.Changed -= ShowSettings;
        }
        base.Dispose(disposing);
    }
}
