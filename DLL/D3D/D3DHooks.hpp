#pragma once
#include "../Mods/ExtendedRangeMode.hpp"

namespace D3DHooks {
	struct PhysicalMarkerCaptureState
	{
		unsigned int armGeneration = 0;
		unsigned long long callbackCount = 0;
		unsigned long long armCallbackBase = 0;
		unsigned long long matchedDrawCount = 0;
		unsigned long long armMatchedDrawBase = 0;
		unsigned long long publishedDrawCount = 0;
		unsigned long long armPublishedDrawBase = 0;
		bool guideSuppressionEnabled = false;
		unsigned long long guideConsideredDrawCount = 0;
		unsigned long long guideSuppressedDrawCount = 0;
		bool isArmed = false;
		bool isActive = false;
	};

	inline UINT StartRegister;
	inline UINT VectorCount;

	inline UINT Stride;
	inline D3DVERTEXBUFFER_DESC vdesc;

	inline IDirect3DVertexDeclaration9* pDecl;
	inline D3DVERTEXELEMENT9 decl[MAXD3DDECLLENGTH];
	inline UINT NumElements;

	inline IDirect3DVertexShader9* vShader;
	inline UINT vSize;

	inline IDirect3DPixelShader9* pShader;
	inline UINT pSize;

	inline IDirect3DTexture9* texture;
	inline D3DSURFACE_DESC sDesc;
	inline D3DLOCKED_RECT pLockedRect;

	inline LPDIRECT3DVERTEXBUFFER9 Stream_Data;
	inline UINT Offset = 0;
	inline UINT vertexBufferSize;

	inline bool debug = true;

	inline bool regenerateUserDefinedTexture = false;
	
	inline HWND hThisWnd = NULL;
	inline WNDPROC oWndProc = NULL;

	inline bool cachedIsInSong = false;

