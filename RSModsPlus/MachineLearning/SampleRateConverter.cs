using System;
using System.Runtime.InteropServices;

namespace RSModsPlus.MachineLearning
{
	internal static class SampleRateConverter
	{
		public static float[] Convert(float[] input, double inputRate, double outputRate, bool scale)
		{
			if (input == null) throw new ArgumentNullException(nameof(input));
			if (inputRate <= 0 || outputRate <= 0) throw new ArgumentOutOfRangeException(nameof(inputRate));
			var output = new float[checked((int)Math.Ceiling(input.Length * outputRate / inputRate))];
			var quality = SoxrQualitySpec(4, 0);
			var error = SoxrOneShot(inputRate, outputRate, 1, input, (UIntPtr)input.Length,
				out var consumed, output, (UIntPtr)output.Length, out var produced, IntPtr.Zero, ref quality, IntPtr.Zero);
			if (error != IntPtr.Zero) throw new InvalidOperationException(Marshal.PtrToStringAnsi(error));
			if (consumed.ToUInt64() != (ulong)input.Length || produced.ToUInt64() > (ulong)output.Length)
			{
				throw new InvalidOperationException("The resampler did not consume the complete audio window.");
			}
			if (scale)
			{
				var factor = (float)Math.Sqrt(inputRate / outputRate);
				for (var index = 0; index < output.Length; index++) output[index] *= factor;
			}
			return output;
		}

		[DllImport("soxr.dll", EntryPoint = "soxr_quality_spec", CallingConvention = CallingConvention.Cdecl)]
		private static extern SoxrQuality SoxrQualitySpec(uint recipe, uint flags);

		[DllImport("soxr.dll", EntryPoint = "soxr_oneshot", CallingConvention = CallingConvention.Cdecl)]
		private static extern IntPtr SoxrOneShot(double inputRate, double outputRate, uint channels,
			float[] input, UIntPtr inputLength, out UIntPtr consumed, [Out] float[] output,
			UIntPtr outputLength, out UIntPtr produced, IntPtr inputOutputSpec, ref SoxrQuality quality, IntPtr runtimeSpec);
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct SoxrQuality
	{
		public double precision;
		public double phaseResponse;
		public double passbandEnd;
		public double stopbandBegin;
		public IntPtr reserved;
		public uint flags;
	}
}
