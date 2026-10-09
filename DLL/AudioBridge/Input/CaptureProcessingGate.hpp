#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace Audio
{
	// Control callers must serialize CloseAndWait/Open. Audio callbacks never wait.
	class CaptureProcessingGate
	{
	public:
		bool TryEnter()
		{
			if (state.load(std::memory_order_acquire) & CLOSED) return false;
			const uint32_t previous = state.fetch_add(1, std::memory_order_acquire);
			if ((previous & CLOSED) == 0) return true;
			Leave();
			return false;
		}

		void Leave()
		{
			state.fetch_sub(1, std::memory_order_release);
		}

		void CloseAndWait()
		{
			state.fetch_or(CLOSED, std::memory_order_acq_rel);
			while ((state.load(std::memory_order_acquire) & ~CLOSED) != 0)
				std::this_thread::yield();
		}

		void Open()
		{
			state.fetch_and(~CLOSED, std::memory_order_release);
		}

		bool IsOpen() const
		{
			return (state.load(std::memory_order_acquire) & CLOSED) == 0;
		}

	private:
		static constexpr uint32_t CLOSED = 0x80000000;
		std::atomic<uint32_t> state{ CLOSED };
	};

	class CaptureCallbackScope
	{
	public:
		explicit CaptureCallbackScope(CaptureProcessingGate& value)
			: gate(value), entered(gate.TryEnter()) {}

		~CaptureCallbackScope()
		{
			if (entered) gate.Leave();
		}

		CaptureCallbackScope(const CaptureCallbackScope&) = delete;
		CaptureCallbackScope& operator=(const CaptureCallbackScope&) = delete;

		explicit operator bool() const { return entered; }

	private:
		CaptureProcessingGate& gate;
		bool entered;
	};
}
