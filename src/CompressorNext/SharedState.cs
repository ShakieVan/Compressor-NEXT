using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;

namespace CompressorNext;

/// <summary>Einstellungen des Maximierers.</summary>
public readonly record struct MaximizerSettings(bool Enabled, float ThresholdDb, float OutputDb)
{
    public const float ThresholdMin = -30f;
    public const float OutputMin = -12f;
    public static readonly MaximizerSettings Default = new(true, -18f, -1f);

    public MaximizerSettings Clamp() => new(Enabled, Math.Clamp(ThresholdDb, ThresholdMin, 0f), Math.Clamp(OutputDb, OutputMin, 0f));

    /// <summary>Anhebung leiser Stellen in dB (Ausgang − Schwelle).</summary>
    public float BoostDb => OutputDb - ThresholdDb;
}

/// <summary>Werte der Pegelanzeige eines Zeitabschnitts (linear).</summary>
public struct MeterReading
{
    public float InL, InR, OutL, OutR, MinGain;
    public bool Engaged;
}

/// <summary>Ein laufender Audiostrom, wie ihn das Audio-Modul meldet.</summary>
public readonly record struct StreamInfo(uint EndpointHash, uint SampleRate, uint Channels, bool Engaged);

/// <summary>
/// Gemeinsamer Zustand zwischen Oberfläche und Audio-Modul: eine kleine Datei, die beide Seiten in den
/// Speicher einblenden. Aufbau identisch in src/native/shared/SharedState.h.
/// </summary>
public sealed unsafe class SharedState : IDisposable
{
    public const uint Magic = 0x31584E43; // "CNX1"
    public const uint Version = 1;
    public const int FileSize = 65536; // der hintere Teil ist das Spurprotokoll des Audio-Moduls
    public const int MaxExcluded = 32;
    public const int MeterSlots = 16;

    // Offsets (siehe SharedState.h)
    private const int OffMagic = 0, OffVersion = 4, OffSeq = 8, OffEnabled = 12, OffThreshold = 16, OffOutput = 20, OffExcludedCount = 24;
    private const int OffExcluded = 64;
    private const int OffMeters = 256;
    private const int SlotSize = 64;
    private const int SlotTick = 0, SlotOwner = 8, SlotSeq = 12, SlotEndpoint = 16, SlotRate = 20, SlotChannels = 24, SlotFlags = 28,
        SlotInL = 32, SlotInR = 36, SlotOutL = 40, SlotOutR = 44, SlotMinGain = 48;

    private readonly MemoryMappedFile _file;
    private readonly MemoryMappedViewAccessor _view;
    private readonly byte* _p;
    private readonly uint[] _lastSlotSeq = new uint[MeterSlots];

