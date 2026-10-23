namespace RumiCar.Protocol.Packs;

/// <summary>One installed logic or pack as list_versions reports it (docs 5.2).</summary>
public sealed record InstalledVersion(string Kind, string DirectoryName, string Id, string Version, bool Verified, string Logic = "");

/// <summary>What to ask the car for: one link, both links, or nothing (with the reason).</summary>
public sealed record ActivationStep(string? Kind, string? Name, string? Logic, string? Pack, string? Error, string? Note = null)
{
    public bool IsSingle => Kind is not null;
    public bool IsPair => Logic is not null;
    public static ActivationStep Single(string kind, string name) => new(kind, name, null, null, null);
    public static ActivationStep PairOf(string logic, string pack, string note) => new(null, null, logic, pack, null, note);
    public static ActivationStep Fail(string error) => new(null, null, null, null, error);
}

/// <summary>
/// A pack is made for one logic (manifest logic.name) and the car runs a
/// logic only with a pack for it.  When the user activates a version of
/// another logic family, the matching other half is chosen here so the car
/// can switch both in one step (activate_pair), instead of rejecting a
/// half-switched pair.  Pure computation (tested).
/// </summary>
public static class ActivationPlanner
{
    /// <summary>The step for activating <paramref name="selected"/> while <paramref name="currentLogic"/>/<paramref name="currentPack"/> (directory names) run.</summary>
    public static ActivationStep Plan(InstalledVersion selected, IReadOnlyList<InstalledVersion> installed, string currentLogic, string currentPack)
    {
        if (!selected.Verified) return ActivationStep.Fail($"{selected.DirectoryName} は検証に失敗しているので使えません");
        var logic = installed.FirstOrDefault(v => v.Kind == "logic" && v.DirectoryName == currentLogic);
        var pack = installed.FirstOrDefault(v => v.Kind == "pack" && v.DirectoryName == currentPack);
        if (selected.Kind == "pack")
        {
            if (selected.Logic.Length == 0 || logic is null || selected.Logic == logic.Id)
                return ActivationStep.Single("pack", selected.DirectoryName);   // same logic (or an older car that does not say)
            var forIt = Newest(installed.Where(v => v.Kind == "logic" && v.Verified && v.Id == selected.Logic));
            if (forIt is null)
                return ActivationStep.Fail($"{selected.DirectoryName} はロジック {selected.Logic} 用ですが、車に {selected.Logic} がありません");
            return ActivationStep.PairOf(forIt.DirectoryName, selected.DirectoryName,
                $"ロジックも {forIt.DirectoryName} に切り替えます（{selected.DirectoryName} は {selected.Logic} 用）");
        }
        if (pack is null || pack.Logic.Length == 0 || pack.Logic == selected.Id)
            return ActivationStep.Single("logic", selected.DirectoryName);
        var packFor = Newest(installed.Where(v => v.Kind == "pack" && v.Verified && v.Logic == selected.Id));
        if (packFor is null)
            return ActivationStep.Fail($"{selected.Id} 用のデータ（パック）が車にありません。先に送ってください");
        return ActivationStep.PairOf(selected.DirectoryName, packFor.DirectoryName,
            $"データも {packFor.DirectoryName} に切り替えます（今のデータ {currentPack} は {pack.Logic} 用）");
    }

    /// <summary>
    /// Rolling back one kind when the previous one belongs to another logic
    /// family than what stays would leave a mismatched pair: then both are rolled back.
    /// </summary>
    public static bool RollbackNeedsBoth(string kind, IReadOnlyList<InstalledVersion> installed,
        string currentLogic, string currentPack, string previousLogic, string previousPack)
    {
        InstalledVersion? Find(string k, string name) => installed.FirstOrDefault(v => v.Kind == k && v.DirectoryName == name);
        if (kind == "logic")
        {
            var prev = Find("logic", previousLogic);
            var pack = Find("pack", currentPack);
            return prev is not null && pack is not null && pack.Logic.Length > 0 && pack.Logic != prev.Id;
        }
        var prevPack = Find("pack", previousPack);
        var logic = Find("logic", currentLogic);
        return prevPack is not null && logic is not null && prevPack.Logic.Length > 0 && prevPack.Logic != logic.Id;
    }

    /// <summary>The newest by version (numeric parts compared as numbers, e.g. 0.10.0 &gt; 0.9.0; 20261023.101500 &gt; 20261022.090000).</summary>
    public static InstalledVersion? Newest(IEnumerable<InstalledVersion> versions) =>
        versions.OrderByDescending(v => v.Version, VersionComparer.Instance).ThenByDescending(v => v.DirectoryName, StringComparer.Ordinal).FirstOrDefault();

    public sealed class VersionComparer : IComparer<string>
    {
        public static readonly VersionComparer Instance = new();

        public int Compare(string? x, string? y)
        {
            var a = (x ?? "").Split('.', '-', '+');
            var b = (y ?? "").Split('.', '-', '+');
            for (var i = 0; i < Math.Max(a.Length, b.Length); i++)
            {
                var pa = i < a.Length ? a[i] : "";
                var pb = i < b.Length ? b[i] : "";
                var na = long.TryParse(pa, out var va);
                var nb = long.TryParse(pb, out var vb);
                var c = na && nb ? va.CompareTo(vb) : string.CompareOrdinal(pa, pb);
                if (c != 0) return c;
            }
            return 0;
        }
    }
}
