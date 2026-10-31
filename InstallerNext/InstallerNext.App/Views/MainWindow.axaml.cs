using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using RS2014_Mod_Installer.Core;

namespace RocksmithAudioBridge.Setup.App.Views;

/// <summary>
/// The wizard: one window, the questions the 4.x installer asked in message boxes asked here in small dialogs, and the
/// plan's own log shown as it runs. All logic is InstallerNext.Core's; this only gathers the answers.
/// </summary>
public partial class MainWindow : Window
{
	private readonly ISystemProbe _probe = new SystemProbe();
	private PayloadSource? _payload;

	public MainWindow()
	{
		InitializeComponent();
		BrowseButton.Click += OnBrowse;
		PlanButton.Click += (_, _) => ShowPlan();
		InstallButton.Click += OnInstall;
		UninstallButton.Click += OnUninstall;
		FolderBox.TextChanged += (_, _) => RefreshMode();
		Closed += (_, _) => _payload?.Dispose();

		_payload = PayloadSource.Resolve(null, Append);
		string? detected = null;
		try { detected = RocksmithAudioBridge.Core.GameFolder.Detect() ?? RocksmithLocator.Locate().Path; } catch { }
		FolderBox.Text = detected ?? "";
		StatusText.Text = string.IsNullOrEmpty(detected) ? "Rocksmith 2014 was not found. Choose its folder." : "Rocksmith 2014 found. Ready to install.";
		RefreshMode();
	}

	private string Game => (FolderBox.Text ?? "").Trim();

	// Install, or Reinstall / Repair plus Uninstall when Rocksmith Audio Bridge is already there (as 4.x).
	private void RefreshMode()
	{
		bool installed = false;
		try
		{
			installed = Game.Length > 0 && (File.Exists(ProductLayout.InGame(Game, ProductLayout.InstallRecord))
				|| File.Exists(ProductLayout.InGame(Game, ProductLayout.AppExe)) || InstallPlanner.Legacy4xPresent(Game));
		}
		catch { }
		InstallButton.Content = installed ? "Reinstall / Repair" : "Install";
		UninstallButton.IsVisible = installed;
	}

	private async void OnBrowse(object? sender, RoutedEventArgs e)
	{
		var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Rocksmith 2014 folder", AllowMultiple = false });
		string? path = folders.FirstOrDefault()?.TryGetLocalPath();
		if (path != null) FolderBox.Text = path;
	}

	private InstallOptions InstallOptions(bool? replaceForeign) => new()
	{
		GameFolder = Game,
		Payload = _payload!.Layout,
		ReplaceForeignNativeMod = replaceForeign,
		CreateStartMenuShortcut = StartMenuBox.IsChecked == true,
		CreateDesktopShortcut = DesktopBox.IsChecked == true,
		LaunchApp = LaunchBox.IsChecked == true,
	};

	private void ShowPlan()
	{
		LogBox.Text = "";
		try
		{
			var plan = new InstallPlanner(_probe).PlanInstall(InstallOptions(null));
			foreach (string line in plan.Describe()) Append(line);
		}
		catch (Exception error) { Append("error: " + error.Message); }
	}

