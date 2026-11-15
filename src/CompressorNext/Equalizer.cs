using System.Globalization;

namespace CompressorNext;

/// <summary>Filtertyp eines Equalizer-Bandes (Werte wie EqTypeCode in src/native/shared/SharedState.h).</summary>
public enum EqType : ushort
{
    Peak = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowCut = 3,
    HighCut = 4,
    Notch = 5,
}

/// <summary>Ein Band des parametrischen Equalizers.</summary>
public readonly record struct EqBand(EqType Type, float FreqHz, float GainDb, float Q, bool On = true)
{
    public const float FreqMin = 20f, FreqMax = 20000f, GainMax = 18f, QMin = 0.1f;

    public static readonly EqType[] Types = [EqType.Peak, EqType.LowShelf, EqType.HighShelf, EqType.LowCut, EqType.HighCut, EqType.Notch];

    /// <summary>Höchste Güte je Typ: Kuhschwänze schwingen darüber stark über, Hoch-/Tiefpässe bekommen eine Resonanzspitze.</summary>
    public static float QMax(EqType t) => t switch
    {
        EqType.LowShelf or EqType.HighShelf => 2f,
        EqType.LowCut or EqType.HighCut => 4f,
        _ => 10f,
    };

    public static bool HasGain(EqType t) => t is EqType.Peak or EqType.LowShelf or EqType.HighShelf;
    public bool HasGainValue => HasGain(Type);

    /// <summary>Güte, mit der ein neues Band dieses Typs beginnt.</summary>
    public static float DefaultQ(EqType t) => t is EqType.Peak ? 1f : t is EqType.Notch ? 4f : 0.71f;

    public EqBand Clamp() => this with
    {
        FreqHz = float.IsFinite(FreqHz) ? Math.Clamp(FreqHz, FreqMin, FreqMax) : 1000f,
        GainDb = float.IsFinite(GainDb) ? Math.Clamp(GainDb, -GainMax, GainMax) : 0f,
        Q = float.IsFinite(Q) ? Math.Clamp(Q, QMin, QMax(Type)) : DefaultQ(Type),
    };

    /// <summary>Wechselt den Typ; die Güte wird in den Bereich des neuen Typs geholt.</summary>
    public EqBand WithType(EqType type)
    {
        if (type == Type) return this;
        var q = Q;
        // Vom schmalen Glocke/Kerbe zum Kuhschwanz/Pass: auf die übliche Güte zurück, statt am Anschlag zu kleben.
        if (q > QMax(type)) q = DefaultQ(type);
        return (this with { Type = type, Q = q }).Clamp();
    }

    public static string TypeName(EqType t) => t switch
    {
        EqType.Peak => "Glocke",
        EqType.LowShelf => "Tiefen-Shelf",
        EqType.HighShelf => "Höhen-Shelf",
        EqType.LowCut => "Hochpass",
        EqType.HighCut => "Tiefpass",
        EqType.Notch => "Kerbe",
        _ => "?",
    };

    public static string TypeDescription(EqType t) => t switch
    {
        EqType.Peak => "Glocke – hebt oder senkt einen Bereich um die Frequenz",
        EqType.LowShelf => "Tiefen-Shelf – hebt oder senkt alles unterhalb",
        EqType.HighShelf => "Höhen-Shelf – hebt oder senkt alles oberhalb",
        EqType.LowCut => "Hochpass – schneidet Tiefen unterhalb ab (12 dB/Oktave)",
        EqType.HighCut => "Tiefpass – schneidet Höhen oberhalb ab (12 dB/Oktave)",
        EqType.Notch => "Kerbe – entfernt eine schmale Frequenz (z. B. Brummen)",
        _ => "?",
    };

    private static readonly CultureInfo De = CultureInfo.GetCultureInfo("de-DE");

    public static string FormatFreq(double f) =>
        f < 1000 ? f.ToString(f < 100 ? "0.#" : "0", De) + " Hz"
        : (f / 1000).ToString(f < 10000 ? "0.00" : "0.0", De) + " kHz";

    public static string FormatGain(double db) => Theme.Db((float)(Math.Round(db * 10) / 10), 1) + " dB";

    public static string FormatQ(double q) => q.ToString(q < 10 ? "0.00" : "0.0", De);

    /// <summary>Liest Eingaben wie "1,2k", "1200", "1.2 kHz", "80 Hz".</summary>
    public static bool TryParseFreq(string text, out float hz)
    {
        hz = 0;
        var s = text.Trim().ToLowerInvariant().Replace(" ", "").Replace(',', '.');
        var factor = 1.0;
        if (s.EndsWith("khz")) { factor = 1000; s = s[..^3]; }
        else if (s.EndsWith("hz")) { s = s[..^2]; }
        else if (s.EndsWith('k')) { factor = 1000; s = s[..^1]; }
        if (!double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) || !double.IsFinite(v)) return false;
        hz = (float)(v * factor);
        return true;
    }

    /// <summary>Liest Eingaben wie "+3", "-4,5", "−6 dB", "1.41".</summary>
    public static bool TryParseNumber(string text, out float value)
    {
        value = 0;
        var s = text.Trim().ToLowerInvariant().Replace(" ", "").Replace(',', '.').Replace('−', '-');
        if (s.EndsWith("db")) s = s[..^2];
        if (s.StartsWith("q")) s = s[1..];
        if (!double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) || !double.IsFinite(v)) return false;
        value = (float)v;
        return true;
    }

    /// <summary>Farbe eines Bandes (Nummer = Platz + 1).</summary>
    public static Color SlotColor(int slot) => SlotColors[slot % SlotColors.Length];

    private static readonly Color[] SlotColors =
    [
        Color.FromArgb(0x5A, 0xB0, 0xFF), Color.FromArgb(0xC7, 0x7D, 0xFF), Color.FromArgb(0x4F, 0xD1, 0xA5), Color.FromArgb(0xFF, 0x6F, 0xA8),
        Color.FromArgb(0xF2, 0xD2, 0x4B), Color.FromArgb(0x7F, 0xD3, 0x5A), Color.FromArgb(0xFF, 0x8A, 0x5C), Color.FromArgb(0x8F, 0xA8, 0xFF),
    ];
}

