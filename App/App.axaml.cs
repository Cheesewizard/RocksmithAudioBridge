using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using RocksmithAudioBridge.App.ViewModels;
using RocksmithAudioBridge.App.Views;

namespace RocksmithAudioBridge.App;

public partial class App : Application
{
	public override void Initialize() => AvaloniaXamlLoader.Load(this);

	public override void OnFrameworkInitializationCompleted()
	{
		if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
		{
			var window = new MainWindow();
			window.DataContext = new MainWindowViewModel(new AppState(), window);
			desktop.MainWindow = window;
		}
		base.OnFrameworkInitializationCompleted();
	}
}
