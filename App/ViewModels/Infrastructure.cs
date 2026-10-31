using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using RocksmithAudioBridge.Core;

namespace RocksmithAudioBridge.App.ViewModels;

public enum StatusTone { Neutral, Good, Warn, Bad }

/// <summary>One "label: value" line of a status card, coloured by tone.</summary>
public sealed record StatusRow(string Label, string Value, StatusTone Tone = StatusTone.Neutral);

/// <summary>What views ask of the window: dialogs that need a top-level.</summary>
public interface IDialogs
{
	Task<string?> PickFolderAsync(string title, string? startFolder);
	Task<bool> ConfirmAsync(string title, string message, string confirmText);
}

/// <summary>
/// The app's own preference file, %APPDATA%\RocksmithAudioBridge\App.ini ([App] GameFolder=). Written only when the
/// player picks a game folder; detection alone never writes it.
/// </summary>
public static class AppSettings
{
	public static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "App.ini");

	public static string? ReadGameFolder()
	{
		try { return File.Exists(FilePath) ? IniText.Load(FilePath).Get("App", "GameFolder") : null; }
		catch { return null; }
	}

	public static void WriteGameFolder(string folder)
	{
		Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
		var ini = IniText.Load(FilePath);
		ini.Set("App", "GameFolder", folder);
		ini.Save(FilePath);
	}
}

/// <summary>Shared state every page reads: the game folder, RSMods.ini, the manifest, whether Rocksmith runs.</summary>
public sealed partial class AppState : ObservableObject
{
	public AppState()
	{
		GameDirectory = GameFolder.Detect(AppSettings.ReadGameFolder());
		Manifest = OurModsManifest.Load();
		RefreshGameRunning();
	}

	[ObservableProperty]
	[NotifyPropertyChangedFor(nameof(HasGame), nameof(Ini), nameof(GameDirectoryText))]
	private string? _gameDirectory;

	[ObservableProperty]
	private bool _isGameRunning;

	public OurModsManifest Manifest { get; }

	/// <summary>Set by --screenshot: every write that a control's value change can trigger (settings, bridge power,
	/// recorder, interface pickers) is skipped, so a verification run can never change the game folder or registry.</summary>
	public static bool ReadOnly { get; set; }

	public bool HasGame => GameDirectory != null;

	public string GameDirectoryText => GameDirectory ?? "Not found. Pick the Rocksmith 2014 folder.";

	public RsModsIni? Ini => GameDirectory == null ? null : new RsModsIni(GameDirectory);

	public bool RefreshGameRunning()
	{
		bool running;
		try
		{
			running = GameDirectory != null
				? new AudioInputModeConfiguration(GameDirectory).IsGameRunning()
				: Process.GetProcessesByName("Rocksmith2014").Length > 0;
		}
		catch { running = false; }
		IsGameRunning = running;
		return running;
	}

	/// <summary>Sets and remembers the folder the player picked. False when it is not a Rocksmith folder.</summary>
	public bool ChooseGameDirectory(string folder)
	{
		if (!GameFolder.IsRocksmithFolder(folder)) return false;
		GameDirectory = Path.GetFullPath(folder);
		AppSettings.WriteGameFolder(GameDirectory);
		RefreshGameRunning();
		return true;
	}
}

/// <summary>A sidebar page. Pages rebuild what they show from disk / registry each time they are opened.</summary>
public abstract partial class PageViewModel : ObservableObject
{
	protected PageViewModel(AppState state, IDialogs dialogs)
	{
		State = state;
		Dialogs = dialogs;
	}

	public AppState State { get; }
	protected IDialogs Dialogs { get; }

	public abstract string Title { get; }
	/// <summary>Segoe Fluent Icons code point, the same glyph set the overlay's sidebar uses.</summary>
	public abstract string Glyph { get; }
	public abstract string Lead { get; }

	[ObservableProperty]
	private string _message = "";

	[ObservableProperty]
	private StatusTone _messageTone;

	[ObservableProperty]
	[NotifyPropertyChangedFor(nameof(IsIdle))]
	private bool _isBusy;

	public bool IsIdle => !IsBusy;

	public bool HasMessage => Message.Length > 0;

	partial void OnMessageChanged(string value) => OnPropertyChanged(nameof(HasMessage));

	public abstract void Refresh();

	protected void Say(string text, StatusTone tone = StatusTone.Neutral)
	{
		Message = text;
		MessageTone = tone;
	}

	/// <summary>Runs a player action off the UI thread with inline feedback (no message boxes), then refreshes.</summary>
	protected async Task RunAsync(Func<string> action, string? busyText = null)
	{
		if (IsBusy) return;
		IsBusy = true;
		if (busyText != null) Say(busyText);
		try
		{
			string done = await Task.Run(action);
			Say(done, StatusTone.Good);
		}
		catch (OperationCanceledException error) { Say(error.Message, StatusTone.Warn); }
		catch (Exception error) { Say(error.Message, StatusTone.Bad); }
		finally
		{
			IsBusy = false;
			try { Refresh(); } catch (Exception error) { Say(error.Message, StatusTone.Bad); }
		}
	}

	protected static void OpenFolder(string folder)
	{
		Directory.CreateDirectory(folder);
		Process.Start(new ProcessStartInfo("explorer.exe", "\"" + folder + "\"") { UseShellExecute = true });
	}

	protected static StatusTone ToTone(AuditState state) => state switch
	{
		AuditState.Ok => StatusTone.Good,
		AuditState.Warning => StatusTone.Warn,
		AuditState.Problem => StatusTone.Bad,
		_ => StatusTone.Neutral,
	};
}
