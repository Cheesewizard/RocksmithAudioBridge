using System.Collections.ObjectModel;
using System.Diagnostics;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using RocksmithAudioBridge.Core;

namespace RocksmithAudioBridge.App.ViewModels;

// ---- Overview ---------------------------------------------------------------------------------------------------------
public sealed partial class OverviewViewModel(AppState state, IDialogs dialogs) : PageViewModel(state, dialogs)
{
	public override string Title => "Overview";
	public override string Glyph => "";
	public override string Lead => "Where Rocksmith Audio Bridge stands. Everything here reads the game folder and Windows as they are now.";

	public ObservableCollection<StatusRow> Rows { get; } = new();

	[ObservableProperty] private bool _power;
	[ObservableProperty] private bool _canChangePower;
	private bool loading;

	public override void Refresh()
	{
		State.RefreshGameRunning();
		Rows.Clear();
		Rows.Add(new StatusRow("Rocksmith folder", State.GameDirectoryText, State.HasGame ? StatusTone.Neutral : StatusTone.Bad));
		Rows.Add(new StatusRow("Rocksmith", State.IsGameRunning ? "Running. Settings that need the game closed are locked." : "Closed", State.IsGameRunning ? StatusTone.Warn : StatusTone.Neutral));
		if (State.GameDirectory is string game)
		{
			AudioInputMode mode;
			try { mode = new AudioInputModeConfiguration(game, () => State.IsGameRunning).ReadMode(); }
			catch (Exception error) { Rows.Add(new StatusRow("Guitar input", error.Message, StatusTone.Bad)); mode = AudioInputMode.Unavailable; }
			Rows.Add(new StatusRow("Guitar input", mode switch
			{
				AudioInputMode.Asio => "ASIO interface (RS_ASIO on)",
				AudioInputMode.Cable => "Real Tone Cable (RS_ASIO installed, off)",
				_ => "Real Tone Cable (RS_ASIO not installed)",
			}));
			bool current = AsioProxySetup.IsProxyRegistrationCurrent(Path.Combine(game, "RocksmithAudioBridgeAsio.dll"));
			Rows.Add(new StatusRow("Bridge ASIO driver", current ? "Registered" : AsioProxySetup.IsProxyRegistered() ? "Registered, but not to this game folder's driver" : "Not registered",
				current ? StatusTone.Good : mode == AudioInputMode.Asio ? StatusTone.Bad : StatusTone.Neutral));
			string target = AsioProxySetup.ReadTarget();
			Rows.Add(new StatusRow("Bridge interface", target.Length > 0 ? target : "None chosen", target.Length > 0 ? StatusTone.Neutral : mode == AudioInputMode.Asio ? StatusTone.Warn : StatusTone.Neutral));
			var rsAsio = RsAsioIniSetup.GetState(game);
			Rows.Add(new StatusRow("RS_ASIO.ini", rsAsio switch
			{
				RsAsioIniSetup.State.Managed => "Set up for the bridge",
				RsAsioIniSetup.State.EditedByHand => "Edited by hand since the set-up",
				RsAsioIniSetup.State.NotSetUp => "Not set up for the bridge",
				_ => "No RS_ASIO",
			}, rsAsio == RsAsioIniSetup.State.Managed ? StatusTone.Good : StatusTone.Neutral));
			if (AsioProxySetup.ReadLastBindError(out string failed, out string reason))
				Rows.Add(new StatusRow("Last launch", (failed.Length > 0 ? failed : target) + " would not open (" + reason.TrimEnd('.') + ")", StatusTone.Bad));
			Rows.Add(new StatusRow("RSMods.ini", State.Ini!.Exists ? State.Ini.FilePath : "Not written yet (Rocksmith writes it on first start)"));
		}
		Rows.Add(new StatusRow("Mod manifest", State.Manifest.SourcePath != null
			? State.Manifest.Settings.Count + " Audio Bridge settings from " + State.Manifest.SourcePath
			: State.Manifest.LoadError ?? "Not found", State.Manifest.SourcePath != null ? StatusTone.Neutral : StatusTone.Warn));

		loading = true;
		Power = State.Ini != null && BridgeSettings.ReadPower(State.Ini);
		CanChangePower = State.HasGame && !State.IsGameRunning;
		loading = false;
	}

