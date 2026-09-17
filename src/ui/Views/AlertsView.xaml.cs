using System.Collections;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using ShieldCordUI.Protocol;
using ShieldCordUI.ViewModels;

namespace ShieldCordUI.Views;

public partial class AlertsView : UserControl
{
    public AlertsView() => InitializeComponent();

    private AlertsViewModel? ViewModel => DataContext as AlertsViewModel;

    /// <summary>
    /// Hand the whole selection to the view model.
    ///
    /// WPF's ListBox.SelectedItems is not a DependencyProperty, so it cannot be
    /// bound — the view has to push it across. SelectionMode="Extended" gives
    /// Ctrl+click, Shift+click and drag-select for free; this is what makes the
    /// commands act on the result.
    /// </summary>
    private void OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ViewModel is null) return;

        ViewModel.SetSelection(AlertList.SelectedItems.OfType<AlertRecord>());
    }

    /// <summary>
    /// Select the row under a right-click before its context menu opens.
    ///
    /// WPF does not change the selection on right-click, and every command in that
    /// menu acts on the selection — so without this, right-clicking row B while
    /// row A happened to be selected would copy row A's path and offer to
    /// terminate row A's process while the user is pointing at row B.
    ///
    /// The exception is right-clicking INSIDE an existing multi-selection, which
    /// must leave it alone. Collapsing it would mean the menu acted on the whole
    /// selection only until you used it — and "select five, right-click, copy all
    /// five" is the entire point.
    /// </summary>
    private void OnRowRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not ListBox list) return;
        if (e.OriginalSource is not DependencyObject source) return;

        // Null when the click landed on the list's padding rather than a row; the
        // menu is attached to the row, so it will not open in that case anyway.
        if (ItemsControl.ContainerFromElement(list, source) is not ListBoxItem row) return;

        if (row.IsSelected) return;   // keep a multi-selection intact

        list.SelectedItems.Clear();
        row.IsSelected = true;
        row.Focus();
    }

    // ── keyboard ─────────────────────────────────────────────

    private void OnCopyCanExecute(object sender, CanExecuteRoutedEventArgs e)
    {
        e.CanExecute = ViewModel?.CopyCommand.CanExecute(null) == true;
        e.Handled = true;
    }

    private void OnCopyExecuted(object sender, ExecutedRoutedEventArgs e)
    {
        if (ViewModel?.CopyCommand.CanExecute(null) == true) ViewModel.CopyCommand.Execute(null);
        e.Handled = true;
    }

    private void OnSelectAllCanExecute(object sender, CanExecuteRoutedEventArgs e)
    {
        // Nothing to select when the list is empty, so the shortcut stays inert
        // rather than firing on an empty view.
        e.CanExecute = AlertList.Items.Count > 0;
        e.Handled = true;
    }

    /// <summary>
    /// Ctrl+A selects everything the FILTER is showing, not everything held.
    /// Selecting rows the user cannot see would make the next action act on the
    /// invisible — copy would return lines that were never on screen.
    /// </summary>
    private void OnSelectAllExecuted(object sender, ExecutedRoutedEventArgs e)
    {
        AlertList.SelectAll();
        e.Handled = true;
    }
}
