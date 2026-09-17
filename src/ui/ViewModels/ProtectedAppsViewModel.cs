using System.Collections.ObjectModel;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Protocol;

namespace ShieldCordUI.ViewModels;

/// <summary>
/// One tile in the Protected Apps grid.
///
/// Normally a signed application from the built-in whitelist, or a publisher the
/// user added at runtime. One instance is the trailing "add" tile, which is a
/// tile rather than a separate element so that it wraps with the rest of the
/// grid instead of sitting beside it.
///
/// Note what is deliberately absent: <c>ProtectedApp.Trusted</c>. The service
/// never puts that key on the wire — BuildProtectedAppsJson emits only
/// process_name, publisher, app_tag, builtin and running — so it always
/// deserialises to false. Binding it would quietly claim that nothing is trusted
/// while the driver is trusting all of it. <see cref="Running"/> is a real
/// measurement, so that is what the tile shows.
/// </summary>
public sealed class ProtectedAppCard : ObservableObject
{
    private bool _isSelected;

    private ProtectedAppCard(string title, string subtitle, string tag, string scope,
                             bool running, bool removable, ProtectedApp? source, bool isAddCard)
    {
        Title     = title;
        Subtitle  = subtitle;
        Tag       = tag;
        Scope     = scope;
        Running   = running;
        Removable = removable;
        Source    = source;
        IsAddCard = isAddCard;
    }

    public static ProtectedAppCard For(ProtectedApp app)
    {
        bool isPublisher = string.IsNullOrEmpty(app.ProcessName);

        return new ProtectedAppCard(
            title:     isPublisher ? app.Publisher : app.ProcessName,
            subtitle:  isPublisher ? "Applies to anything this publisher signs" : app.Publisher,
            tag:       string.IsNullOrEmpty(app.AppTag) ? "—" : app.AppTag,
            scope:     app.Builtin ? "Built-in" : "Added by you",
            running:   app.Running,
            // Only a publisher the user added can be removed. The built-in
            // entries are compiled into the service, so offering to remove one
            // would be an affordance that cannot possibly work.
            removable: !app.Builtin,
            source:    app,
            isAddCard: false);
    }

    /// <summary>The trailing tile. Clicking it goes to the one place a publisher
    /// can actually be trusted today — a blocked event in Activity.</summary>
    public static ProtectedAppCard AddTile() =>
        new(title: "Trust a publisher",
            subtitle: "from a blocked event",
            tag: "", scope: "", running: false, removable: false,
            source: null, isAddCard: true);

    public string Title     { get; }
    public string Subtitle  { get; }
    public string Tag       { get; }
    public string Scope     { get; }
    public bool   Running   { get; }
    public bool   Removable { get; }
    public ProtectedApp? Source { get; }

    /// <summary>True for the trailing add tile, which is not an application and
    /// must not be counted, selected, or acted on as one.</summary>
    public bool IsAddCard { get; }

    /// <summary>Every tile that represents an actual application.</summary>
    public bool IsApp => !IsAddCard;

    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    /// <summary>Process presence right now — never "Online", which would assert
    /// a reachability the engine does not report.</summary>
    public string RunningText => Running ? "Running" : "Not running";

    /// <summary>
    /// Whether this tile has a run state worth showing at all.
    ///
    /// The engine reports process presence only for the built-in whitelist,
    /// which names a binary. A publisher entry names no process — the service
    /// hard-codes running=false for it — so "Not running" there would assert a
    /// measurement that was never taken.
    /// </summary>
    public bool HasRunState => IsApp && Source is not null && !string.IsNullOrEmpty(Source.ProcessName);

    public string Publisher => Source?.Publisher ?? "";
}

/// <summary>
/// The Protected Apps page: everything signed that ShieldCord allows to read its
/// own token stores, plus the publishers the user has added.
///
/// The list itself lives on the shell, because the Settings page renders the
/// same two collections and fetching them separately would mean two process
/// scans on the service for one screen.
/// </summary>
public sealed class ProtectedAppsViewModel : ObservableObject
{
    private readonly ShellViewModel _shell;

    private ProtectedAppCard? _selected;
    private string _status = "";
    private bool _loaded;

    public ProtectedAppsViewModel(ShellViewModel shell)
    {
        _shell = shell;

        RefreshCommand       = new AsyncRelayCommand(RefreshAsync);
        ViewActivityCommand  = new RelayCommand(() => _shell.Navigate("Alerts"));
        FindPublisherCommand = new RelayCommand(() => _shell.Navigate("Alerts"));
        SelectCommand        = new RelayCommand(p => Selected = p as ProtectedAppCard);
        UntrustCommand       = new AsyncRelayCommand(UntrustAsync, () => Selected?.Removable == true);
    }

