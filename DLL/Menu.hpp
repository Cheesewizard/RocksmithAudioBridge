#pragma once
namespace Menu
{
	inline bool ImGuiInit = false;
	inline bool menuEnabled = false; // Do we show the user the ImGUI settings menu?
	inline bool audioBridgeMenuEnabled = false; // Do we show the in-game Audio Bridge control panel?
	inline bool enableColorBlindCheckboxGUI = false; // Do we allow the user to change Colorblind mode in the imGUI menu?

	void Init(IDirect3DDevice9* pDevice, LONG_PTR WndProc);
	void AddMidiMenu();
	void AddMicrophonesMenu();
	void AddVoicelinesMenu();
	void AddCalibrationMenu();
	bool IsOverlayCall();
	void UpdateStringTextures(IDirect3DDevice9* pDevice);
	void RenderImGuiMenu();
	void CreateTextures(IDirect3DDevice9* pDevice);
};