/// <summary>
/// Einstellungen des Equalizers. Bänder liegen auf festen Plätzen (wie in der Zustandsdatei): Ein Band behält
/// seine Nummer, solange es besteht.
/// </summary>
public sealed record EqSettings(bool Enabled, bool AutoLevel, EqBand?[] Slots)
{
    /// <summary>Plätze in der Zustandsdatei.</summary>
    public const int SlotCount = 8;

    /// <summary>So viele Bänder bietet die Oberfläche an.</summary>
    public const int MaxBands = 5;

    public static EqSettings Default => new(true, true, new EqBand?[SlotCount]);

    public int BandCount => Slots.Count(b => b != null);

    /// <summary>Verändert der Equalizer gerade den Klang?</summary>
    public bool IsActive => Enabled && Slots.Any(b => b is { On: true });

    public EqSettings WithBand(int slot, EqBand? band)
    {
        var copy = (EqBand?[])Slots.Clone();
        copy[slot] = band?.Clamp();
        return this with { Slots = copy };
    }

    public int FreeSlot()
    {
        if (BandCount >= MaxBands) return -1;
        for (var i = 0; i < SlotCount; i++)
            if (Slots[i] == null) return i;
        return -1;
    }

    public bool Equals(EqSettings? other) =>
        other is not null && Enabled == other.Enabled && AutoLevel == other.AutoLevel && Slots.SequenceEqual(other.Slots);

    public override int GetHashCode() => HashCode.Combine(Enabled, AutoLevel, BandCount);
}

/// <summary>
/// Frequenzgang der Filter – dieselben RBJ-Formeln wie im Audio-Modul (src/native/dsp/Equalizer.h), damit die
/// Kurve genau das zeigt, was man hört.
/// </summary>
internal static class EqMath
{
    public readonly record struct Coefs(double B0, double B1, double B2, double A1, double A2);

