using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace ShieldCordUI.Mvvm;

/// <summary>
/// Minimal INotifyPropertyChanged base.
///
/// Deliberately hand-rolled rather than pulling in CommunityToolkit.Mvvm: the
/// app then has zero NuGet dependencies, so a restore failure can never be the
/// reason the UI does not build or ship.
/// </summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    /// <summary>Raises change notifications for several computed properties at once.</summary>
    protected void OnPropertiesChanged(params string[] names)
    {
        foreach (string n in names) OnPropertyChanged(n);
    }

    /// <summary>Assigns and notifies only when the value actually changed.</summary>
    protected bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }
}
