namespace ShieldCordUI.ViewModels;

/// <summary>
/// The Help page is static text, so it has no state to hold.
///
/// It exists as a type at all only because that is how App.xaml maps a view
/// model to a view: the shell's ContentControl binds to an instance and WPF
/// picks the DataTemplate by CLR type. Every claim the page makes is one the
/// rest of the app can back up — there is no feature list here that the engine
/// does not actually implement.
/// </summary>
public sealed class HelpViewModel
{
}
