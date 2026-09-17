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

    private void Sponsor_Click(object sender, RoutedEventArgs e) => OpenUrl("https://github.com/sponsors/YOUR_GITHUB_USERNAME");
    private void Patreon_Click(object sender, RoutedEventArgs e) => OpenUrl("https://patreon.com/YOUR_PATREON");
    private void Kofi_Click(object sender, RoutedEventArgs e) => OpenUrl("https://ko-fi.com/YOUR_KOFI");
}
