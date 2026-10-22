namespace RumiCar.Protocol.Control;

/// <summary>
/// Product version strings (docs 2.1, 7.3): a release is "X.Y.Z" (the tag vX.Y.Z);
/// a build that is not a release carries the commit after a plus sign,
/// "X.Y.Z+g1a2b3c4".  The car reports its own in hello (core_version), the PC
/// application in its title.
/// </summary>
public static class ProductVersion
{
    /// <summary>The "X.Y.Z" part ("0.2.1+g1a2b3c4" -> "0.2.1"); the whole string, trimmed, when it has no such part.</summary>
    public static string Release(string version)
    {
        var v = (version ?? "").Trim();
        var plus = v.IndexOf('+');
        return plus >= 0 ? v[..plus] : v;
    }

    /// <summary>True when the two are builds of the same release number (build metadata ignored).</summary>
    public static bool SameRelease(string a, string b) =>
        Release(a).Length > 0 && string.Equals(Release(a), Release(b), StringComparison.Ordinal);

    /// <summary>True for a build that is not a release (has build metadata).</summary>
    public static bool IsDevelopmentBuild(string version) => (version ?? "").Contains('+');
}