	HRESULT APIENTRY Hook_SetVertexDeclaration(LPDIRECT3DDEVICE9 pDevice, IDirect3DVertexDeclaration9* pdecl);
	HRESULT APIENTRY Hook_DP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT StartIndex, UINT PrimCount);
	HRESULT APIENTRY Hook_SetVertexShaderConstantF(LPDIRECT3DDEVICE9 pDevice, UINT StartRegister, const float* pConstantData, UINT Vector4fCount);
	HRESULT APIENTRY Hook_SetVertexShader(LPDIRECT3DDEVICE9 pDevice, IDirect3DVertexShader9* veShader);
	HRESULT APIENTRY Hook_SetPixelShader(LPDIRECT3DDEVICE9 pDevice, IDirect3DPixelShader9* piShader);
	HRESULT APIENTRY Hook_SetStreamSource(LPDIRECT3DDEVICE9 pDevice, UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride);
	HRESULT APIENTRY Hook_Reset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
	HRESULT APIENTRY Hook_DIP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimCount);
	HRESULT APIENTRY Hook_DPUP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT PrimCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
	HRESULT APIENTRY Hook_DIPUP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT MinVertexIndex, UINT NumVertices, UINT PrimCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
	HRESULT APIENTRY Hook_EndScene(IDirect3DDevice9* pDevice);
	void ArmNoteByNoteRenderSnapshot();
	void ArmPhysicalMarkerDrawCapture();
	bool ShouldSuppressDrawBySignature(IDirect3DDevice9* device, UINT stride, UINT vertexCount, UINT primitiveCount);
	void SetPhysicalGuideSuppression(
		bool shouldEnable,
		uint32_t stride,
		uint32_t vertexCount,
		uint32_t primitiveCount);
	void FinishNoteByNoteRenderFrame();
	PhysicalMarkerCaptureState GetPhysicalMarkerCaptureState();

	// Stale fretboard marker quad filter (drops doubled markers): runtime toggle and
	// counters for the bridge.
	struct StaleMarkerFilterState
	{
		bool enabled = false;
		unsigned long long consideredQuads = 0;
		unsigned long long droppedQuads = 0;
		unsigned long long filteredDraws = 0;
		// Read-back locks taken with (lockNoWaits) and without (lockWaits) NOOVERWRITE.
		unsigned long long lockWaits = 0;
		unsigned long long lockNoWaits = 0;
	};
	void SetStaleMarkerFilterEnabled(bool shouldEnable);
	StaleMarkerFilterState GetStaleMarkerFilterState();

	// Frame-time telemetry: sampled at the EndScene seam, reported by the bridge 'status'
	// reply. averageMs is a ~60-frame exponential average; windowMaxMs the worst frame of
	// the last 120.
	struct FrameTimeStats
	{
		float averageMilliseconds = 0.0f;
		float windowMaxMilliseconds = 0.0f;
		unsigned long long frameCount = 0;
	};
	FrameTimeStats GetFrameTimeStats();

	// Host-drawn finger numerals: screen anchors collected from kept
	// members' marker draws (world translation in c0..c3, view-projection in
	// c4..c7), consumed by the overlay each frame. The game's own numeral glyphs
	// are screen-space and never repaint on frozen retargets, so the overlay
	// draws the SNG template's fingers at these anchors instead. Render thread only.
	void SetFingerNumeralsEnabled(bool shouldEnable);
	bool AreFingerNumeralsEnabled();
	// True when the game's finger ring for a kept chord member stood at rest this frame or the last.
	bool HasRestingFingerRing(int stringIndex, int fret);
	int GetFreshFingerAnchors(
		int (&strings)[8],
		int (&frets)[8],
		float (&screenX)[8],
		float (&screenY)[8]);

	// Shadow of vertex-shader constants c0..c7, kept by Hook_SetVertexShaderConstantF for
	// the research probe's screen-map capture (readable even if the device rejects
	// GetVertexShaderConstantF).
	extern float vertexShaderConstantShadow[8][4];
	void SetNoteByNoteNativeLifecycleTraceEnabled(bool shouldEnable);

	// Mod Settings
	inline bool resetHeadstockCache = true; // Do we want to reset the headstock cache? Triggers when opening tuning menu
	inline bool toggleSkyline = false; // Do we want to toggle the skyline right now? Triggers to false when turned on/ off
	inline int EnumSliderVal = 10000; // Sleep every X ms for enumeration (1000 ms = 1s)
	inline bool SkylineOff = false; // Is the skyline disabled right now? Toggles when skyline turns off (True - No Skyline, False - Skyline)
	inline bool DrawSkylineInMenu = false; // If the user is in "Song" mode of Toggle Skyline, should we draw the skyline in this menu (True - Skyline, False - No Skyline)
	
	inline bool GreenScreenWall = false; // If true, set the Greenscreen wall up. This helps call it in Lesson mode for No-Loft users (True - Black wall, False - Loft)
	inline bool RemoveLyrics = false; // If true, remove the lyrics from Learn A Song & Non-stop Play. (True - No Lyrics, False - Keep Lyrics)
	
	inline bool RemoveHeadstockInThisMenu = false; // If true, the headstock of the guitar / bass will be disabled in this menu. (True - No Headstock, False - Keep Headstock)
	inline bool showSongTimerOnScreen = false; // If true, the current song timer will be shown in the top-right of the screen. This will only work when inside a song. (True - Show, False - Hide)
	inline bool DiscoModeEnabled = false; // If true, we do the trippy effects that disco mode is known for (True - Disco, False - Normal).
	inline std::map<IDirect3DDevice9*, std::pair<DWORD, DWORD>> DiscoModeInitialSetting; // List of all the pDevices that have been affected by Disco Mode
	inline bool ToggleOffLoftWhenDoneWithMod = false; // If true, we save this until after the mod is done and re-enable it.
	inline bool PrideMode = false; // If true, the background will be cycle through colors.
	inline bool RainbowNotes = false; // If true, the notes will turn rainbow along with the stems
	inline bool AutomatedSelectedVolume = false; // If true, we will always show the selected volume.

	inline bool RecreateTextures = false; // User has triggered an update, so we need to re-create textures.
	inline bool RecreateTextureTimer = false; // If user spams recreating textures then we end up with a lot of memory usage. Limit how often we update textures.

	// Dev Functions
	inline bool startLogging = false; // Should we log what's happening in Hook_DIP? Logs to log.txt in your RS2014 directory

	// Misc
	inline bool setAllToNoteGradientTexture = false; // Should we override the 6-string note textures with the 7-string note textures?

	/// <summary>
	/// Convert time stored as a float of seconds, to h:m:s
	/// </summary>
	/// <param name="timeInSeconds"> - Float containing number of seconds elapsed.</param>
	/// <returns>std::string of time in "h:m:s" format.</returns>
	std::string ConvertFloatTimeToStringTime(float timeInSeconds);
	void RegenerateTwitchNoteColors(IDirect3DDevice9* pDevice);

	inline HWND cachedGameHwnd = nullptr;
	inline HWND GetGameWindow() {
		if (cachedGameHwnd) {
			return cachedGameHwnd;
		}

		cachedGameHwnd = FindWindowA(nullptr, "Rocksmith 2014");

		return cachedGameHwnd;
	}
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(WINAPI* tDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
inline tDrawIndexedPrimitive oDrawIndexedPrimitive;

typedef HRESULT(WINAPI* tBeginScene)(IDirect3DDevice9*);
inline tBeginScene oBeginScene;

typedef HRESULT(WINAPI* tDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
inline tDrawPrimitive oDrawPrimitive;

typedef HRESULT(WINAPI* tDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, CONST void*, UINT);
inline tDrawPrimitiveUP oDrawPrimitiveUP;

typedef HRESULT(WINAPI* tDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT);
inline tDrawIndexedPrimitiveUP oDrawIndexedPrimitiveUP;

typedef HRESULT(__stdcall* tEndScene)(IDirect3DDevice9*);
inline tEndScene oEndScene;

typedef HRESULT(APIENTRY* tReset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
inline tReset oReset;

typedef HRESULT(APIENTRY* tSetStreamSource)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
inline tSetStreamSource oSetStreamSource;

typedef HRESULT(APIENTRY* tSetVertexDeclaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
inline tSetVertexDeclaration oSetVertexDeclaration;

typedef HRESULT(APIENTRY* tSetVertexShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
inline tSetVertexShaderConstantF oSetVertexShaderConstantF;

typedef HRESULT(APIENTRY* tSetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
inline tSetVertexShader oSetVertexShader;

typedef HRESULT(APIENTRY* tSetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);;
inline tSetPixelShader oSetPixelShader;

typedef HRESULT(APIENTRY* tSetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
inline tSetTexture oSetTexture;

typedef HRESULT(APIENTRY* tPresent) (IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
inline tPresent oPresent;

/// <summary>
/// Dump stages 0-7 of texture. https://docs.microsoft.com/en-us/windows/win32/direct3d9/texture-blending
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="file_prefix"> - Prefix for file name. "_stage#.png" is appended to this to end up with the file name.</param>
inline void DumpTextureStages(IDirect3DDevice9* pDevice, std::string file_prefix) {
	LPDIRECT3DBASETEXTURE9 baseTexture;
	D3DCAPS9 deviceCaps;
	pDevice->GetDeviceCaps(&deviceCaps);

	file_prefix += "_stage";

	// The reason we get Device Caps is because some devices don't support 8 stages (0-7), so we have to account for that.

	for (DWORD i = 0; i < deviceCaps.MaxTextureBlendStages; i++) {
		std::string filename = std::string(file_prefix + std::to_string(i) + ".png").c_str();
		pDevice->GetTexture(i, &baseTexture);
		D3DXSaveTextureToFileA(filename.c_str(), D3DXIFF_PNG, baseTexture, NULL);
	}
}
