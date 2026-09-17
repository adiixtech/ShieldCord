using System.Globalization;

namespace ShieldCordUI.Services;

/// <summary>
/// The one place that understands the engine's timestamp format.
///
/// AlertHistory::Append (src/service/alert_history.cpp) stamps every record with
/// the machine's local time via
/// <c>swprintf_s(L"%04d-%02d-%02dT%02d:%02d:%02d")</c> — an ISO-8601 shape with
/// no zone designator and no sub-second part.
///
/// Three things need a real <see cref="DateTime"/> out of an alert: the activity
/// list, the "last attempt" figure, and the traffic chart's bucketer. Three
/// private parse sites would drift apart the day that format changes, so they
/// all come through here instead.
///
/// A parse failure is never an exception. A record written by an older or newer
/// engine is a reason to render less, never a reason to take the window down.
/// </summary>
public static class AlertTime
{
    /// <summary>Round-trip format, exactly as the engine writes it.</summary>
    private const string WireFormat = "yyyy-MM-ddTHH:mm:ss";

    public static bool TryParse(string? text, out DateTime value) =>
        DateTime.TryParseExact(text, WireFormat, CultureInfo.InvariantCulture,
                               DateTimeStyles.None, out value);

    /// <summary>
    /// "14:32" for something that happened today, "11 Sep 14:32" for anything
    /// older. The feed is read newest-first and is overwhelmingly today's
    /// events, so the date is noise in the common case and essential in the
    /// rare one — but the time alone would be actively misleading for an event
    /// from three days ago, so the date is what decides the shape.
    /// </summary>
    public static string Display(string? text)
    {
        if (!TryParse(text, out var when)) return text ?? "";

        return when.Date == DateTime.Today
            ? when.ToString("HH:mm", CultureInfo.InvariantCulture)
            : when.ToString("d MMM HH:mm", CultureInfo.InvariantCulture);
    }
}
