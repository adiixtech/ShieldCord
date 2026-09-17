using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Text;
using System.Text.Json.Nodes;
using System.Windows.Data;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Protocol;
using ShieldCordUI.Services;

namespace ShieldCordUI.ViewModels;

/// <summary>
/// The activity timeline, plus the actions that make an alert actionable.
///
/// The list itself lives on the shell (so it survives navigation and is filled
/// even while another page is open); this view model only filters and acts on
/// it. Filtering goes through an ICollectionView rather than a materialised
/// copy, which is what keeps a 500-row list virtualized instead of building
/// 500 containers.
/// </summary>
public sealed class AlertsViewModel : ObservableObject
{
    private readonly ShellViewModel _shell;

    private AlertRecord? _selected;
    private string _filter = FilterAll;
    private string _actionMessage = "";
    private string _searchText = "";
    private int _visibleCount;

    public const string FilterAll    = "all";
    public const string FilterBlocks = "blocked";
    public const string FilterKills  = "killed";
    public const string FilterMemory = "memory";

    public AlertsViewModel(ShellViewModel shell)
    {
        _shell = shell;

        View = CollectionViewSource.GetDefaultView(shell.Alerts);
        View.Filter = Passes;

        KillCommand = new AsyncRelayCommand(KillAsync,
            () => Selected is { Pid: > 0 });

        TrustPublisherCommand = new AsyncRelayCommand(TrustPublisherAsync,
            () => SharedPublisher() is not null);

        CopyPathCommand = new RelayCommand(CopyPath, () => _selection.Count > 0);
        ClearCommand = new RelayCommand(ClearVisible, () => shell.Alerts.Count > 0);

        // Enabled when ANY selected event has a convertible path, so a selection
        // that happens to include one is not silently refused.
        CopyWindowsPathCommand = new RelayCommand(CopyWindowsPath,
            () => _selection.Any(r => KernelPath.IsConvertible(r.Path)));

        CopyDetailsCommand   = new RelayCommand(CopyDetails,   () => _selection.Count > 0);

        // Ctrl+C gets its own command rather than aliasing a menu item: it is the
        // one people reach for without looking, and the two should be free to say
        // different things later without one silently changing the other.
        CopyCommand = new RelayCommand(CopyDetails, () => _selection.Count > 0);

        // Removing a detection is the one action here that cannot be undone from
        // the same menu, so it exists alongside a restore rather than on its own —
        // see RestoreDismissedCommand. Works over the whole selection: clearing
        // twenty lines of noise one at a time is not a feature.
        DismissCommand = new RelayCommand(Dismiss, () => _selection.Count > 0);

        // Enabled only when something is actually hidden, so the menu does not
        // offer to restore nothing.
        RestoreDismissedCommand = new RelayCommand(RestoreDismissed,
            () => _shell.HasDismissedAlerts);

        SetFilterCommand = new RelayCommand(p => Filter = p as string ?? FilterAll);
        ClearSearchCommand = new RelayCommand(() => SearchText = "");
    }

    public ICollectionView View { get; }

    public AsyncRelayCommand KillCommand { get; }
    public AsyncRelayCommand TrustPublisherCommand { get; }
    public RelayCommand CopyPathCommand { get; }
    public RelayCommand CopyWindowsPathCommand { get; }
    public RelayCommand CopyDetailsCommand { get; }
    public RelayCommand CopyCommand { get; }
    public RelayCommand DismissCommand { get; }
    public RelayCommand RestoreDismissedCommand { get; }
    public RelayCommand ClearCommand { get; }
    public RelayCommand SetFilterCommand { get; }
    public RelayCommand ClearSearchCommand { get; }

    /// <summary>
    /// Free-text filter over the timeline. Composes with the action pills
    /// rather than replacing them, so "killed" + "gimp" narrows to killed
    /// events involving that process.
    ///
    /// This is a filter over records already in hand, not a search of the
    /// engine: it can only ever find what the UI still holds, and the header
    /// says so.
    /// </summary>
    public string SearchText
    {
        get => _searchText;
        set
        {
            if (!SetProperty(ref _searchText, value)) return;
            View.Refresh();
            RaiseCounts();
            OnPropertiesChanged(nameof(HasSearch), nameof(IsFiltered));
        }
    }

