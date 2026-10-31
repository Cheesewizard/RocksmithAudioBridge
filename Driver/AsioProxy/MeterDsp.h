#pragma once

// Per-channel output metering, shared by the proxy (which meters the forwarded output) and an offline test.
// Pure and header-only. The proxy computes a block's linear peak and RMS from the output samples, then feeds
// them here; this keeps a decaying peak-hold (so a brief transient stays visible) and a smoothed RMS.
namespace MeterDsp
{
	struct ChannelMeter
	{
		float peakHold = 0.0f;    // decaying peak, [0,1]+
		float rms = 0.0f;         // smoothed RMS, [0,1]
		float peakDecay = 0.95f;  // per-block peak-hold decay (closer to 1 = holds longer)
		float rmsCoef = 0.2f;     // per-block RMS smoothing (higher = snappier)

		// Feed one block's already-computed linear peak and RMS.
		void Update(float blockPeak, float blockRms)
		{
			// >= so a sustained level holds steady; only a strictly lower block lets the hold decay.
			peakHold = (blockPeak >= peakHold) ? blockPeak : peakHold * peakDecay;
			rms += (blockRms - rms) * rmsCoef;
		}
	};
}
