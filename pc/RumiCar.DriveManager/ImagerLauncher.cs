using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace RumiCar.DriveManager;

/// <summary>
/// Finds Raspberry Pi Imager on this PC and opens it with a repository
/// manifest (docs 6.5).  The writing itself — choosing the card, unmounting
/// its volumes, writing, verifying, ejecting — stays with Imager: its
/// executable asks for administrator rights by itself (its manifest requires
/// them), so this application never runs elevated.
///
/// Imager 2.0's installer (Inno Setup; src/windows/rpi-imager.iss.in in
/// raspberrypi/rpi-imager) installs to "Program Files\Raspberry Pi Ltd\Imager",
/// registers the path under App Paths\rpi-imager.exe and associates
/// .rpi-imager-manifest with "rpi-imager.exe" "%1"; Imager treats a manifest
/// given as its first argument like --repo (src/main.cpp).  The 1.x installer
/// used "Program Files (x86)\Raspberry Pi Imager".
/// </summary>
public static class ImagerLauncher
{
    public const string DownloadPage = "https://www.raspberrypi.com/software/";
    public const string ExeName = "rpi-imager.exe";

    /// <summary>The path of rpi-imager.exe, or null when Imager is not installed.</summary>
    public static string? Find()
    {
        foreach (var hive in new[] { Registry.LocalMachine, Registry.CurrentUser })
        {
            try
            {
                using var key = hive.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\App Paths\" + ExeName);
                if (key?.GetValue("") is string p && File.Exists(Trim(p))) return Trim(p);
            }
            catch (Exception) { /* no access to that hive: try the next source */ }
        }
        foreach (var root in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
        {
            if (string.IsNullOrEmpty(root)) continue;
            foreach (var sub in new[] { @"Raspberry Pi Ltd\Imager", "Raspberry Pi Imager" })
            {
                var p = Path.Combine(root, sub, ExeName);
                if (File.Exists(p)) return p;
            }
        }
        return null;
    }

    private static string Trim(string registryValue) => registryValue.Trim().Trim('"');

    /// <summary>
    /// Starts Imager with <paramref name="manifestPath"/> as its repository.
    /// Returns what was started (for the log).  Throws <see cref="FileNotFoundException"/>
    /// when Imager is not installed and the manifest has no file association either.
    /// </summary>
    public static string Launch(string manifestPath)
    {
        if (!File.Exists(manifestPath)) throw new FileNotFoundException("Imager の一覧ファイルがありません", manifestPath);
        var exe = Find();
        if (exe is not null)
        {
            Process.Start(new ProcessStartInfo(exe) { ArgumentList = { manifestPath }, UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe) });
            return exe;
        }
        // No known installation: let Windows open the manifest with whatever is associated with it (Imager 2.0 registers the type).
        try
        {
            using var key = Registry.ClassesRoot.OpenSubKey(".rpi-imager-manifest");
            if (key?.GetValue("") is string progId && progId.Length > 0)
            {
                Process.Start(new ProcessStartInfo(manifestPath) { UseShellExecute = true });
                return "関連付け " + progId;
            }
        }
        catch (Exception) { /* no association readable: fall through */ }
        throw new FileNotFoundException("Raspberry Pi Imager が見つかりません（" + DownloadPage + " から導入してください）", ExeName);
    }
}