    public bool HasSearch => !string.IsNullOrWhiteSpace(_searchText);

    /// <summary>True when the list on screen is a subset of the feed.</summary>
    public bool IsFiltered => HasSearch || _filter != FilterAll;

    /// <summary>
    /// Set by the view. The view model decides WHEN a confirmation is needed
    /// (trusting a publisher is a real reduction in protection); the view
    /// decides how to ask.
    /// </summary>
    public Func<string, string, bool>? Confirm { get; set; }

    /// <summary>Copies text to the clipboard. Injected so the view model stays
    /// free of WPF clipboard threading concerns.</summary>
    public Action<string>? CopyToClipboard { get; set; }

    public AlertRecord? Selected
    {
        get => _selected;
        set
        {
            if (!SetProperty(ref _selected, value)) return;
            KillCommand.RaiseCanExecuteChanged();
            TrustPublisherCommand.RaiseCanExecuteChanged();
            CopyPathCommand.RaiseCanExecuteChanged();
            CopyWindowsPathCommand.RaiseCanExecuteChanged();
            CopyDetailsCommand.RaiseCanExecuteChanged();
            DismissCommand.RaiseCanExecuteChanged();
            OnPropertyChanged(nameof(HasSelection));
            OnPropertyChanged(nameof(SelectionSummary));
            OnPropertyChanged(nameof(SelectedWindowsPath));
            OnPropertyChanged(nameof(HasWindowsPath));
        }
    }

    public bool HasSelection => _selected is not null;

    /// <summary>
    /// Everything currently selected, in list order.
    ///
    /// Held separately from <see cref="Selected"/> because WPF's ListBox
    /// SelectedItems is not bindable, so the view hands the whole set over and
    /// this decides what to do with it. Selected stays as the FOCUSED item — the
    /// one the detail panel describes — while this is what the actions act on.
    /// </summary>
    public IReadOnlyList<AlertRecord> Selection => _selection;

    private readonly List<AlertRecord> _selection = new();

    public int SelectionCount => _selection.Count;

    /// <summary>True when more than one event is selected, for wording that has
    /// to say "events" rather than name a single process.</summary>
    public bool IsMultiSelection => _selection.Count > 1;

    /// <summary>Called by the view whenever the list's selection changes.</summary>
    public void SetSelection(IEnumerable<AlertRecord> items)
    {
        _selection.Clear();
        _selection.AddRange(items);

        KillCommand.RaiseCanExecuteChanged();
        TrustPublisherCommand.RaiseCanExecuteChanged();
        CopyPathCommand.RaiseCanExecuteChanged();
        CopyWindowsPathCommand.RaiseCanExecuteChanged();
        CopyDetailsCommand.RaiseCanExecuteChanged();
        CopyCommand.RaiseCanExecuteChanged();
        DismissCommand.RaiseCanExecuteChanged();

        OnPropertiesChanged(nameof(SelectionCount), nameof(IsMultiSelection),
                            nameof(SelectionSummary));
    }

    /// <summary>
    /// The one publisher every selected event shares, or null when there is not
    /// exactly one.
    ///
    /// Trusting a publisher grants access to every protected path for everything
    /// it signs, so it is never offered over a mixed selection — "trust these
    /// three publishers" is not a decision anyone means to make by drag-selecting
    /// a list.
    /// </summary>
    private string? SharedPublisher()
    {
        string? shared = null;

        foreach (var record in _selection)
        {
            if (string.IsNullOrWhiteSpace(record.Publisher)) return null;
            if (shared is null) shared = record.Publisher;
            else if (!string.Equals(shared, record.Publisher, StringComparison.OrdinalIgnoreCase))
                return null;
        }

        return shared;
    }