	// Bridge power: game-closed only, as in the 4.x Audio Bridge tab.
	partial void OnPowerChanged(bool value)
	{
		if (loading || AppState.ReadOnly || State.Ini is not RsModsIni ini) return;
		if (State.RefreshGameRunning()) { Say("Close Rocksmith before turning the audio bridge on or off.", StatusTone.Warn); Refresh(); return; }
		try
		{
			BridgeSettings.WritePower(ini, value);
			Say(value ? "The audio bridge is on from the next launch." : "The audio bridge is off from the next launch.", StatusTone.Good);
		}
		catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}

	[RelayCommand]
	private async Task ChooseGameFolder()
	{
		string? folder = await Dialogs.PickFolderAsync("Rocksmith 2014 folder", State.GameDirectory);
		if (folder == null) return;
		if (State.ChooseGameDirectory(folder)) Say("Rocksmith folder set.", StatusTone.Good);
		else Say("That folder has no cache.psarc, so it is not a Rocksmith 2014 folder.", StatusTone.Warn);
		Refresh();
	}
}

// ---- Input --------------------------------------------------------------------------------------------------------
public sealed partial class InputViewModel : PageViewModel
{
	public InputViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		Conditioner = new SettingsGroupViewModel(state, "Guitar input (Input conditioner)", state.Manifest.ForMod("InputConditioner"));
	}

	public override string Title => "Input";
	public override string Glyph => "";
	public override string Lead => "How the guitar reaches Rocksmith: the Real Tone Cable, or an audio interface through RS_ASIO and the bridge's ASIO driver.";

	public SettingsGroupViewModel Conditioner { get; }

	[ObservableProperty] private bool _isCable;
	[ObservableProperty] private bool _isAsio;
	[ObservableProperty] private bool _canSwitch;
	[ObservableProperty] private string _modeNote = "";
	[ObservableProperty] private string _rocksmithDetail = "";
	[ObservableProperty] private StatusTone _rocksmithTone;
	[ObservableProperty] private bool _rocksmithNeedsFix;
	[ObservableProperty] private string _recommended = "";

	private AudioInputMode mode = AudioInputMode.Unavailable;

	public override void Refresh()
	{
		State.RefreshGameRunning();
		if (State.GameDirectory is not string game) { CanSwitch = false; ModeNote = "Pick the Rocksmith folder on the Overview page."; return; }
		try { mode = new AudioInputModeConfiguration(game, () => State.IsGameRunning).ReadMode(); ModeNote = ""; }
		catch (Exception error) { mode = AudioInputMode.Unavailable; ModeNote = error.Message; }
		IsAsio = mode == AudioInputMode.Asio;
		IsCable = !IsAsio;
		CanSwitch = mode != AudioInputMode.Unavailable && !State.IsGameRunning;
		if (State.IsGameRunning) ModeNote = "Rocksmith is running. Close it to switch the input.";
		else if (mode == AudioInputMode.Unavailable && ModeNote.Length == 0) ModeNote = "RS_ASIO is not installed, so the guitar uses the Real Tone Cable and cannot be switched here.";

		var check = AuditChecks.CheckRocksmithSettings(game, IsAsio);
		RocksmithDetail = check.Detail;
		RocksmithTone = ToTone(check.State);
		RocksmithNeedsFix = check.State == AuditState.Problem;
		Recommended = AuditChecks.RecommendedText(IsAsio);
		Conditioner.Load();
	}

	// Renames avrt.dll / RS_ASIO.dll (to and from .disabled); refused while the game runs.
	[RelayCommand]
	private Task UseCable() => Switch(false);

	[RelayCommand]
	private Task UseAsio() => Switch(true);

	private async Task Switch(bool asio)
	{
		if (State.GameDirectory is not string game) return;
		if ((mode == AudioInputMode.Asio) == asio) { Refresh(); return; }
		await RunAsync(() =>
		{
			new AudioInputModeConfiguration(game).SetAsioEnabled(asio);
			return asio ? "The guitar input is now your ASIO interface. It applies when Rocksmith starts."
				: "The guitar input is now the Real Tone Cable. It applies when Rocksmith starts.";
		});
	}

	// Cable mode needs Win32UltraLowLatencyMode=1 (ASIO also ExclusiveMode=1). Dated backup first, as the Audit's Fix.
	[RelayCommand]
	private async Task UseRecommended()
	{
		if (State.GameDirectory is not string game) return;
		bool asio = IsAsio;
		await RunAsync(() =>
		{
			AuditChecks.FixRocksmithSettings(game, asio);
			return "Rocksmith.ini now has the recommended audio settings. A backup is in " + RsAsioIniSetup.BackupFolder + ".";
		});
	}
}