    /// <summary>Ort der Zustandsdatei; CNX_STATE_FILE lenkt sie für Tests um.</summary>
    public static string DefaultPath =>
        Environment.GetEnvironmentVariable("CNX_STATE_FILE") is { Length: > 0 } custom
            ? custom
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "CompressorNext", "state.bin");

    public string FilePath { get; }

    public SharedState(string? path = null)
    {
        FilePath = path ?? DefaultPath;
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        var stream = new FileStream(FilePath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length < FileSize) stream.SetLength(FileSize);
        _file = MemoryMappedFile.CreateFromFile(stream, null, FileSize, MemoryMappedFileAccess.ReadWrite, HandleInheritability.None, leaveOpen: false);
        _view = _file.CreateViewAccessor(0, FileSize, MemoryMappedFileAccess.ReadWrite);
        byte* p = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        _p = p + _view.PointerOffset;

        if (U32(OffMagic) != Magic || U32(OffVersion) != Version)
        {
            // Neue oder fremde Datei: mit Standardwerten anlegen.
            new Span<byte>(_p, FileSize).Clear();
            SetU32(OffVersion, Version);
            WriteSettings(MaximizerSettings.Default, []);
            Volatile.Write(ref *(uint*)(_p + OffMagic), Magic);
        }
    }

    private uint U32(int off) => Volatile.Read(ref *(uint*)(_p + off));
    private void SetU32(int off, uint v) => Volatile.Write(ref *(uint*)(_p + off), v);
    private float F32(int off) => *(float*)(_p + off);

    public MaximizerSettings ReadSettings() =>
        new MaximizerSettings(U32(OffEnabled) != 0, F32(OffThreshold), F32(OffOutput)).Clamp();

    public uint[] ReadExcluded()
    {
        var n = (int)Math.Min(U32(OffExcludedCount), MaxExcluded);
        var list = new uint[n];
        for (var i = 0; i < n; i++) list[i] = U32(OffExcluded + 4 * i);
        return list;
    }

    /// <summary>Schreibt Einstellungen und Ausnahmeliste. Das Audio-Modul übernimmt sie beim nächsten Audioblock.</summary>
    public void WriteSettings(MaximizerSettings s, IReadOnlyCollection<uint> excluded)
    {
        s = s.Clamp();
        var seq = U32(OffSeq);
        if ((seq & 1) != 0) seq++; // nach einem Abbruch mitten im Schreiben
        SetU32(OffSeq, seq + 1);   // ungerade: wird geschrieben
        SetU32(OffEnabled, s.Enabled ? 1u : 0u);
        *(float*)(_p + OffThreshold) = s.ThresholdDb;
        *(float*)(_p + OffOutput) = s.OutputDb;
        var i = 0;
        foreach (var h in excluded)
        {
            if (i >= MaxExcluded) break;
            SetU32(OffExcluded + 4 * i++, h);
        }
        SetU32(OffExcludedCount, (uint)i);
        SetU32(OffSeq, seq + 2);
    }

    /// <summary>
    /// Fasst alle neuen Meldungen der laufenden Audioströme zusammen (lauteste Spitzen, stärkste Begrenzung).
    /// </summary>
    /// <returns>false, wenn seit dem letzten Aufruf nichts Neues kam.</returns>
    public bool ReadMeters(out MeterReading reading, List<StreamInfo>? streams = null)
    {
        reading = new MeterReading { MinGain = 1f };
        var fresh = false;
        var now = (ulong)Environment.TickCount64;
        for (var i = 0; i < MeterSlots; i++)
        {
            var slot = _p + OffMeters + i * SlotSize;
            if (Volatile.Read(ref *(uint*)(slot + SlotOwner)) == 0) continue;
            // Lesen mit Folgenummer: nur vollständig geschriebene Werte übernehmen.
            uint seq1, seq2, endpoint, rate, channels, flags;
            ulong tick;
            float inL, inR, outL, outR, minGain;
            var tries = 0;
            do
            {
                seq1 = Volatile.Read(ref *(uint*)(slot + SlotSeq));
                tick = Volatile.Read(ref *(ulong*)(slot + SlotTick));
                endpoint = *(uint*)(slot + SlotEndpoint);
                rate = *(uint*)(slot + SlotRate);
                channels = *(uint*)(slot + SlotChannels);
                flags = *(uint*)(slot + SlotFlags);
                inL = *(float*)(slot + SlotInL);
                inR = *(float*)(slot + SlotInR);
                outL = *(float*)(slot + SlotOutL);
                outR = *(float*)(slot + SlotOutR);
                minGain = *(float*)(slot + SlotMinGain);
                seq2 = Volatile.Read(ref *(uint*)(slot + SlotSeq));
            } while ((seq1 != seq2 || (seq1 & 1) != 0) && ++tries < 4);
            if (seq1 != seq2 || (seq1 & 1) != 0) continue;
            if (tick == 0 || now - tick > 1000) continue; // Strom meldet sich nicht mehr
            streams?.Add(new StreamInfo(endpoint, rate, channels, (flags & 1) != 0));
            if (seq1 == _lastSlotSeq[i]) continue;        // schon ausgewertet
            _lastSlotSeq[i] = seq1;
            fresh = true;
            reading.InL = Math.Max(reading.InL, Sane(inL));
            reading.InR = Math.Max(reading.InR, Sane(inR));
            reading.OutL = Math.Max(reading.OutL, Sane(outL));
            reading.OutR = Math.Max(reading.OutR, Sane(outR));
            if (minGain > 0f && minGain < reading.MinGain) reading.MinGain = minGain;
            reading.Engaged |= (flags & 1) != 0;
        }
        return fresh;
    }

    private static float Sane(float v) => float.IsFinite(v) && v > 0f ? v : 0f;

    /// <summary>Kennung eines Ausgabegeräts, wie sie auch das Audio-Modul bildet (FNV-1a über die GUID in Kleinbuchstaben).</summary>
    public static uint HashEndpoint(string guid)
    {
        var h = 2166136261u;
        foreach (var ch in guid)
        {
            var c = ch is >= 'A' and <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
            h ^= (uint)c & 0xFF;
            h *= 16777619u;
        }
        return h == 0 ? 1 : h;
    }

    public void Dispose()
    {
        _view.SafeMemoryMappedViewHandle.ReleasePointer();
        _view.Dispose();
        _file.Dispose();
    }
}