    /// <summary>
    /// The detected path as a Windows path, for showing under the raw one.
    ///
    /// The raw value is what the kernel reported and is the evidence; this is the
    /// form anybody will actually try to open. Shown only when it says something
    /// different, which <see cref="HasWindowsPath"/> decides.
    /// </summary>
    public string SelectedWindowsPath =>
        _selected is null ? "" : KernelPath.Describe(_selected.Path);

    public bool HasWindowsPath =>
        _selected is not null && KernelPath.IsConvertible(_selected.Path);

    /// <summary>
    /// The line above the detail panel: which event is being described, or how
    /// many are selected.
    ///
    /// Multi-selection says so plainly rather than describing the focused one —
    /// naming a single process while five are selected is how someone acts on the
    /// wrong thing.
    /// </summary>
    public string SelectionSummary
    {
        get
        {
            if (_selection.Count > 1) return $"{_selection.Count} events selected.";
            if (_selected is null) return "Select an event to act on it.";

            return string.IsNullOrWhiteSpace(_selected.Publisher)
                ? $"{_selected.ProcessName} (PID {_selected.Pid}) — publisher unknown, so it cannot be trusted."
                : $"{_selected.ProcessName} (PID {_selected.Pid}) — signed by {_selected.Publisher}";
        }
    }

    public string Filter
    {
        get => _filter;
        set
        {
            if (!SetProperty(ref _filter, value)) return;
            View.Refresh();
            RaiseCounts();
            OnPropertyChanged(nameof(IsFiltered));
        }
    }

    public string ActionMessage
    {
        get => _actionMessage;
        private set { if (SetProperty(ref _actionMessage, value)) OnPropertyChanged(nameof(HasActionMessage)); }
    }

    public bool HasActionMessage => !string.IsNullOrEmpty(_actionMessage);

    public int TotalCount => _shell.Alerts.Count;

    /// <summary>How many rows survive the current filter and search. Shown
    /// beside the total so a narrowed list never looks like a lost feed.</summary>
    public int VisibleCount
    {
        get => _visibleCount;
        private set => SetProperty(ref _visibleCount, value);
    }

    public bool IsEmpty => _shell.Alerts.Count == 0;

    /// <summary>
    /// An empty feed means two very different things and they must not read the
    /// same. With the engine connected, an empty list is good news — nothing has
    /// tried. With it unreachable nothing is being recorded at all, and a green
    /// tick over "nothing has been blocked" would be the reassuring reading of a
    /// situation that is the opposite of reassuring.
    /// </summary>
    public bool EmptyIsReassuring => _shell.IsConnected;

    public string EmptyHeadline => _shell.IsConnected
        ? "Nothing has been recorded yet."
        : "The engine is not reachable.";

    public string EmptyDetail => _shell.IsConnected
        ? "Events appear here the moment something tries to read your tokens."
        : "Nothing is being recorded while the service is not responding, so an empty list here proves nothing.";

    /// <summary>Re-evaluate the empty state when the engine comes or goes.</summary>
    public void OnConnectionChanged() =>
        OnPropertiesChanged(nameof(EmptyIsReassuring), nameof(EmptyHeadline), nameof(EmptyDetail));

    /// <summary>
    /// The feed has events, but none survive the filter and search. Distinct
    /// from an empty feed: "nothing was ever blocked" and "nothing matched what
    /// you typed" are different answers and must not share a message.
    /// </summary>
    public bool HasNoResults => _visibleCount == 0 && _shell.Alerts.Count > 0;

    public void OnAlertSeen(AlertRecord record)
    {
        RaiseCounts();
        ClearCommand.RaiseCanExecuteChanged();
    }

    public void RaiseCounts()
    {
        OnPropertyChanged(nameof(TotalCount));
        OnPropertyChanged(nameof(IsEmpty));
        VisibleCount = View.Cast<object>().Count();
        OnPropertyChanged(nameof(HasNoResults));
    }

