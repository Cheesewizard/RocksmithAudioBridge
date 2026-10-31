using System.Collections.ObjectModel;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace RocksmithAudioBridge.App.ViewModels;

/// <summary>The sidebar and the page shown beside it, like the in-game overlay's shell.</summary>
public sealed partial class MainWindowViewModel : ObservableObject
{
	private readonly DispatcherTimer gameWatch;

	public MainWindowViewModel(AppState state, IDialogs dialogs)
	{
		State = state;
		Pages = new ObservableCollection<PageViewModel>
		{
			new OverviewViewModel(state, dialogs),
			new InputViewModel(state, dialogs),
			new OutputViewModel(state, dialogs),
			new DropPedalViewModel(state, dialogs),
			new NoteByNoteViewModel(state, dialogs),
			new RecorderViewModel(state, dialogs),
			new ToneDesignerViewModel(state, dialogs),
			new AuditViewModel(state, dialogs),
			new SettingsViewModel(state, dialogs),
			new AboutViewModel(state, dialogs),
		};
		SelectedPage = Pages[0];

		// Whether Rocksmith runs decides what can be changed; checked every few seconds (a process list, no file I/O).
		gameWatch = new DispatcherTimer(TimeSpan.FromSeconds(3), DispatcherPriority.Background, (_, _) =>
		{
			bool was = State.IsGameRunning;
			if (State.RefreshGameRunning() != was) SafeRefresh(SelectedPage);
		});
		gameWatch.Start();
	}

	public AppState State { get; }
	public ObservableCollection<PageViewModel> Pages { get; }

	[ObservableProperty]
	private PageViewModel? _selectedPage;

	public string GameStatus => State.IsGameRunning ? "Rocksmith is running" : "Rocksmith is closed";

	partial void OnSelectedPageChanged(PageViewModel? value) => SafeRefresh(value);

	private void SafeRefresh(PageViewModel? page)
	{
		OnPropertyChanged(nameof(GameStatus));
		if (page == null) return;
		try { page.Refresh(); }
		catch (Exception error) { page.Message = "Could not read the current state: " + error.Message; page.MessageTone = StatusTone.Bad; }
	}
}
