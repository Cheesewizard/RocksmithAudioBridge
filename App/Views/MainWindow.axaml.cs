using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using RocksmithAudioBridge.App.ViewModels;

namespace RocksmithAudioBridge.App.Views;

public partial class MainWindow : Window, IDialogs
{
	public MainWindow()
	{
		InitializeComponent();
		if (ScreenshotMode.Directory != null) Opened += async (_, _) => await ScreenshotMode.CaptureAsync(this);
	}

	public async Task<string?> PickFolderAsync(string title, string? startFolder)
	{
		var options = new FolderPickerOpenOptions { Title = title, AllowMultiple = false };
		if (startFolder != null && Directory.Exists(startFolder))
			options.SuggestedStartLocation = await StorageProvider.TryGetFolderFromPathAsync(startFolder);
		var folders = await StorageProvider.OpenFolderPickerAsync(options);
		return folders.Count > 0 ? folders[0].TryGetLocalPath() : null;
	}

	/// <summary>A small modal in the app's own style (Avalonia has no message box).</summary>
	public async Task<bool> ConfirmAsync(string title, string message, string confirmText)
	{
		var dialog = new Window
		{
			Title = title,
			Width = 520,
			SizeToContent = SizeToContent.Height,
			CanResize = false,
			WindowStartupLocation = WindowStartupLocation.CenterOwner,
			ShowInTaskbar = false,
		};
		var confirm = new Button { Content = confirmText, Classes = { "primary" }, IsDefault = true };
		var cancel = new Button { Content = "Cancel", Classes = { "ghost" }, IsCancel = true };
		confirm.Click += (_, _) => dialog.Close(true);
		cancel.Click += (_, _) => dialog.Close(false);
		dialog.Content = new StackPanel
		{
			Margin = new Thickness(24, 20),
			Spacing = 16,
			Children =
			{
				new TextBlock { Text = title, Classes = { "cardTitle" } },
				new TextBlock { Text = message, TextWrapping = Avalonia.Media.TextWrapping.Wrap, Classes = { "muted" } },
				new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right, Children = { cancel, confirm } },
			},
		};
		return await dialog.ShowDialog<bool>(this);
	}
}

/// <summary>
/// --screenshot "&lt;folder&gt;": opens the window, renders every page to &lt;folder&gt;\NN-Page.png, writes
/// startup.log and exits. A check that the app reaches its main window and every page renders, with no clicks.
/// Pages only read; nothing is written outside that folder.
/// </summary>
public static class ScreenshotMode
{
	public static string? Directory { get; private set; }

	public static void Parse(string[] args)
	{
		int index = Array.IndexOf(args, "--screenshot");
		if (index >= 0 && index + 1 < args.Length)
		{
			Directory = Path.GetFullPath(args[index + 1]);
			ViewModels.AppState.ReadOnly = true;
		}
	}

	public static async Task CaptureAsync(MainWindow window)
	{
		string folder = Directory!;
		System.IO.Directory.CreateDirectory(folder);
		var log = new List<string> { "MainWindow opened " + DateTime.Now.ToString("O") };
		try
		{
			if (window.DataContext is MainWindowViewModel model)
			{
				for (int index = 0; index < model.Pages.Count; index++)
				{
					var page = model.Pages[index];
					model.SelectedPage = page;
					await Task.Delay(700);
					await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Render);
					var size = new PixelSize((int)window.ClientSize.Width, (int)window.ClientSize.Height);
					using var bitmap = new RenderTargetBitmap(size);
					bitmap.Render(window);
					string file = Path.Combine(folder, $"{index + 1:00}-{page.Title.Replace(" / ", "-").Replace(' ', '-')}.png");
					bitmap.Save(file);
					log.Add($"{page.Title}: rendered; message='{page.Message}'");
				}
			}
		}
		catch (Exception error) { log.Add("ERROR " + error); }
		File.WriteAllLines(Path.Combine(folder, "startup.log"), log);
		window.Close();
	}
}