// ---- Output / ASIO ------------------------------------------------------------------------------------------------
public sealed partial class OutputViewModel : PageViewModel
{
	public const string SameAsOutput = "Same as output";

	public OutputViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		Routing = new SettingsGroupViewModel(state, "Output and Rocksmith's noise gate", state.Manifest.ForMod("AudioBridgeRouting"));
	}

	public override string Title => "Output / ASIO";
	public override string Glyph => "";
	public override string Lead => "The interface the bridge's ASIO driver opens for the guitar and the game's sound. The list leaves out the bridge's own driver.";

	public SettingsGroupViewModel Routing { get; }
	public ObservableCollection<string> Drivers { get; } = new();
	public ObservableCollection<string> InputDrivers { get; } = new();

	[ObservableProperty] private string? _selectedDriver;
	[ObservableProperty] private string? _selectedInputDriver;
	[ObservableProperty] private bool _pickersEnabled;
	[ObservableProperty] private bool _isAsioMode;
	[ObservableProperty] private string _driverStatus = "";
	[ObservableProperty] private StatusTone _driverTone;
	[ObservableProperty] private string _registration = "";
	[ObservableProperty] private StatusTone _registrationTone;
	[ObservableProperty] private string _suggestedTarget = "";
	[ObservableProperty] private bool _hasSuggestion;
	private bool syncing;

	private string ProxyDllPath => Path.Combine(State.GameDirectory ?? "", "RocksmithAudioBridgeAsio.dll");

	public override void Refresh()
	{
		State.RefreshGameRunning();
		if (State.GameDirectory is not string game) { PickersEnabled = false; DriverStatus = "Pick the Rocksmith folder on the Overview page."; return; }
		AudioInputMode mode;
		try { mode = new AudioInputModeConfiguration(game, () => State.IsGameRunning).ReadMode(); } catch { mode = AudioInputMode.Unavailable; }
		IsAsioMode = mode == AudioInputMode.Asio;

		bool current = AsioProxySetup.IsProxyRegistrationCurrent(ProxyDllPath);
		Registration = current ? "Registered with Windows and pointing at this game folder's driver."
			: !File.Exists(ProxyDllPath) ? "RocksmithAudioBridgeAsio.dll is missing from the game folder. Reinstall Rocksmith Audio Bridge."
			: AsioProxySetup.IsProxyRegistered() ? "Registered, but to another file (" + AsioProxySetup.RegisteredProxyPath() + "). Repair it below."
			: "Not registered, so RS_ASIO cannot find it. Repair it below.";
		RegistrationTone = current ? StatusTone.Good : StatusTone.Bad;

		string target = AsioProxySetup.ReadTarget();
		var drivers = AsioProxySetup.ListBridgeDrivers();
		syncing = true;
		try
		{
			Drivers.Clear();
			foreach (string driver in drivers) Drivers.Add(driver);
			SelectedDriver = drivers.FirstOrDefault(d => string.Equals(d, target, StringComparison.OrdinalIgnoreCase));
			InputDrivers.Clear();
			InputDrivers.Add(SameAsOutput);
			foreach (string driver in drivers) InputDrivers.Add(driver);
			string input = AsioProxySetup.ReadInputTarget();
			SelectedInputDriver = string.Equals(input, target, StringComparison.OrdinalIgnoreCase) ? SameAsOutput
				: drivers.FirstOrDefault(d => string.Equals(d, input, StringComparison.OrdinalIgnoreCase)) ?? SameAsOutput;
			PickersEnabled = current && drivers.Count > 0;
		}
		finally { syncing = false; }

		// The 4.x settings app filled the target on every refresh (AsioProxySetup.FillTarget). Here it is offered as a
		// one-click suggestion instead, so opening the app never writes the registry.
		string? suggestion = drivers.Exists(d => string.Equals(d, target, StringComparison.OrdinalIgnoreCase)) ? null
			: AsioProxySetup.ResolveBridgeInterface(drivers, target, AsioProxySetup.ReadOutputDriver(game));
		SuggestedTarget = suggestion ?? "";
		HasSuggestion = current && !string.IsNullOrEmpty(suggestion);

		bool linked = string.Equals(AsioProxySetup.ReadOutputDriver(game), AsioProxySetup.ProxyName, StringComparison.OrdinalIgnoreCase);
		if (!current) { DriverStatus = "Not set up correctly, so Rocksmith would find no audio device under ASIO."; DriverTone = StatusTone.Warn; }
		else if (string.IsNullOrEmpty(target)) { DriverStatus = drivers.Count == 0 ? "No ASIO driver is installed. Install your interface's ASIO driver." : "No interface is chosen yet. Pick yours above."; DriverTone = StatusTone.Warn; }
		else if (linked && AsioProxySetup.ReadLastBindError(out string failedDriver, out string reason))
		{
			if (failedDriver.Length == 0) failedDriver = target;
			DriverStatus = "Last launch: " + failedDriver + " would not open (" + reason.TrimEnd('.') + "). Guitar was silent. Plug it in or close other apps using it (its control app, a DAW, OBS), then pick it again above or on the in-game Output page. No restart needed.";
			DriverTone = StatusTone.Warn;
		}
		else if (linked) { DriverStatus = "Ready. The bridge opens this interface when Rocksmith starts."; DriverTone = StatusTone.Good; }
		else { DriverStatus = "Installed, but RS_ASIO.ini does not use it yet. Set up for me on the Audit page points RS_ASIO at it."; DriverTone = StatusTone.Neutral; }

		Routing.Load();
	}

	// HKCU AsioProxy\Target; with Rocksmith running, switched live through engine op 25 (as the 4.x tab and the overlay).
	partial void OnSelectedDriverChanged(string? value)
	{
		if (syncing || AppState.ReadOnly || string.IsNullOrEmpty(value)) return;
		_ = SwitchInterface(value);
	}

	partial void OnSelectedInputDriverChanged(string? value)
	{
		if (syncing || AppState.ReadOnly || value == null) return;
		_ = SwitchInputInterface(value == SameAsOutput ? "" : value);
	}

	private async Task SwitchInterface(string driver)
	{
		string previous = AsioProxySetup.ReadTarget();
		if (string.Equals(driver, previous, StringComparison.OrdinalIgnoreCase)) return;
		var client = AudioControlClient.FindGame();
		if (client == null)
		{
			AsioProxySetup.SelectTarget(driver);
			AsioProxySetup.SetPreferRealOutput(true);
			Say("Saved. Rocksmith opens " + driver + " next time it starts.", StatusTone.Good);
			Refresh();
			return;
		}
		Say("Switching to " + driver + "...");
		try
		{
			await client.SendAsync(AudioControlClient.OpSwitchInterface, driver);
			AsioProxySetup.SelectTarget(driver);
			AsioProxySetup.SetPreferRealOutput(true);
			Say("Now on " + driver + ". It is remembered for the next launch.", StatusTone.Good);
		}
		catch (Exception error)
		{
			bool back = false;
			if (!string.IsNullOrEmpty(previous))
				try { await client.SendAsync(AudioControlClient.OpSwitchInterface, previous); back = true; } catch { }
			Say(driver + " would not open (" + error.Message.TrimEnd('.') + ")." + (back ? " Back on " + previous + "." : ""), StatusTone.Warn);
		}
		Refresh();
	}

	private async Task SwitchInputInterface(string driver)
	{
		// A picker re-selecting what is already saved is not a change (refreshes must never write the registry).
		string saved = AsioProxySetup.ReadInputTarget();
		if (string.Equals(saved, driver, StringComparison.OrdinalIgnoreCase)
			|| (driver.Length == 0 && string.Equals(saved, AsioProxySetup.ReadTarget(), StringComparison.OrdinalIgnoreCase))) return;
		AsioProxySetup.SelectInputTarget(driver);
		string name = driver.Length == 0 ? "the output interface" : driver;
		var client = AudioControlClient.FindGame();
		if (client == null) { Say("Saved. The guitar comes from " + name + " next time Rocksmith starts.", StatusTone.Good); return; }
		try
		{
			await client.SendAsync(AudioControlClient.OpSwitchInputInterface, driver);
			Say("The guitar now comes from " + name + ". If it could not open, the in-game Output page says why.", StatusTone.Good);
		}
		catch (Exception error) { Say("Saved, but Rocksmith did not take it now (" + error.Message.TrimEnd('.') + "). It applies next launch.", StatusTone.Warn); }
	}

	[RelayCommand]
	private void UseSuggestion()
	{
		if (SuggestedTarget.Length == 0) return;
		AsioProxySetup.SelectTarget(SuggestedTarget);
		AsioProxySetup.SetPreferRealOutput(true);
		Say("The bridge now opens " + SuggestedTarget + ".", StatusTone.Good);
		Refresh();
	}

	/// <summary>
	/// The one admin action: registers the driver (and its 64-bit guest, and UsbDk) machine-wide. Runs this exe again
	/// as a separate elevated child with the installer verb (--install-audio-bridge-driver "&lt;game&gt;" --elevated),
	/// so this window itself never runs elevated and nothing elevates at start-up. One UAC prompt.
	/// </summary>
	[RelayCommand]
	private async Task RepairDriver()
	{
		if (State.GameDirectory is not string game) return;
		if (!await Dialogs.ConfirmAsync("Repair the bridge driver",
			"Windows will ask for administrator approval once, to register Rocksmith Audio Bridge ASIO for every ASIO program (and install UsbDk for the direct USB cable read if it is missing). Nothing else is changed.",
			"Continue")) return;
		await RunAsync(() =>
		{
			var info = new ProcessStartInfo(Environment.ProcessPath!)
			{
				UseShellExecute = true,
				Verb = "runas",
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			info.ArgumentList.Add(AsioProxySetup.InstallArgument);
			info.ArgumentList.Add(game);
			info.ArgumentList.Add("--elevated");
			int exit;
			try
			{
				using var process = Process.Start(info)!;
				process.WaitForExit();
				exit = process.ExitCode;
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223)
			{
				throw new OperationCanceledException("Registering the driver needs a one-time Windows admin approval, and the prompt was declined.");
			}
			return exit switch
			{
				0 => "The bridge driver is registered.",
				AsioProxySetup.EXIT_USBDK_RESTART => "The bridge driver is registered. UsbDk was installed and needs a Windows restart before the direct USB cable read works.",
				AsioProxySetup.EXIT_USBDK_FAILED => "The bridge driver is registered. UsbDk was not installed; only the direct USB cable read is unavailable. See " + UsbDkSetup.LogPath(game) + ".",
				2 => throw new OperationCanceledException("The admin prompt was declined."),
				_ => throw new InvalidOperationException("Registering the driver failed (exit code " + exit + "). See " + Path.Combine(AppContext.BaseDirectory, "AudioBridgeDriverInstall.log") + "."),
			};
		}, "Waiting for Windows' admin approval...");
	}
}