    private bool Passes(object item)
    {
        if (item is not AlertRecord r) return false;

        bool byAction = _filter switch
        {
            FilterBlocks => r.Action == Actions.Blocked,
            FilterKills  => r.Action is Actions.Killed or Actions.KillFailed,
            FilterMemory => r.Source == Source.Memory,
            _            => true,
        };
        if (!byAction) return false;

        string query = _searchText.Trim();
        if (query.Length == 0) return true;

        // Deliberately the fields an investigator would have in hand: which
        // program, who signed it, what it touched, and what the engine said.
        return Matches(r.ProcessName, query)
            || Matches(r.Publisher, query)
            || Matches(r.Path, query)
            || Matches(r.Target, query)
            || Matches(r.Message, query);
    }

    private static bool Matches(string haystack, string needle) =>
        !string.IsNullOrEmpty(haystack)
        && haystack.Contains(needle, StringComparison.OrdinalIgnoreCase);

    // ── actions ──────────────────────────────────────────────

    private async Task KillAsync()
    {
        var target = Selected;
        if (target is null || target.Pid <= 0) return;

        if (Confirm is not null &&
            !Confirm($"Terminate {target.ProcessName} (PID {target.Pid})?",
                     "ShieldCord will end this process now. Use this only for something you "
                     + "believe is malicious — an unsaved document in that app would be lost."))
        {
            return;
        }

        var outcome = await _shell.RunPrivilegedAsync(Verbs.KillProcess,
            o => o["pid"] = target.Pid);

        ActionMessage = outcome.Ok
            ? $"Termination requested for PID {target.Pid}."
            : outcome.Describe();
    }

    private async Task TrustPublisherAsync()
    {
        // The shared publisher of the whole selection — null when the events come
        // from more than one, which CanExecute already refused.
        string? publisher = SharedPublisher();
        if (publisher is null) return;

        var target = Selected ?? _selection[0];

        if (Confirm is not null &&
            !Confirm($"Trust everything signed by \"{publisher}\"?",
                     $"Any program signed by \"{publisher}\" will be allowed to read "
                     + "Discord and browser tokens, now and in future. Only do this if you "
                     + "recognise the publisher and the block was a false alarm.\n\n"
                     + "You can undo this in Settings → Trusted publishers."))
        {
            return;
        }

        var outcome = await _shell.RunPrivilegedAsync(Verbs.TrustPublisher, o =>
        {
            o["publisher"] = publisher;
            o["pid"]       = target.Pid;
        });

        ActionMessage = outcome.Ok
            ? $"\"{publisher}\" is now trusted. Remove it in Settings if that was a mistake."
            : outcome.Describe();

        if (outcome.Ok) await _shell.SettingsPage.ReloadAsync();
    }

    private void CopyPath()
    {
        if (_selection.Count == 0) return;

        // One line per event, in list order. Copying a multi-selection as a single
        // run-together string would be unusable, and joining with newlines is what
        // a paste into a terminal or a chat expects.
        var lines = _selection.Select(r =>
            !string.IsNullOrWhiteSpace(r.Path)   ? r.Path
          : !string.IsNullOrWhiteSpace(r.Target) ? r.Target
          : r.Message);

        CopyToClipboard?.Invoke(string.Join(Environment.NewLine, lines));
        ActionMessage = _selection.Count == 1
            ? "Copied to the clipboard."
            : $"Copied {_selection.Count} paths.";
    }

    /// <summary>
    /// Copy the path as a Windows path.
    ///
    /// The driver reports <c>\Device\HarddiskVolume3\...</c>, which is exact and
    /// unusable: no Explorer address bar, terminal or bug report accepts it. This
    /// is the version the user can actually paste somewhere.
    ///
    /// Over a selection, events whose volume cannot be named fall back to their
    /// raw path rather than being dropped — losing a line silently would make the
    /// count wrong, and a count that is wrong is worse than one that is ugly.
    /// </summary>
    private void CopyWindowsPath()
    {
        if (_selection.Count == 0) return;

        var lines = _selection.Select(r => KernelPath.Describe(r.Path));
        CopyToClipboard?.Invoke(string.Join(Environment.NewLine, lines));

        int converted = _selection.Count(r => KernelPath.IsConvertible(r.Path));

        ActionMessage = _selection.Count == 1
            ? "Copied as a Windows path."
            : converted == _selection.Count
                ? $"Copied {converted} Windows paths."
                : $"Copied {converted} of {_selection.Count} as Windows paths; "
                + "the rest are on volumes ShieldCord cannot name.";
    }