    public static Coefs Design(EqType type, double fs, double freqHz, double gainDb, double q)
    {
        var f = Math.Clamp(freqHz, 1.0, 0.49 * fs);
        var w0 = 2 * Math.PI * f / fs;
        double cw = Math.Cos(w0), sw = Math.Sin(w0);
        var alpha = sw / (2 * Math.Clamp(q, 0.01, 100));
        var a = Math.Pow(10, gainDb / 40);
        double b0, b1, b2, a0, a1, a2;
        switch (type)
        {
            case EqType.Peak:
                b0 = 1 + alpha * a; b1 = -2 * cw; b2 = 1 - alpha * a;
                a0 = 1 + alpha / a; a1 = -2 * cw; a2 = 1 - alpha / a;
                break;
            case EqType.LowShelf:
            {
                var sq = 2 * Math.Sqrt(a) * alpha;
                b0 = a * ((a + 1) - (a - 1) * cw + sq); b1 = 2 * a * ((a - 1) - (a + 1) * cw); b2 = a * ((a + 1) - (a - 1) * cw - sq);
                a0 = (a + 1) + (a - 1) * cw + sq; a1 = -2 * ((a - 1) + (a + 1) * cw); a2 = (a + 1) + (a - 1) * cw - sq;
                break;
            }
            case EqType.HighShelf:
            {
                var sq = 2 * Math.Sqrt(a) * alpha;
                b0 = a * ((a + 1) + (a - 1) * cw + sq); b1 = -2 * a * ((a - 1) + (a + 1) * cw); b2 = a * ((a + 1) + (a - 1) * cw - sq);
                a0 = (a + 1) - (a - 1) * cw + sq; a1 = 2 * ((a - 1) - (a + 1) * cw); a2 = (a + 1) - (a - 1) * cw - sq;
                break;
            }
            case EqType.LowCut:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case EqType.HighCut:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            default: // Kerbe
                b0 = 1; b1 = -2 * cw; b2 = 1;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
        }
        return new Coefs(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
    }

    public static double MagnitudeDb(in Coefs c, double fs, double freqHz)
    {
        var w = 2 * Math.PI * freqHz / fs;
        double c1 = Math.Cos(w), s1 = Math.Sin(w), c2 = Math.Cos(2 * w), s2 = Math.Sin(2 * w);
        double nr = c.B0 + c.B1 * c1 + c.B2 * c2, ni = -(c.B1 * s1 + c.B2 * s2);
        double dr = 1 + c.A1 * c1 + c.A2 * c2, di = -(c.A1 * s1 + c.A2 * s2);
        var p = (nr * nr + ni * ni) / Math.Max(dr * dr + di * di, 1e-300);
        return 10 * Math.Log10(Math.Max(p, 1e-24));
    }

    public static Coefs Design(EqBand b, double fs) => Design(b.Type, fs, b.FreqHz, b.HasGainValue ? b.GainDb : 0, b.Q);

    /// <summary>Frequenzgang eines Bandes an den gegebenen Frequenzen (dB).</summary>
    public static void Curve(EqBand b, double fs, ReadOnlySpan<double> freqs, Span<float> db)
    {
        var c = Design(b, fs);
        for (var i = 0; i < freqs.Length; i++) db[i] = (float)MagnitudeDb(c, fs, Math.Min(freqs[i], 0.4999 * fs));
    }

    /// <summary>
    /// Auto-Pegel: minus die stärkste Anhebung der Gesamtkurve (20 Hz … 20 kHz), damit der Equalizer hinter dem
    /// Maximierer nicht übersteuert.
    /// </summary>
    public static float AutoLevelDb(EqSettings eq, double fs = 48000)
    {
        if (!eq.Enabled || !eq.AutoLevel) return 0f;
        const int points = 240;
        Span<double> freqs = stackalloc double[points];
        Span<float> sum = stackalloc float[points];
        Span<float> one = stackalloc float[points];
        for (var i = 0; i < points; i++) freqs[i] = EqBand.FreqMin * Math.Pow(EqBand.FreqMax / EqBand.FreqMin, i / (points - 1.0));
        foreach (var b in eq.Slots)
        {
            if (b is not { On: true } band) continue;
            Curve(band, fs, freqs, one);
            for (var i = 0; i < points; i++) sum[i] += one[i];
        }
        var max = 0f;
        foreach (var v in sum) max = Math.Max(max, v);
        // Schmale Glocken können zwischen die Stützstellen fallen: auch genau an den Bandmitten nachsehen.
        foreach (var b in eq.Slots)
            if (b is { On: true } band && band.FreqHz < 0.49 * fs) max = Math.Max(max, (float)TotalDb(eq, fs, band.FreqHz));
        return -MathF.Round(max * 10) / 10;
    }

    public static double TotalDb(EqSettings eq, double fs, double freqHz)
    {
        var sum = 0.0;
        foreach (var b in eq.Slots)
            if (b is { On: true } band) sum += MagnitudeDb(Design(band, fs), fs, freqHz);
        return sum;
    }
}