	private async void OnInstall(object? sender, RoutedEventArgs e)
	{
		LogBox.Text = "";
		if (!_probe.IsRocksmithFolder(Game)) { StatusText.Text = "Choose the Rocksmith 2014 folder (with cache.psarc and a non-empty dlc folder)."; return; }

		bool? replace = null;
		ExistingNativeMod existing = _probe.InspectNativeMod(Game);
		if (existing.NeedsConfirmation && !InstallPlanner.IsOurNativeMod(existing, Game))
		{
			string question = existing.Kind == ExistingNativeModKind.CustomRsMods
				? "This Rocksmith folder has an RSMods build with other mods: " + existing.Description + ".\n\nRocksmith Audio Bridge replaces it, and those mods stop working until you reinstall their build. Replace it?"
				: "Rocksmith's xinput1_3.dll belongs to another program: " + existing.Description + ".\n\nRocksmith Audio Bridge needs that file, so installing replaces it and that program stops working with Rocksmith. Replace it?";
			replace = await Ask("Replace xinput1_3.dll?", question, yesIsDefault: false);
		}

		var plan = new InstallPlanner(_probe).PlanInstall(InstallOptions(replace));
		if (plan.Blockers.Count > 0)
		{
			foreach (string line in plan.Describe()) Append(line);
			StatusText.Text = "Not installed: " + string.Join(" ", plan.Blockers);
			return;
		}

		RunResult result = await RunAsync(plan);
		if (result.Succeeded)
		{
			StatusText.Text = "Rocksmith Audio Bridge is installed." + (result.Warnings.Count > 0 ? " With warnings: " + string.Join(" ", result.Warnings) : "");
			// Last, so it is the message players see: UsbDk was just installed and only starts after a restart.
			if (result.RestartNeeded && await Ask("Restart required",
				"RESTART YOUR PC BEFORE YOU PLAY.\n\nThe low-latency driver for the Real Tone Cable was just installed. It only starts after Windows restarts. "
				+ "Until then the cable still works, but with Windows' usual delay.\n\nSave anything open in other programs, then press Yes to restart now.", yesIsDefault: true))
			{
				try
				{
					Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "shutdown.exe"),
						"/r /t 10 /c \"Restarting to finish installing Rocksmith Audio Bridge.\"") { UseShellExecute = false, CreateNoWindow = true });
					Close();
				}
				catch (Exception error) { StatusText.Text = "Windows could not be restarted from here (" + error.Message + "). Please restart before playing."; }
			}
		}
		else StatusText.Text = (result.RolledBack ? "Install cancelled and rolled back: " : "Install did not finish: ") + result.Error;
		RefreshMode();
	}

	private async void OnUninstall(object? sender, RoutedEventArgs e)
	{
		LogBox.Text = "";
		if (!await Ask("Uninstall", "Uninstall Rocksmith Audio Bridge from\n" + Game + "?\n\nYour recordings are kept.", yesIsDefault: true)) return;
		bool removeSettings = await Ask("Uninstall", "Also remove your mod settings (RSMods.ini)?\n\nChoose No to keep them for a later reinstall.", yesIsDefault: false);
		bool? removeUsbDk = null;
		if (_probe.IsUsbDkInstalled())
		{
			bool ours = _probe.UsbDkInstalledByUs();
			removeUsbDk = await Ask("Uninstall", ours
				? "Also remove UsbDk, the low-latency Real Tone Cable driver this installer put on the PC?\n\nChoose No only if another program you use needs it."
				: "UsbDk, the low-latency Real Tone Cable driver, is installed on this PC. It may have come with this mod or with another program.\n\nRemove it too?", yesIsDefault: ours);
		}

		var plan = new InstallPlanner(_probe).PlanUninstall(new UninstallOptions { GameFolder = Game, RemoveSettings = removeSettings, RemoveUsbDk = removeUsbDk });
		if (plan.Blockers.Count > 0)
		{
			foreach (string line in plan.Describe()) Append(line);
			StatusText.Text = "Not uninstalled: " + string.Join(" ", plan.Blockers);
			return;
		}
		RunResult result = await RunAsync(plan);
		StatusText.Text = result.Succeeded
			? "Rocksmith Audio Bridge was uninstalled. Your recordings were kept." + (result.Warnings.Count > 0 ? " Warnings: " + string.Join(" ", result.Warnings) : "")
				+ (result.RestartNeeded ? " Restart Windows to finish removing UsbDk." : "")
			: (result.Cancelled ? "Uninstall cancelled: " : "Uninstall failed: ") + result.Error;
		RefreshMode();
	}

	private async Task<RunResult> RunAsync(InstallPlan plan)
	{
		SetBusy(true);
		try
		{
			var host = new SystemInstallHost(line => Dispatcher.UIThread.Post(() => Append(line)));
			return await Task.Run(() => PlanRunner.Run(plan, host, _probe));
		}
		finally { SetBusy(false); }
	}

	private void SetBusy(bool busy)
	{
		Busy.IsVisible = busy;
		InstallButton.IsEnabled = UninstallButton.IsEnabled = PlanButton.IsEnabled = BrowseButton.IsEnabled = !busy;
	}

	private void Append(string line)
	{
		LogBox.Text += line + Environment.NewLine;
		LogBox.CaretIndex = LogBox.Text?.Length ?? 0;
	}

	private async Task<bool> Ask(string title, string question, bool yesIsDefault)
	{
		bool answer = false;
		var dialog = new Window { Title = title, Width = 480, SizeToContent = SizeToContent.Height, CanResize = false, WindowStartupLocation = WindowStartupLocation.CenterOwner };
		var yes = new Button { Content = "Yes", MinWidth = 90, IsDefault = yesIsDefault };
		var no = new Button { Content = "No", MinWidth = 90, IsDefault = !yesIsDefault, IsCancel = true };
		yes.Click += (_, _) => { answer = true; dialog.Close(); };
		no.Click += (_, _) => dialog.Close();
		dialog.Content = new StackPanel
		{
			Margin = new Avalonia.Thickness(20),
			Spacing = 16,
			Children =
			{
				new TextBlock { Text = question, TextWrapping = Avalonia.Media.TextWrapping.Wrap },
				new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 10, Children = { yes, no } },
			},
		};
		await dialog.ShowDialog(this);
		return answer;
	}
}
