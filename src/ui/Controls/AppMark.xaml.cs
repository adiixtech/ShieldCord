using System.Windows.Controls;

namespace ShieldCordUI.Controls;

/// <summary>
/// The product mark — see the XAML for why it is an abstract tile rather than a
/// picture of a computer.
///
/// A UserControl rather than a bare ResourceDictionary of geometry because the
/// brushes inside it have to resolve through <c>DynamicResource</c> at the point
/// of use for the theme swap to reach them, and a frozen Freezable pulled out of
/// a resource dictionary would not.
/// </summary>
public partial class AppMark : UserControl
{
    public AppMark() => InitializeComponent();
}
