using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace ShieldCordUI.Services;

/// <summary>Outcome of one elevated round-trip.</summary>
/// <param name="Started">False when UAC was dismissed or the helper could not launch.</param>
/// <param name="Cancelled">True specifically when the user dismissed the UAC prompt.</param>
/// <param name="ReplyJson">The engine's reply, when the verb actually ran.</param>
public sealed record ElevatedResult(bool Started, bool Cancelled, string? ReplyJson)
{
    public static readonly ElevatedResult Denied = new(false, true, null);
    public static readonly ElevatedResult Failed = new(false, false, null);
}

/// <summary>
/// Runs ONE privileged verb in a short-lived elevated copy of this executable.
///
/// Why not a requireAdministrator manifest: that would prompt for UAC every
/// time the app starts, including at login, just to show a tray icon. Instead
/// the UI runs asInvoker and re-launches itself with the "runas" verb only
/// when the user actually changes something — one prompt per deliberate action.
///
/// The request and reply travel through temp files rather than command-line
/// arguments: a publisher name or path can contain spaces and quotes, and
/// getting Windows command-line quoting right for arbitrary user data is a
/// losing game.
/// </summary>
public static class ElevatedHelper
{
    public const string VerbSwitch = "--elevated-verb";
    public const string OutSwitch  = "--out";

    /// <summary>
    /// How long the parent waits for the elevated child.
    ///
    /// Sized for the ordinary verbs, which answer in well under a second.
    /// Installing the driver is the exception and passes its own: it runs
    /// bcdedit, then an installer that waits up to a minute for the driver to
    /// reach RUNNING, so it can legitimately take far longer than this. The
    /// timeout must never be so short that a slow-but-working install is killed
    /// and then reported as a failure.
    /// </summary>
    private static readonly TimeSpan DefaultChildTimeout = TimeSpan.FromSeconds(30);

    public static async Task<ElevatedResult> RunAsync(string requestJson,
                                                      CancellationToken ct = default,
                                                      TimeSpan? timeout = null)
    {
        string exe = Environment.ProcessPath ?? "";
        if (string.IsNullOrEmpty(exe) || !File.Exists(exe)) return ElevatedResult.Failed;

        TimeSpan childTimeout = timeout ?? DefaultChildTimeout;

        string stamp     = Guid.NewGuid().ToString("N");
        string requestFile = Path.Combine(Path.GetTempPath(), $"shieldcord-req-{stamp}.json");
        string replyFile   = Path.Combine(Path.GetTempPath(), $"shieldcord-rep-{stamp}.json");

        try
        {
            await File.WriteAllTextAsync(requestFile, requestJson, new UTF8Encoding(false), ct);

            var psi = new ProcessStartInfo
            {
                FileName        = exe,
                UseShellExecute = true,          // required for Verb = "runas"
                Verb            = "runas",
                Arguments       = $"{VerbSwitch} \"{requestFile}\" {OutSwitch} \"{replyFile}\"",
                WindowStyle     = ProcessWindowStyle.Hidden,
            };

            try
            {
                using var proc = Process.Start(psi);
                if (proc is null) return ElevatedResult.Failed;

                using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                cts.CancelAfter(childTimeout);
                try
                {
                    await proc.WaitForExitAsync(cts.Token);
                }
                catch (OperationCanceledException)
                {
                    try { proc.Kill(entireProcessTree: true); } catch { }
                    return ElevatedResult.Failed;
                }
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)
            {
                // ERROR_CANCELLED — the user pressed No on the UAC prompt.
                // This is a decision, not a failure, and the UI must say so.
                return ElevatedResult.Denied;
            }
            catch (Win32Exception)
            {
                return ElevatedResult.Failed;
            }

            string? reply = File.Exists(replyFile)
                ? await File.ReadAllTextAsync(replyFile, ct)
                : null;

            return new ElevatedResult(true, false, string.IsNullOrWhiteSpace(reply) ? null : reply);
        }
        catch (OperationCanceledException)
        {
            return ElevatedResult.Failed;
        }
        finally
        {
            TryDelete(requestFile);
            TryDelete(replyFile);
        }
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { /* best effort */ }
    }
}
