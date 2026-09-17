using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;

namespace ShieldCordUI.Views;

public partial class HelpView : UserControl
{
    public HelpView()
    {
        InitializeComponent();
    }

    private void OpenUrl(string url)
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = url,
                UseShellExecute = true
            });
        }
        catch { }
    }

    // Handlers will be restored when payment methods are configured.
}
