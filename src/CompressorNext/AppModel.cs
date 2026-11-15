namespace CompressorNext;

/// <summary>Hält Einstellungen und Ausnahmeliste und schreibt Änderungen sofort in den gemeinsamen Zustand.</summary>
internal sealed class AppModel : IDisposable
{
    public SharedState State { get; }
    public MaximizerSettings Settings { get; private set; }
    public EqSettings Eq { get; private set; }
    public HashSet<uint> Excluded { get; }
    public bool Exiting { get; set; }

    /// <summary>Pegel hinter dem Equalizer (Auto-Pegel: minus die stärkste Anhebung der Kurve).</summary>
    public float EqLevelDb { get; private set; }

    /// <summary>Einstellungen haben sich geändert (durch die Oberfläche oder von außen).</summary>
    public event Action? Changed;

    public AppModel(SharedState state)
    {
        State = state;
        Settings = state.ReadSettings();
        Eq = state.ReadEq();
        EqLevelDb = EqMath.AutoLevelDb(Eq);
        // Pegel nachziehen, falls ein anderer Aufruf die Bänder ohne passenden Auto-Pegel geschrieben hat.
        if (state.ReadEqLevelDb() != EqLevelDb) state.WriteEq(Eq, EqLevelDb);
        Excluded = [.. state.ReadExcluded()];
    }

    public void Update(MaximizerSettings settings)
    {
        settings = settings.Clamp();
        if (settings == Settings) return;
        Settings = settings;
        State.WriteSettings(Settings, Excluded);
        Changed?.Invoke();
    }

    public void UpdateEq(EqSettings eq)
    {
        if (eq.Equals(Eq)) return;
        Eq = eq;
        EqLevelDb = EqMath.AutoLevelDb(eq);
        State.WriteEq(eq, EqLevelDb);
        Changed?.Invoke();
    }

    public void SetExcluded(uint endpointHash, bool excluded)
    {
        var changed = excluded ? Excluded.Add(endpointHash) : Excluded.Remove(endpointHash);
        if (!changed) return;
        State.WriteSettings(Settings, Excluded);
        Changed?.Invoke();
    }

    /// <summary>Übernimmt Änderungen, die ein anderer Aufruf (z. B. "CompressorNext.exe --set …") geschrieben hat.</summary>
    public void PollExternalChanges()
    {
        var current = State.ReadSettings();
        var eq = State.ReadEq();
        if (current == Settings && eq.Equals(Eq)) return;
        Settings = current;
        if (!eq.Equals(Eq))
        {
            Eq = eq;
            EqLevelDb = EqMath.AutoLevelDb(eq);
            if (State.ReadEqLevelDb() != EqLevelDb) State.WriteEq(eq, EqLevelDb); // Bänder von außen: Auto-Pegel nachziehen
        }
        Changed?.Invoke();
    }

    public void Dispose() => State.Dispose();
}
