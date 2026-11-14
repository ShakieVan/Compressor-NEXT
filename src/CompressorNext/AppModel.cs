namespace CompressorNext;

/// <summary>Hält Einstellungen und Ausnahmeliste und schreibt Änderungen sofort in den gemeinsamen Zustand.</summary>
internal sealed class AppModel : IDisposable
{
    public SharedState State { get; }
    public MaximizerSettings Settings { get; private set; }
    public HashSet<uint> Excluded { get; }
    public bool Exiting { get; set; }

    /// <summary>Einstellungen haben sich geändert (durch die Oberfläche oder von außen).</summary>
    public event Action? Changed;

    public AppModel(SharedState state)
    {
        State = state;
        Settings = state.ReadSettings();
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
        if (current == Settings) return;
        Settings = current;
        Changed?.Invoke();
    }

    public void Dispose() => State.Dispose();
}
