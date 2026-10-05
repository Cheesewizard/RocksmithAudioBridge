using System;
using System.IO;
using System.Numerics;

namespace RSModsPlus.MachineLearning
{
	public sealed class CqtTransform
	{
		public const int SAMPLE_RATE = 22050;
		public const int HOP_LENGTH = 512;
		private const int BIN_COUNT = 192;
		private const int CONTEXT = 9;
		private readonly CqtOctave[] octaves = new CqtOctave[8];
		private readonly double[] lengthScale = new double[BIN_COUNT];

		public CqtTransform()
		{
			using (var stream = typeof(CqtTransform).Assembly.GetManifestResourceStream("RSModsPlus.MachineLearning.CqtBasis.bin"))
			{
				if (stream == null) throw new InvalidOperationException("Embedded CQT basis is missing.");
				using (var reader = new BinaryReader(stream))
				{
					if (reader.ReadInt32() != 1) throw new InvalidDataException("Unsupported CQT basis version.");
					for (var bin = 0; bin < BIN_COUNT; bin++) lengthScale[bin] = reader.ReadDouble();
					for (var octave = 0; octave < octaves.Length; octave++)
					{
						var basis = new CqtOctave(reader.ReadInt32(), reader.ReadInt32());
						for (var bin = 0; bin < 24; bin++)
						{
							var count = reader.ReadInt32();
							basis.indices[bin] = new int[count];
							basis.weights[bin] = new Complex[count];
							for (var index = 0; index < count; index++)
							{
								basis.indices[bin][index] = reader.ReadInt32();
								basis.weights[bin][index] = new Complex(reader.ReadSingle(), reader.ReadSingle());
							}
						}
						octaves[octave] = basis;
					}
					if (stream.Position != stream.Length) throw new InvalidDataException("Unexpected CQT basis data.");
				}
			}
		}

		public float[] ComputeLatest(float[] audio, int sampleRate, out int centerSample)
		{
			if (audio == null) throw new ArgumentNullException(nameof(audio));
			if (audio.Length == 0 || sampleRate <= 0) throw new ArgumentException("A nonempty audio window and positive sample rate are required.");
			for (var index = 0; index < audio.Length; index++)
			{
				if (float.IsNaN(audio[index]) || float.IsInfinity(audio[index])) throw new ArgumentException("Audio must be finite.", nameof(audio));
			}
			var samples = sampleRate == SAMPLE_RATE ? audio : SampleRateConverter.Convert(audio, sampleRate, SAMPLE_RATE, false);
			var frames = 1 + samples.Length / HOP_LENGTH;
			if (frames < CONTEXT) throw new ArgumentException("Audio window is too short for the trained context.", nameof(audio));
			var magnitudes = new double[BIN_COUNT * frames];
			var peak = 0.0;
			for (var octave = 0; octave < octaves.Length; octave++)
			{
				var basis = octaves[octave];
				var spectrum = new Complex[basis.fftSize];
				for (var frame = 0; frame < frames; frame++)
				{
					var start = frame * basis.hop - basis.fftSize / 2;
					for (var index = 0; index < spectrum.Length; index++)
					{
						var position = start + index;
						spectrum[index] = position >= 0 && position < samples.Length ? samples[position] : Complex.Zero;
					}
					ForwardFft(spectrum);
					for (var bin = 0; bin < 24; bin++)
					{
						var sum = Complex.Zero;
						for (var index = 0; index < basis.indices[bin].Length; index++)
						{
							sum += basis.weights[bin][index] * spectrum[basis.indices[bin][index]];
						}
						var targetBin = (7 - octave) * 24 + bin;
						var magnitude = sum.Magnitude / lengthScale[targetBin];
						magnitudes[targetBin * frames + frame] = magnitude;
						peak = Math.Max(peak, magnitude);
					}
				}
				if (octave < octaves.Length - 1) samples = SampleRateConverter.Convert(samples, 2, 1, true);
			}
			var tile = new float[BIN_COUNT * CONTEXT];
			for (var bin = 0; bin < BIN_COUNT; bin++)
			{
				for (var frame = 0; frame < CONTEXT; frame++)
				{
					tile[bin * CONTEXT + frame] = peak > 0 ? (float)(magnitudes[bin * frames + frames - CONTEXT + frame] / peak) : 0;
				}
			}
			centerSample = (int)Math.Round((frames - 5) * (double)HOP_LENGTH * sampleRate / SAMPLE_RATE);
			return tile;
		}

		private static void ForwardFft(Complex[] values)
		{
			for (int index = 1, reversed = 0; index < values.Length; index++)
			{
				var bit = values.Length >> 1;
				for (; (reversed & bit) != 0; bit >>= 1) reversed ^= bit;
				reversed ^= bit;
				if (index < reversed)
				{
					var temporary = values[index];
					values[index] = values[reversed];
					values[reversed] = temporary;
				}
			}
			for (var length = 2; length <= values.Length; length <<= 1)
			{
				var rotation = Complex.FromPolarCoordinates(1, -2 * Math.PI / length);
				for (var start = 0; start < values.Length; start += length)
				{
					var weight = Complex.One;
					for (var offset = 0; offset < length / 2; offset++)
					{
						var first = values[start + offset];
						var second = weight * values[start + offset + length / 2];
						values[start + offset] = first + second;
						values[start + offset + length / 2] = first - second;
						weight *= rotation;
					}
				}
			}
		}
	}

	internal sealed class CqtOctave
	{
		public readonly int fftSize;
		public readonly int hop;
		public readonly int[][] indices = new int[24][];
		public readonly Complex[][] weights = new Complex[24][];

		public CqtOctave(int fftSize, int hop)
		{
			this.fftSize = fftSize;
			this.hop = hop;
		}
	}
}
