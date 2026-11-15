namespace CompressorNext;

/// <summary>
/// Spektrum des Ausgangs: Hann-Fenster und FFT über die letzten ~170 ms (8192 Werte bei 44,1/48 kHz,
/// 16384 darüber), Ergebnis in dBFS je Frequenzlinie. Ein Vollaussteuerungs-Sinus ergibt 0 dB.
/// </summary>
internal sealed class SpectrumAnalyzer
{
    private int _n;
    private float[] _window = [];
    private float[] _re = [], _im = [];
    private float[] _cos = [], _sin = [];
    private int[] _rev = [];
    private float _norm;

    /// <summary>Hier hinein kopiert man die neuesten Abtastwerte (Länge = <see cref="Size"/>).</summary>
    public float[] Input { get; private set; } = [];

    /// <summary>Pegel je Frequenzlinie in dB (Länge Size/2 + 1), nach <see cref="Compute"/>.</summary>
    public float[] Db { get; private set; } = [];

    public int Size => _n;
    public int SampleRate { get; private set; }

    public void Configure(int sampleRate)
    {
        if (sampleRate == SampleRate && _n > 0) return;
        SampleRate = sampleRate;
        var n = sampleRate <= 50000 ? 8192 : 16384;
        if (n == _n) return;
        _n = n;
        Input = new float[n];
        Db = new float[n / 2 + 1];
        _re = new float[n];
        _im = new float[n];
        _window = new float[n];
        double sum = 0;
        for (var i = 0; i < n; i++)
        {
            _window[i] = (float)(0.5 - 0.5 * Math.Cos(2 * Math.PI * i / n));
            sum += _window[i];
        }
        _norm = (float)(2.0 / sum); // Amplitude eines Sinus
        _cos = new float[n / 2];
        _sin = new float[n / 2];
        for (var i = 0; i < n / 2; i++)
        {
            _cos[i] = (float)Math.Cos(2 * Math.PI * i / n);
            _sin[i] = (float)-Math.Sin(2 * Math.PI * i / n);
        }
        _rev = new int[n];
        var bits = (int)Math.Log2(n);
        for (var i = 0; i < n; i++)
        {
            var r = 0;
            for (var b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
            _rev[i] = r;
        }
    }

    public void Compute()
    {
        var n = _n;
        for (var i = 0; i < n; i++)
        {
            _re[_rev[i]] = Input[i] * _window[i];
            _im[_rev[i]] = 0f;
        }
        for (var size = 2; size <= n; size <<= 1)
        {
            var half = size >> 1;
            var step = n / size;
            for (var start = 0; start < n; start += size)
            {
                for (int k = 0, t = 0; k < half; k++, t += step)
                {
                    float wr = _cos[t], wi = _sin[t];
                    int a = start + k, b = a + half;
                    var xr = _re[b] * wr - _im[b] * wi;
                    var xi = _re[b] * wi + _im[b] * wr;
                    _re[b] = _re[a] - xr;
                    _im[b] = _im[a] - xi;
                    _re[a] += xr;
                    _im[a] += xi;
                }
            }
        }
        for (var k = 0; k <= n / 2; k++)
        {
            var p = (_re[k] * _re[k] + _im[k] * _im[k]) * _norm * _norm;
            Db[k] = p > 1e-14f ? 10f * MathF.Log10(p) : -140f;
        }
    }

    /// <summary>Höchster Pegel im Frequenzbereich [f0, f1]; liegt keine Linie darin, wird zwischen den Nachbarn interpoliert.</summary>
    public float LevelBetween(double f0, double f1)
    {
        var binHz = (double)SampleRate / _n;
        var k0 = f0 / binHz;
        var k1 = f1 / binHz;
        var last = _n / 2;
        var lo = (int)Math.Ceiling(k0);
        var hi = (int)Math.Floor(k1);
        if (hi >= lo && lo >= 0)
        {
            var max = -140f;
            for (var k = lo; k <= Math.Min(hi, last); k++) max = Math.Max(max, Db[k]);
            return max;
        }
        var kc = Math.Clamp((k0 + k1) / 2, 0, last);
        var i = Math.Min((int)kc, last - 1);
        var frac = (float)(kc - i);
        return Db[i] + (Db[i + 1] - Db[i]) * frac;
    }
}