// ---- Manifest-only pages ------------------------------------------------------------------------------------------
public sealed class DropPedalViewModel : PageViewModel
{
	public DropPedalViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		Settings = new SettingsGroupViewModel(state, "Drop Pedal and Speaker Mode", state.Manifest.ForMod("DropPedal"));
	}

	public override string Title => "Drop Pedal";
	public override string Glyph => "";
	public override string Lead => "Play songs in other tunings without retuning: the pedal shifts your guitar to the song. Speaker Mode and the shift itself are switched in game (overlay or hotkey).";
	public SettingsGroupViewModel Settings { get; }
	public override void Refresh() => Settings.Load();
}

public sealed partial class NoteByNoteViewModel : PageViewModel
{
	public NoteByNoteViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		Settings = new SettingsGroupViewModel(state, "Note by Note readout", state.Manifest.ForMod("NoteByNote"));
	}

	public override string Title => "Note by Note";
	public override string Glyph => "";
	public override string Lead => "Riff Repeater waits for each note. It is switched on in the game's Riff Repeater, Advanced Settings, which needs the NOTE BY NOTE row added to the game's menu (cache.psarc).";
	public SettingsGroupViewModel Settings { get; }

	[ObservableProperty] private string _menuState = "";
	[ObservableProperty] private StatusTone _menuTone;
	[ObservableProperty] private bool _canChangeMenu;

	public override void Refresh()
	{
		State.RefreshGameRunning();
		if (State.GameDirectory is string game)
		{
			bool installedOnce = File.Exists(NoteByNoteMenuInstaller.BackupPath(game));
			if (MenuTone == StatusTone.Neutral || MenuState.Length == 0)
				MenuState = installedOnce ? "Installed at some point (its recovery copy of cache.psarc exists). Check reads the menu itself."
					: "Not installed by this tool yet. Check reads the menu itself.";
			CanChangeMenu = !State.IsGameRunning;
		}
		else CanChangeMenu = false;
		Settings.Load();
	}

	// Read-only: unpacks cache.psarc to a temp folder (a few seconds).
	[RelayCommand]
	private async Task CheckMenu()
	{
		if (State.GameDirectory is not string game) return;
		IsBusy = true;
		MenuState = "Reading the game's menu...";
		MenuTone = StatusTone.Neutral;
		try
		{
			bool installed = await Task.Run(() => NoteByNoteMenuInstaller.IsInstalled(game));
			MenuState = installed ? "NOTE BY NOTE and FLOW MODE are in Riff Repeater, Advanced Settings." : "Not in the Riff Repeater menu.";
			MenuTone = installed ? StatusTone.Good : StatusTone.Warn;
		}
		catch (Exception error) { MenuState = error.Message; MenuTone = StatusTone.Bad; }
		finally { IsBusy = false; }
	}

	[RelayCommand]
	private Task InstallMenu() => ChangeMenu(true);

	[RelayCommand]
	private Task UninstallMenu() => ChangeMenu(false);

	// Rewrites cache.psarc, touching only our two rows (NoteByNoteMenuInstaller.Apply); game closed.
	private async Task ChangeMenu(bool add)
	{
		if (State.GameDirectory is not string game) return;
		if (!await Dialogs.ConfirmAsync(add ? "Add Note by Note to the menu" : "Remove Note by Note from the menu",
			(add ? "Adds NOTE BY NOTE and FLOW MODE to Riff Repeater, Advanced Settings" : "Removes NOTE BY NOTE and FLOW MODE from Riff Repeater, Advanced Settings")
			+ " by rebuilding the game's cache.psarc. Everything else in it stays. Rocksmith must be closed.", add ? "Add" : "Remove")) return;
		MenuTone = StatusTone.Neutral;
		await RunAsync(() =>
		{
			NoteByNoteMenuInstaller.Apply(game, add);
			return (add ? "Added to" : "Removed from") + " Riff Repeater, Advanced Settings. Log: " + NoteByNoteMenuInstaller.LogPath;
		}, "Rebuilding cache.psarc...");
		MenuState = "";
	}
}

