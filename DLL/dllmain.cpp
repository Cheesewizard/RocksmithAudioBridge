#include "stdafx.h"
#include "Main.hpp"
#include "Mods/NoteByNoteNativeScoring.hpp"
#include "Mods/NoteByNoteHostServices.hpp"
#include "Mods/RocksmithGate.hpp"
#include "Research/ResearchBridge.hpp"
#include "Audio/MlServiceLauncher.hpp"
#include "Audio/GameAudioRecorder.hpp"
#include "Audio/SharedOutput.hpp"
#include "Audio/TakeRecorder.hpp"
#include "Audio/AsioProxyRegistration.hpp"
#include "Audio/AudioLifecycleTrace.hpp"
#include "Audio/ExternalAmp.hpp"
#include "ProductVersion.hpp"
#include "OverlayInputCapture.hpp"

#include <io.h>
#include <share.h>

#if defined(_DEBUG) || defined(_WWISE_LOGS)
bool debug = true;
#else
bool debug = false;
#endif

#ifdef _WWISE_LOGS
bool wwiseLogging = true;
#else
bool wwiseLogging = false;
#endif

/// <summary>
/// Handle Force Enumeration
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI EnumerationThread() {
	while (!GameState::GameLoaded)
		Sleep(5000);

	int oldDLCCount = Enumeration::GetCurrentDLCCount();
	int newDLCCount = oldDLCCount;

	while (!GameState::GameClosing) {
		if (Settings::ReturnSettingValue("ForceReEnumerationEnabled") == "automatic") {
			oldDLCCount = newDLCCount;
			newDLCCount = Enumeration::GetCurrentDLCCount();

			if (oldDLCCount != newDLCCount)
				Enumeration::ForceEnumeration();
		}

		Sleep(Settings::GetModSetting("CheckForNewSongsInterval"));
	}

	return 0;
}

/// <summary>
/// Send Midi Data Async. Only really used in debug builds to test MIDI commands.
/// Secondary purpose of remaking D3D textures every 32 ticks (~ 1 second).
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI MidiThread() {
	// Initial some values.
	int currentCount = 0;

	while (!GameState::GameClosing) {
		// If this is the 32nd loop, remake the D3D textures.
		// This allows us to have real-time updates to textures.
		if (currentCount == 31) {
			currentCount = 0;
			D3DHooks::RecreateTextureTimer = true;
		}

		// If we have sent a Midi PC/CC value to this thread, send the Midi value.
		if (Midi::sendPC)
			Midi::SendProgramChange(Midi::dataToSendPC);

		if (Midi::sendCC)
			Midi::SendControlChange(Midi::dataToSendCC);

		Sleep(Midi::sleepFor);
		currentCount++;
	}

	return 0;
}

unsigned WINAPI RiffRepeaterThread() {
	// Wait for the game to enter the main menu before attempting to read the current song info, in order to prevent crashes
	while (!GameState::GameLoaded)
		Sleep(5000);

	std::string previousSongKey = "";

	// We can only user Riff Repeater while the game is open, so verify it's open. Runs every 100 ms.
	while (!GameState::GameClosing) {
		Sleep(100);

		const auto songKey = GameState::GetSongKey();
		if (songKey != previousSongKey) {
			previousSongKey = songKey;

			RiffRepeater::HandleSongChange(previousSongKey);
		}

		RiffRepeater::SaveSpeedToFileOnChange();

		// Retire Note by Note when the player leaves the song / Riff Repeater context, so it
		// does not persist onto results, song select, or the main menu (per-song lifecycle).
		NoteByNoteProbe::TickLifecycle();
	}

	return 0;
}

const bool ensureForcedTopMode = false;

