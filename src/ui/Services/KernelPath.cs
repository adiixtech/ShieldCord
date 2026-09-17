using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace ShieldCordUI.Services;

/// <summary>
/// Turns the kernel's path notation into something you can actually use.
///
/// The driver reports paths the way the kernel sees them:
///
///     \Device\HarddiskVolume3\ProgramData\ShieldCord\Decoy\chrome\Default\Network
///
/// That is a perfectly accurate path and completely useless to paste anywhere —
/// Explorer, a terminal, or a bug report all want <c>C:\ProgramData\...</c>.
/// Nothing accepts the device form, so copying it verbatim hands the user a
/// string they cannot act on.
///
/// The mapping comes from <c>QueryDosDevice</c>, which is the same call that
/// underlies every "what is my C: drive really" question: ask it about "C:" and
/// it answers with the device path. Query all the letters once and invert it.
/// </summary>
public static class KernelPath
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint QueryDosDeviceW(string lpDeviceName, StringBuilder lpTargetPath, int ucchMax);

    /// <summary>Device path (lowercased) to drive letter, e.g.
    /// "\device\harddiskvolume3" to "C:".</summary>
    private static Dictionary<string, string>? _map;

    /// <summary>
    /// Convert a kernel path to a Windows path. Returns false — leaving
    /// <paramref name="windowsPath"/> empty — when the volume is not one we can
    /// name, which is a real outcome for removable or network volumes and must
    /// not be papered over with a half-converted string.
    /// </summary>
    public static bool TryConvert(string? kernelPath, out string windowsPath)
    {
        windowsPath = "";

        if (string.IsNullOrWhiteSpace(kernelPath)) return false;
        if (!kernelPath.StartsWith(@"\Device\", StringComparison.OrdinalIgnoreCase)) return false;

        if (TryLookup(kernelPath, out windowsPath)) return true;

        // A drive may have been mounted since we last looked, so refresh once and
        // retry before giving up.
        _map = BuildMap();
        return TryLookup(kernelPath, out windowsPath);
    }

    /// <summary>
    /// The converted path when there is one, otherwise the original. For showing
    /// alongside the raw value, where an unconvertible path is still worth seeing.
    /// </summary>
    public static string Describe(string? kernelPath) =>
        TryConvert(kernelPath, out string windows) ? windows : (kernelPath ?? "");

    /// <summary>True when a conversion exists, for deciding whether a second line
    /// would say anything the first one did not.</summary>
    public static bool IsConvertible(string? kernelPath) => TryConvert(kernelPath, out _);

    private static bool TryLookup(string kernelPath, out string windowsPath)
    {
        windowsPath = "";

        Dictionary<string, string> map = _map ??= BuildMap();
        if (map.Count == 0) return false;

        int end = kernelPath.IndexOf('\\', @"\Device\".Length);
        if (end < 0) return false;

        string device = kernelPath[..end].ToLowerInvariant();
        if (!map.TryGetValue(device, out string? drive)) return false;

        windowsPath = drive + kernelPath[end..];
        return true;
    }

    private static Dictionary<string, string> BuildMap()
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var buffer = new StringBuilder(1024);

        for (char letter = 'A'; letter <= 'Z'; letter++)
        {
            string drive = $"{letter}:";

            // Non-zero return means success; zero means no such drive.
            if (QueryDosDeviceW(drive, buffer, buffer.Capacity) == 0) continue;

            // The result is a MULTI_SZ — one device path per string, terminated by
            // an empty entry. A drive can have several (a mounted folder too), and
            // the first is the one that names it.
            string first = buffer.ToString().Split('\0', StringSplitOptions.RemoveEmptyEntries)
                                 .FirstOrDefault() ?? "";
            if (first.Length == 0) continue;

            // First mapping wins: if two letters point at one volume, the earlier
            // letter is the more likely thing the user means.
            map.TryAdd(first, drive);
        }

        return map;
    }
}