public sealed partial class RecorderViewModel : PageViewModel
{
	public RecorderViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		Settings = new SettingsGroupViewModel(state, "Hotkey", state.Manifest.ForMod("AudioRecorder"));
	}

	public override string Title => "Recorder";
	public override string Glyph => "";
	public override string Lead => "Takes from the overlay's Record button and the record hotkey, in one dated folder per game launch.";
	public SettingsGroupViewModel Settings { get; }
	public string[] Qualities => BridgeSettings.VideoQualities;

	[ObservableProperty] private string _folder = "";
	[ObservableProperty] private bool _video;
	[ObservableProperty] private bool _audioOnly;
	[ObservableProperty] private string? _quality;
	private bool loading;

	public override void Refresh()
	{
		if (State.Ini is not RsModsIni ini || State.GameDirectory is not string game) return;
		loading = true;
		Folder = BridgeSettings.ReadRecordingFolder(ini, game);
		Video = BridgeSettings.ReadPreferVideo(ini);
		AudioOnly = !Video;
		Quality = BridgeSettings.ReadVideoQuality(ini);
		loading = false;
		Settings.Load();
	}

	partial void OnVideoChanged(bool value)
	{
		if (loading || !value || State.Ini is not RsModsIni ini) return;
		Write(() => BridgeSettings.WritePreferVideo(ini, true));
		AudioOnly = false;
	}

	partial void OnAudioOnlyChanged(bool value)
	{
		if (loading || !value || State.Ini is not RsModsIni ini) return;
		Write(() => BridgeSettings.WritePreferVideo(ini, false));
		Video = false;
	}

	partial void OnQualityChanged(string? value)
	{
		if (loading || value == null || State.Ini is not RsModsIni ini) return;
		Write(() => BridgeSettings.WriteVideoQuality(ini, value));
	}

	private void Write(Action write)
	{
		if (AppState.ReadOnly) return;
		try { write(); Say("Saved.", StatusTone.Good); }
		catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}

	// A folder dialog cannot show over fullscreen Rocksmith, which is why this lives in the desktop app.
	[RelayCommand]
	private async Task ChangeFolder()
	{
		if (State.Ini is not RsModsIni ini) return;
		string? folder = await Dialogs.PickFolderAsync("Where should takes be saved?", Directory.Exists(Folder) ? Folder : null);
		if (folder == null) return;
		Write(() => BridgeSettings.WriteRecordingFolder(ini, folder));
		Refresh();
	}

	[RelayCommand]
	private void OpenRecordings()
	{
		try { OpenFolder(Folder); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}
}