    public ObservableCollection<ProtectedAppCard> Apps { get; } = new();

    public AsyncRelayCommand RefreshCommand       { get; }
    public RelayCommand      ViewActivityCommand  { get; }
    public RelayCommand      FindPublisherCommand { get; }
    public RelayCommand      SelectCommand        { get; }
    public AsyncRelayCommand UntrustCommand       { get; }

    /// <summary>Set by the view — the view decides how to ask, the view model
    /// decides when asking is warranted.</summary>
    public Func<string, string, bool>? Confirm { get; set; }

    public ProtectedAppCard? Selected
    {
        get => _selected;
        set
        {
            if (ReferenceEquals(_selected, value)) return;

            if (_selected is not null) _selected.IsSelected = false;
            _selected = value;
            if (_selected is not null) _selected.IsSelected = true;

            OnPropertyChanged();
            UntrustCommand.RaiseCanExecuteChanged();
            OnPropertiesChanged(nameof(HasSelection), nameof(HasNoSelection));
        }
    }

    public bool HasSelection => _selected is not null;
    public bool HasNoSelection => _selected is null;

    /// <summary>Applications only — the add tile is not one of them.</summary>
    public int AppCount => Apps.Count(a => a.IsApp);

    /// <summary>How many of the listed applications are running right now. A
    /// real count of a real measurement, not an inference.</summary>
    public int RunningCount => Apps.Count(a => a.IsApp && a.Running);

    public bool HasApps => AppCount > 0;
    public bool HasNoApps => AppCount == 0;

    public string Status
    {
        get => _status;
        private set { if (SetProperty(ref _status, value)) OnPropertyChanged(nameof(HasStatus)); }
    }

    public bool HasStatus => !string.IsNullOrEmpty(_status);

    /// <summary>
    /// The empty state has two very different causes and they must not read the
    /// same: an engine that has not answered yet is not an engine that reports
    /// no applications.
    /// </summary>
    public string EmptyMessage => !_shell.IsConnected
        ? "The ShieldCord engine is not reachable, so this list cannot be read."
        : _loaded
            ? "The engine reported no protected applications."
            : "Loading the protected applications…";

    /// <summary>
    /// Rebuild the tiles after the shell has re-read the lists.
    ///
    /// Selection is carried across by title: a refresh replaces every tile
    /// object, and losing the user's place on a refresh is the kind of small
    /// betrayal that makes a page feel broken.
    /// </summary>
    public void OnDetailsChanged()
    {
        string? keep = _selected?.Title;

        Apps.Clear();
        foreach (var app in _shell.ProtectedApps) Apps.Add(ProtectedAppCard.For(app));
        Apps.Add(ProtectedAppCard.AddTile());

        _loaded = true;

        Selected = Apps.FirstOrDefault(a => a.IsApp && a.Title == keep)
                ?? Apps.FirstOrDefault(a => a.IsApp);

        OnPropertiesChanged(nameof(AppCount), nameof(RunningCount),
                            nameof(HasApps), nameof(HasNoApps), nameof(EmptyMessage));
    }

    /// <summary>
    /// <see cref="EmptyMessage"/> is derived from whether the engine is
    /// reachable, so a connection change has to re-evaluate it — otherwise a
    /// page left open through a disconnect keeps explaining that the engine is
    /// merely busy loading.
    /// </summary>
    public void OnConnectionChanged() => OnPropertyChanged(nameof(EmptyMessage));

    private async Task RefreshAsync()
    {
        Status = "";
        await _shell.LoadDetailsAsync();
        OnPropertyChanged(nameof(EmptyMessage));
    }

    private async Task UntrustAsync()
    {
        var card = Selected;
        if (card is null || !card.Removable) return;

        if (Confirm is not null &&
            !Confirm($"Stop trusting \"{card.Publisher}\"?",
                     "Programs signed by this publisher will no longer be allowed to read "
                     + "Discord and browser tokens, and will be blocked the next time they try.\n\n"
                     + "Processes already running keep their access until they exit."))
        {
            return;
        }

        var outcome = await _shell.RunPrivilegedAsync(Verbs.UntrustPublisher,
            o => o["publisher"] = card.Publisher);

        Status = outcome.Ok
            ? $"\"{card.Publisher}\" removed. Processes already running keep their access until they exit."
            : outcome.Describe();

        if (outcome.Ok) await _shell.LoadDetailsAsync();
    }
}