/// <summary>
/// Handle Keypress Inputs. Used to toggle mods on / off. WARNING: RUNS (almost) EVERY FRAME
/// </summary>
/// <param name="hWnd"> - ID of Rocksmith</param>
/// <param name="msg"> - Reason Function was called. KEYUP = Keypress | COPYDATA = Settings Update | CLOSE = Game Closing.</param>
/// <param name="keyPressed"> - Virtual Key of the key pressed. Reference here: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes </param>
/// <param name="lParam"> - Data Sent</param>
/// <returns>Verification that message was sent.</returns>
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM keyPressed, LPARAM lParam) {
	// Overlay key picker: every key goes to the picker (none reaches the game or fires a hotkey) until one is
	// released. Closing the overlay cancels it.
	if (Keybindings::IsCapturingKey()) {
		if (!Menu::audioBridgeMenuEnabled) Keybindings::CancelKeyCapture();
		else if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR) {
			if (msg == WM_KEYUP || msg == WM_SYSKEYUP) Keybindings::CaptureKey(keyPressed);
			return true;
		}
	}
	// The overlay toggle is owned by RSModsPlus. Consume it before ImGui's general keyboard capture so
	// keyboard navigation inside the focused panel cannot prevent the same key from closing the panel.
	if (GameState::GameLoaded && keyPressed == VK_OEM_5 && (msg == WM_KEYDOWN || msg == WM_KEYUP)) {
		if (msg == WM_KEYUP) Keybindings::HandleKeyUp(keyPressed);
		return true;
	}
	if (GameState::GameLoaded && msg == WM_CHAR && keyPressed == 0x5C) // backslash '\'
		return true;

	// Keep ImGui's input state synchronized even while its panels are closed. It must see a button release or
	// focus-loss event that occurs after a panel closes; otherwise MouseDown remains latched and the next open
	// panel stops accepting input.
	if (Menu::ImGuiInit)
		ImGui_ImplWin32_WndProcHandler(hWnd, msg, keyPressed, lParam);

	// When a panel is open, swallow only what it owns: mouse events while the
	// cursor is over a panel, and keyboard navigation while the panel owns it. This lets sliders and
	// buttons work over exclusive-fullscreen Rocksmith without the clicks also driving the game, while leaving
	// gameplay input live when the cursor is outside the panel.
	if (Menu::menuEnabled || Menu::audioBridgeMenuEnabled) {
		const ImGuiIO& io = ImGui::GetIO();
		OverlayInputCapture::SetMouseCapture(io.WantCaptureMouse);
		const bool isMouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
		const bool isKey = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR);
		if ((io.WantCaptureMouse && isMouse) || (io.WantCaptureKeyboard && isKey))
			return true;
	}

	if (Settings::ReturnSettingValue("PreventMidSongPause") == "on" && D3DHooks::cachedIsInSong) {
		switch (msg) {
			case WM_NCACTIVATE:
			case WM_ACTIVATEAPP:
			case WM_ACTIVATE:
				return CallWindowProc(D3DHooks::oWndProc, hWnd, msg, TRUE, lParam);
			case WM_KILLFOCUS:
				return false;
		}
	}

	switch (msg) {
		case WM_SYSCOMMAND:
			// Makes ALT + ENTER cause F11 to be pressed.
			// This is mainly so a user can use a common shortcut, that works in most games now-a-days.
			if (keyPressed == SC_KEYMENU && lParam == VK_RETURN) {
				WndProc(hWnd, WM_KEYUP, VK_F11, 0);
				return true;
			}

			// Prevent a weird bug when trying to play Guitarcade or Score Attack with a key combination.
			if (keyPressed == SC_MOVE + 0x2 && GameState::Menus::IsInOnlineModes()) {
				return true;
			}

			break;
		case WM_KEYUP:
			// Note by Note: RIGHT ARROW while a note is frozen skips it (the controller ignores the press
			// unless it holds a note). Only while a song plays, not in the pause menu; the key still
			// reaches the game, which has no in-song use for it.
			if ((keyPressed == VK_RIGHT || keyPressed == VK_LEFT) && GameState::Menus::IsInLASPlayingModes()
				&& NoteByNoteProbe::IsAutomaticEnabled())
				NoteByNoteHostServices::QueueNoteNavigation(keyPressed == VK_RIGHT ? 1 : -1);
			Keybindings::HandleKeyUp(keyPressed);
			break;
		case WM_KEYDOWN:
			Keybindings::HandleKeyDown(keyPressed);
			break;
		case WM_CLOSE:
			GameState::GameClosing = true;
			// Finish recordings before the process goes away: stop a take that is still open (as the Record toggle
			// would, video included), then let the dry take's background normalization complete. Otherwise quitting
			// mid-take or just after Stop leaves an un-normalized or half-rewritten dry WAV. Normally nothing is pending.
			if (Audio::SharedOutput::HasTake()) Audio::Takes::Toggle(false);
			if (!Audio::GameAudioRecorder::WaitForPendingCloses(10000))
				LOG_WARNING("(AUDIO ROUTING) Game closing while a dry take was still being finalized" << std::endl);
			break;
		case WM_COPYDATA:
			Keybindings::UpdateSettingsOnGUIChange(lParam);
			break;

		default:
			POINT mPos;
			GetCursorPos(&mPos);
			ScreenToClient(hWnd, &mPos);
			ImGui::GetIO().MousePos.x = mPos.x;
			ImGui::GetIO().MousePos.y = mPos.y;
			break;
	}

	return CallWindowProc(D3DHooks::oWndProc, hWnd, msg, keyPressed, lParam);
}