public sealed partial class ToneDesignerViewModel(AppState state, IDialogs dialogs) : PageViewModel(state, dialogs)
{
	public override string Title => "Tone designer";
	public override string Glyph => "";
	public override string Lead => "The Tone designer (amps, pedals, captures and cabinets through the VST host) is played and edited in game, on the overlay's Guitar Tone page. Its files live in your user folders.";

	public static string StompboxFolder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "Stompbox");
	public static string BackupsFolder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Rocksmith Audio Bridge", "Tone designer backups");

	public ObservableCollection<StatusRow> Rows { get; } = new();
	public bool HasSettings => State.Manifest.ForMod("ToneDesigner").Any();

	public override void Refresh()
	{
		Rows.Clear();
		Rows.Add(new StatusRow("Tones, captures and plug-in set-up", StompboxFolder, Directory.Exists(StompboxFolder) ? StatusTone.Neutral : StatusTone.Warn));
		Rows.Add(new StatusRow("Setup exports (Export my setup)", BackupsFolder));
	}

	[RelayCommand]
	private void OpenStompbox()
	{
		try { OpenFolder(StompboxFolder); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}

	[RelayCommand]
	private void OpenBackups()
	{
		try { OpenFolder(BackupsFolder); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}
}

// ---- Audit --------------------------------------------------------------------------------------------------------
public sealed partial class AuditItemViewModel : ObservableObject
{
	public AuditItemViewModel(AuditCheck check, Func<AuditCheck, Task> fix)
	{
		Check = check;
		FixCommand = new AsyncRelayCommand(() => fix(check));
	}

	public AuditCheck Check { get; }
	public string Name => Check.Name;
	public string Detail => Check.Detail;
	public StatusTone Tone => Check.State switch
	{
		AuditState.Ok => StatusTone.Good,
		AuditState.Warning => StatusTone.Warn,
		AuditState.Problem => StatusTone.Bad,
		_ => StatusTone.Neutral,
	};
	public string StateText => Check.State switch
	{
		AuditState.Ok => "OK",
		AuditState.Warning => "Check",
		AuditState.Problem => "Problem",
		_ => "Not used",
	};
	public bool HasFix => Check.FixLabel != null;
	public string FixLabel => Check.FixLabel ?? "";
	public IAsyncRelayCommand FixCommand { get; }
}

public sealed partial class AuditViewModel(AppState state, IDialogs dialogs) : PageViewModel(state, dialogs)
{
	public override string Title => "Audit";
	public override string Glyph => "";
	public override string Lead => "Basic checks for the guitar input that is selected, because the Real Tone Cable and an ASIO interface fail in different ways. Only what stops a launch from having sound.";

	public ObservableCollection<AuditItemViewModel> Items { get; } = new();

	[ObservableProperty] private string _modeName = "";
	[ObservableProperty] private bool _canRestore;
	[ObservableProperty] private string _recommended = "";

	public override void Refresh()
	{
		Items.Clear();
		if (State.GameDirectory is not string game) { ModeName = "No Rocksmith folder"; return; }
		var mode = AuditChecks.Mode(game);
		bool asio = mode == AudioInputMode.Asio;
		ModeName = asio ? "ASIO interface" : "Real Tone Cable";
		Recommended = AuditChecks.RecommendedText(asio);

		// The same checks per mode as the 4.x Audit tab (GUI/Features/Audit/UI.AuditTab.cs AuditChecksForMode).
		var rocksmith = AuditChecks.CheckRocksmithSettings(game, asio);
		if (rocksmith.State == AuditState.Problem) rocksmith.FixLabel = "Use recommended";
		var checks = new List<AuditCheck> { rocksmith };
		if (asio)
		{
			checks.Add(AuditChecks.CheckRsAsioInstalled(game));
			var setUp = AuditChecks.CheckRsAsioSetUp(game);
			if (setUp.State is AuditState.Problem or AuditState.Warning) setUp.FixLabel = "Set up for me";
			checks.Add(setUp);
			checks.Add(AuditChecks.CheckBridgeInterface());
		}
		else
		{
			checks.Add(AuditChecks.CheckCable());
			checks.Add(AuditChecks.CheckMicrophoneAccess());
		}
		foreach (var check in checks) Items.Add(new AuditItemViewModel(check, Fix));
		CanRestore = asio && RsAsioIniSetup.CanRestore;
	}

	private async Task Fix(AuditCheck check)
	{
		if (State.GameDirectory is not string game) return;
		bool asio = AuditChecks.AsioMode(game);
		switch (check.FixLabel)
		{
			case "Use recommended":
				await RunAsync(() => { AuditChecks.FixRocksmithSettings(game, asio); return "Rocksmith.ini updated (a backup is in " + RsAsioIniSetup.BackupFolder + ")."; });
				break;
			case "Set up for me":
			{
				var plan = RsAsioIniSetup.Analyze(game);
				string notes = plan.Notes.Count > 0 ? "\n\n" + string.Join("\n", plan.Notes) : "";
				if (!await Dialogs.ConfirmAsync("Set up RS_ASIO for the bridge",
					"Points RS_ASIO.ini at Rocksmith Audio Bridge ASIO" + (plan.Target != null ? ", which opens " + plan.Target : "")
					+ ". Your current file is backed up first and Restore puts it back." + notes, "Set up")) return;
				await RunAsync(() => string.Join("\n", RsAsioIniSetup.SetUp(game)));
				break;
			}
			default:
				try { check.Fix?.Invoke(); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
				break;
		}
	}

	[RelayCommand]
	private async Task RestoreRsAsio()
	{
		if (State.GameDirectory is not string game) return;
		if (!await Dialogs.ConfirmAsync("Restore RS_ASIO.ini", "Puts your own RS_ASIO.ini back as it was before Set up for me. The bridge's interface and channel routes stay saved.", "Restore")) return;
		await RunAsync(() => { RsAsioIniSetup.Restore(game); return "Your original RS_ASIO.ini is back."; });
	}

	[RelayCommand]
	private void OpenBackups()
	{
		try { OpenFolder(RsAsioIniSetup.BackupFolder); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}
}

// ---- Settings (manifest-driven) -------------------------------------------------------------------------------------
public sealed class SettingsViewModel : PageViewModel
{
	public SettingsViewModel(AppState state, IDialogs dialogs) : base(state, dialogs)
	{
		foreach (var mod in state.Manifest.Settings.GroupBy(s => s.Mod))
			Groups.Add(new SettingsGroupViewModel(state, SettingsGroupViewModel.ModTitle(mod.Key), mod));
	}

	public override string Title => "Settings";
	public override string Glyph => "";
	public override string Lead => "Every Rocksmith Audio Bridge setting the game DLL declares in its mod manifest, saved to RSMods.ini as you change it. A running game reloads them at once.";

	public ObservableCollection<SettingsGroupViewModel> Groups { get; } = new();
	public string ManifestSource => State.Manifest.SourcePath ?? State.Manifest.LoadError ?? "";
	public bool HasNoManifest => Groups.Count == 0;

	public override void Refresh()
	{
		foreach (var group in Groups) group.Load();
	}
}

// ---- About / Bug report -------------------------------------------------------------------------------------------
public sealed partial class AboutViewModel(AppState state, IDialogs dialogs) : PageViewModel(state, dialogs)
{
	public override string Title => "About";
	public override string Glyph => "";
	public override string Lead => "Rocksmith Audio Bridge runs inside RSMods' mod framework. This app sets up our part; RSMods' own configurator handles RSMods' features.";

	public string Version => ProductInfo.DISPLAY_NAME;
	public string Email => BugReport.DevEmail;

	[ObservableProperty] private string _reportPath = "";
	[ObservableProperty] private bool _hasReport;
	private BugReport.Result? report;

	// Zips the last two launches' logs and settings (user name scrubbed) and opens a pre-filled email. The game must
	// be closed: the mod's log is buffered while it runs, so the newest lines may not be on disk yet.
	[RelayCommand]
	private async Task MakeReport()
	{
		if (State.GameDirectory is not string game) { Say("The Rocksmith folder was not found.", StatusTone.Warn); return; }
		if (State.RefreshGameRunning()) { Say("Close Rocksmith first: its log is still being written.", StatusTone.Warn); return; }
		await RunAsync(() =>
		{
			report = BugReport.Build(game);
			string note = report.LoggingWasOff ? " Logging was off; it is on from the next launch, so reproduce the problem once and make another report." : "";
			bool mailed = BugReport.OpenEmail(report);
			BugReport.ShowInExplorer(report.ZipPath);
			return (mailed ? "Report made and an email opened. Drag the selected zip into it." : "Report made. Email it to " + BugReport.DevEmail + ".") + note;
		}, "Collecting logs...");
		if (report != null) { ReportPath = report.ZipPath; HasReport = true; }
	}

	[RelayCommand]
	private void OpenReports()
	{
		try { OpenFolder(BugReport.ReportFolder); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
	}

	public override void Refresh() { }
}
