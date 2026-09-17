using Microsoft.Win32;

namespace ShieldCordUI.Services;

/// <summary>
/// "Start with Windows" via the per-user Run key.
///
/// HKCU rather than HKLM on purpose: the UI runs asInvoker, and a machine-wide
/// entry would need elevation to write — meaning the user could not turn their
/// own tray icon's autostart off without an admin prompt. The protection itself
/// starts at boot regardless; this only controls the tray icon.
/// </summary>
public static class StartupRegistration
{
    private const string RunKey    = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "ShieldCord";

    /// <summary>
    /// Passed when Windows launches us at sign-in, so the app starts into the
    /// tray instead of throwing a window in the user's face at every login.
    /// </summary>
    public const string StartupSwitch = "--startup";

    public static bool IsEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: false);
            return key?.GetValue(ValueName) is string s && s.Length > 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Returns true when the registry now holds the desired state.</summary>
    public static bool Set(bool enabled)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true)
                            ?? Registry.CurrentUser.CreateSubKey(RunKey);
            if (key is null) return false;

            if (enabled)
            {
                string? exe = Environment.ProcessPath;
                if (string.IsNullOrEmpty(exe)) return false;

                // Quoted: the install path contains a space ("C:\Program Files\...").
                key.SetValue(ValueName, $"\"{exe}\" {StartupSwitch}", RegistryValueKind.String);
            }
            else
            {
                key.DeleteValue(ValueName, throwOnMissingValue: false);
            }
            return true;
        }
        catch
        {
            return false;
        }
    }
}