void UpdateGameWindowStacking() {
	if (Settings::ReturnSettingValue("PreventMidSongPause") == "on") {
		bool actuallyInSong = GameState::IsInSong();

		if (ensureForcedTopMode) {
			static bool lastState = false;
			if (actuallyInSong != lastState) {
				HWND position = actuallyInSong ? HWND_TOPMOST : HWND_NOTOPMOST;

				SetWindowPos(D3DHooks::GetGameWindow(), position, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

				lastState = actuallyInSong;
			}
		}
	
		D3DHooks::cachedIsInSong = actuallyInSong;
	}
}

/// <summary>
/// Hook on game boot. Initialize all our own UI elements (text on screen, and ImGUI).
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <returns>HRESULT of the official EndScene</returns>
HRESULT APIENTRY D3DHooks::Hook_EndScene(IDirect3DDevice9* pDevice) {
	HRESULT originalReturn = oEndScene(pDevice);
	if (Menu::IsOverlayCall()) {
		return originalReturn;
	}
	D3DHooks::FinishNoteByNoteRenderFrame();

	// Re-assert the manual Rocksmith gate override (P1_NoiseFloor) every presented frame while it is on,
	// so it survives the game rewriting that RTPC on calibration and song transitions. No-op when off.
	RocksmithGate::ApplyPerFrame();

	// Don't draw our overlay onto a lost / not-yet-reset device (e.g. mid Alt+Tab out of exclusive fullscreen).
	if (FAILED(pDevice->TestCooperativeLevel())) {
		return originalReturn;
	}

	Menu::Init(pDevice, (LONG_PTR)WndProc);
	// Patches dinput8's shared mouse vtable so overlay clicks never reach the game. Once, on the render thread.
	static const bool inputCaptureInstalled = OverlayInputCapture::Install();
	(void)inputCaptureInstalled;
	Menu::RenderImGuiMenu();
	OverlayInputCapture::SetMouseCapture((Menu::menuEnabled || Menu::audioBridgeMenuEnabled) && ImGui::GetIO().WantCaptureMouse);
	Menu::UpdateStringTextures(pDevice);
	UpdateGameWindowStacking();
	GameOverlay::RenderOverlay(pDevice);
	D3DHooks::RegenerateTwitchNoteColors(pDevice);
	
	return originalReturn;
}

/// <summary>
/// Manage Queue of Twitch Effects.
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI HandleEffectQueueThread() {
	while (!GameState::GameClosing) {
		if (Twitch::effectQueue.empty() && GameState::IsInSong()) {
			Twitch::ParseEffectQueue();
		}

		Sleep(250);
	}
	return 0;
}

/// <summary>
/// Main Thread where we trigger the mods to startup.
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI MainThread() {
	// First: make sure RS_ASIO will resolve the bridge driver to the right DLL. RS_ASIO enumerates ~30 s into
	// boot; a stale entry there leaves the game with no audio device at all. See AsioProxyRegistration.hpp.
	AsioProxyRegistration::Heal();

	LOG_NOHEAD(ProductVersion::DISPLAY_NAME
		<< " (based on RSMods " << ProductVersion::UPSTREAM_VERSION << "). DEBUG: "
		<< std::boolalpha << debug << ". Wwise Logs: " << std::boolalpha << wwiseLogging << "."
		<< std::endl);

	GameLoopState loopState = {};

	Keybindings::InitializeCommands();
	ModManager::InitializeConfiguration();
	ModManager::InitializeMods(debug);
	// There is no desktop Audio Bridge window: the in-game overlay (\) does everything while playing, and
	// game-closed setup lives on the RSMods Rocksmith Audio Bridge page.
	ModManager::ApplyStartupMods();

	// Note by Note must initialize in every configuration: its Riff Repeater menu item
	// renders unconditionally, and without its backing state the rocker is dead. The probe
	// host lives inside ResearchBridge, so the bridge initializes in Release too.
	if (!ModManager::IsHookGroupSkipped("nbn")) {
		ResearchBridge::Initialize();
		NoteByNoteProbe::Initialize();
		NoteByNoteMenu::Initialize();
		NoteByNoteHudLabel::Initialize();
	}

	// The FretNet ML string/fret companion (bound to this game's lifetime) is what Note-by-Note reads for
	// its ML "stuck hold" rescue. It is a separate process with the model loaded, so it starts only the first
	// time Note by Note is on or the Riff Repeater menus are open (so it is warm before the first note),
	// then stays up for the session. The reader reconnects on its own once the service appears.
	bool mlServiceWanted = false;

	while (!GameState::GameClosing) {
		Sleep(250);
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
		Audio::LifecycleTrace::Poll();
#endif

		if (GameState::GameLoaded) {
			ModManager::HandlePostGameLoadedMods(loopState);
		}
		else {
			ModManager::UpdateGameLoadingState(loopState);
		}

		// currentMenu is written by the ModManager calls above, on this thread.
		if (!mlServiceWanted && GameState::GameLoaded
			&& (NoteByNoteProbe::IsAutomaticEnabledFast() || GameState::Menus::IsInRiffRepeaterMenus()))
			mlServiceWanted = true;
		MlServiceLauncher::Poll(mlServiceWanted);
	}

	MlServiceLauncher::Shutdown();
	ResearchBridge::Shutdown();
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
	Audio::LifecycleTrace::Poll();
#endif

	return 0;
}

/// <summary>
/// Unhook all threads to take advantage of multi-threading.
/// </summary>
void Initialize() {
	LogSettings::startupTime = clock();

	Wwise::Exports::Initialize();
	// OverlayInputCapture::Install() runs on the first EndScene instead: it creates a DirectInput object, which
	// must not happen here under the loader lock.

	// Read before any thread is spawned. Every mod thread reads these maps, so
	// rebuilding them later frees the strings a reader is still holding.
	Settings::ReadKeyBinds();
	Settings::ReadModSettings();

	std::thread(MainThread).detach(); // Mod Toggle based on menus
	std::thread(EnumerationThread).detach(); // Force Enumeration
	std::thread(HandleEffectQueueThread).detach(); // Twitch Effects
	std::thread(MidiThread).detach(); // MIDI Auto Tuning / True Tuning
	std::thread(RiffRepeaterThread).detach(); // RR Speed Above 100% Log
	ExternalAmp::Start(); // External amp: mutes the game's guitar while an amp sim plays through the bridge
}

void SetupLogging() {
	// Opt-in: only log to file if RSMods_debug.txt already exists (same as before).
	const bool debugLogPresent = std::ifstream("RSMods_debug.txt").good();

	// Keep the previous launch's evidence. The game truncates audiodump.txt when its audio
	// initialises (after this DLL loads) and RSMods_debug.txt is truncated below, which would
	// otherwise erase the log that explains a "no sound / no cable" launch.
	if (debugLogPresent) CopyFileA("RSMods_debug.txt", "RSMods_debug.previous.txt", FALSE);
	CopyFileA("audiodump.txt", "audiodump.previous.txt", FALSE);

	if (debug) {
		AllocConsole();

		// Quick-edit mode freezes the WHOLE GAME on a stray click: selecting text
		// in the console blocks WriteFile on stdout, and the log writes are
		// synchronous on the main thread, so the game hangs until the selection
		// is cleared. Selection stays available through right-click > Mark.
		HANDLE consoleInput = GetStdHandle(STD_INPUT_HANDLE);
		DWORD consoleMode = 0;
		if (consoleInput != INVALID_HANDLE_VALUE && GetConsoleMode(consoleInput, &consoleMode)) {
			SetConsoleMode(consoleInput, (consoleMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
		}

		// Connect stdin, stdout to the debug console.
		FILE* streamRead = nullptr;
		FILE* streamConsole = nullptr;
		freopen_s(&streamRead, "CONIN$", "r", stdin);
		freopen_s(&streamConsole, "CONOUT$", "w", stdout);
	}

	// Create log file to both help with debugging release builds,
	// and allow the user to examine their debug logs after a crash.
	if (debugLogPresent) {
		// freopen_s / fopen_s open with exclusive (no share) mode, which blocks
		// external tools from reading the log while the game is running.
		// Open with _SH_DENYWR so others can read; deny concurrent writers.
		// Mode "w" truncates so we start clean each launch (same as before).
		FILE* debugLog = _fsopen("RSMods_debug.txt", "w", _SH_DENYWR);
		if (debugLog) {
			// Point stderr's fd at the share-read handle. Logger writes via std::cerr.
			if (_dup2(_fileno(debugLog), _fileno(stderr)) == 0) {
				// Unbuffered so external readers see new lines promptly.
				setvbuf(stderr, nullptr, _IONBF, 0);
			}
			// Keep debugLog open for process lifetime (handle must stay valid).
		}
	}
}

/// <summary>
/// Hook into the game for us to run our own code. **DISPLAYS DEBUG CONSOLE ON DEBUG BUILD**
/// </summary>
/// <param name="hModule"></param>
/// <param name="dwReason"></param>
/// <param name="lpReserved"></param>
/// <returns>Always returns TRUE</returns>
BOOL APIENTRY DllMain(HMODULE hModule, uint32_t dwReason, LPVOID lpReserved) {
	switch (dwReason) {
		case DLL_PROCESS_ATTACH:
			SetupLogging();
			DisableThreadLibraryCalls(hModule); // Disables the DLL_THREAD_ATTACH and DLL_THREAD_DETACH notifications. | https://docs.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-disablethreadlibrarycalls
			Proxy::Init(); // Proxy all real XInput commands to the actual xinput1_3.dll.
			Initialize(); // Inject our mod code.
			return TRUE;
		case DLL_PROCESS_DETACH:
			OverlayInputCapture::Shutdown();
			Proxy::Shutdown(); // Kill Proxy to xinput1_3.dll

			if (Menu::ImGuiInit)
			{
				ImGui_ImplWin32_Shutdown();
				ImGui_ImplDX9_Shutdown();
				ImGui::DestroyContext();
			}
			return TRUE;
		default:
			// Should never happen, but handle gracefully
			break;
	}
	
	return TRUE;
}
