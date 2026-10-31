#pragma once

#include "Framework/AudioInput.hpp"

#include <array>
#include <atomic>
#include <cstdint>

// Player 1's input stage meters, ported from 4.x's InputCaptureHook (stage meters 1-3 and PollInputStageMeter).
//
// Why: the Input page's level meter and the diagnostics HUD's SIGNAL row read CableInput's tap figures
// (ReportTapPacket, ReportPlayerTwoPacket), which 4.x's capture hook fed and nothing in the Mod port did, so both sat
// empty. The "(INPUT STAGES)" log line brackets the chain, so a dead stretch says WHERE the signal stopped: the device
// (bridge driver), RS_ASIO -> game, or the conditioner.
//
// How: two observers in the framework's input chain (latency 0, never change the audio), one before the conditioner
// (-100) and one right after it, before the latency capture (-50) and the Drop Pedal shifter (0), the points 4.x
// measured at. Stage 1 (the real device's samples inside the bridge driver, before RS_ASIO) comes from the driver's
// RSModsAsio_SetInputObserver callback, which also feeds GameToneLatency's ASIO input side as 4.x did. 4.x's proxy
// callback also processed the audio itself while the game had no capture client bound; that fallback is not ported:
// the framework chain runs on the game's capture client only, so it would apply the chain a second time.
namespace AudioBridge::Input::StageMeter
{
	enum class Point
	{
		CaptureRaw,         // what RS_ASIO (or the cable) handed the game, before the conditioner
		AfterConditioner,   // what the game's amp and detection hear, before any pitch shift
	};

	class Observer final : public Framework::IInputProcessor
	{
	public:
		explicit Observer(Point point) : point(point) {}

		void Prepare(std::uint32_t input, const Framework::CaptureFormat& format) override;
		bool Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
			bool active) noexcept override;
		std::uint32_t GetLatencyFrames(std::uint32_t input) const override;

	private:
		Point point;
		std::array<std::atomic<std::uint32_t>, Framework::kMaxAudioInputs> sampleRates{};
	};

	// Chain positions: the conditioner runs at -100, the latency capture at -50.
	constexpr int CAPTURE_RAW_ORDER = -200;
	constexpr int AFTER_CONDITIONER_ORDER = -90;

	Observer& CaptureRawObserver();
	Observer& AfterConditionerObserver();

	// CaptureTap, on that input's audio thread, right before it runs the chain: whether the game's packet carried
	// AUDCLNT_BUFFERFLAGS_SILENT (the chain then sees zeros).
	void BeginPacket(std::uint32_t input, bool silentFlagged) noexcept;
	// CaptureTap: the bridge's placeholder packet (device away or changed) was handed to the game as silence.
	void NoteReplacedWithSilence(std::uint32_t input) noexcept;

	// MainThread (the tap mod's OnTick): installs the bridge driver's input observer once it is loaded and logs the
	// stage line every 10 s.
	void Tick();
}