    /// <summary>
    /// Copy the whole detection as plain text, for pasting into a chat or a bug
    /// report. Deliberately includes the raw kernel path AND its Windows form when
    /// there is one: the raw form is the evidence, the Windows form is what
    /// anybody will try to open.
    ///
    /// Over a selection, one block per event separated by a blank line, so the
    /// paste stays readable instead of running into one wall.
    /// </summary>
    private void CopyDetails()
    {
        if (_selection.Count == 0) return;

        var blocks = _selection.Select(Describe).ToList();

        CopyToClipboard?.Invoke(
            string.Join(Environment.NewLine + Environment.NewLine, blocks));

        ActionMessage = _selection.Count == 1
            ? "Copied the full detection."
            : $"Copied {_selection.Count} detections.";
    }

    private static string Describe(AlertRecord target)
    {
        var sb = new StringBuilder();
        sb.AppendLine("ShieldCord detection");
        sb.AppendLine($"Time:      {target.Time}");
        sb.AppendLine($"Severity:  {target.Severity}");
        sb.AppendLine($"Action:    {target.Action}");

        if (!string.IsNullOrWhiteSpace(target.ProcessName))
            sb.AppendLine($"Process:   {target.ProcessName} (PID {target.Pid})");

        if (!string.IsNullOrWhiteSpace(target.Publisher))
            sb.AppendLine($"Publisher: {target.Publisher}");
        else
            sb.AppendLine("Publisher: could not be read for this process");

        if (!string.IsNullOrWhiteSpace(target.Path))
        {
            sb.AppendLine($"Path:      {target.Path}");

            if (KernelPath.TryConvert(target.Path, out string windows))
                sb.AppendLine($"           {windows}");
        }

        if (!string.IsNullOrWhiteSpace(target.Target))
            sb.AppendLine($"Target:    {target.Target}");

        if (!string.IsNullOrWhiteSpace(target.Message))
            sb.AppendLine($"Detail:    {target.Message}");

        return sb.ToString().TrimEnd();
    }

    /// <summary>
    /// Take the selected detections out of the timeline.
    ///
    /// Hides them from this list only. The engine keeps its records and its
    /// "protection events" count is unaffected, because that figure is what the
    /// engine did — not what is currently on screen. Removing a row here is
    /// tidying, and it must not read as "this never happened".
    /// </summary>
    private void Dismiss()
    {
        if (_selection.Count == 0) return;

        int count = _selection.Count;

        // Copied first: DismissAlert mutates the shell's collection, and the
        // selection list is holding references into it.
        foreach (var record in _selection.ToList()) _shell.DismissAlert(record.Id);

        SetSelection(Array.Empty<AlertRecord>());
        Selected = null;
        RestoreDismissedCommand.RaiseCanExecuteChanged();

        ActionMessage = count == 1
            ? "Removed from this list. The engine's own record is unchanged; "
            + "use \"Restore removed\" to bring it back."
            : $"Removed {count} detections from this list. The engine's own records are "
            + "unchanged; use \"Restore removed\" to bring them back.";
    }

    /// <summary>Bring back everything currently hidden, re-read from the engine.</summary>
    private void RestoreDismissed()
    {
        _shell.RestoreDismissedAlerts();
        RestoreDismissedCommand.RaiseCanExecuteChanged();
        ActionMessage = "Restoring removed detections from the engine's history.";
    }

    private void ClearVisible()
    {
        _shell.Alerts.Clear();
        Selected = null;
        RaiseCounts();
        ClearCommand.RaiseCanExecuteChanged();
        ActionMessage = "Timeline cleared. This only clears the view — the engine's log is untouched.";
    }
}
