using System;

namespace RSModsPlus.MachineLearning
{
	public sealed class NoteEvidence
	{
		private static readonly int[] openStringMidi = { 40, 45, 50, 55, 59, 64 };
		// The bass model fills slots 0..3 (E1 A1 D2 G2); slots 4 and 5 are trained silent and ignored.
		private static readonly int[] bassOpenStringMidi = { 28, 33, 38, 43 };
		public int[] Frets { get; } = { -1, -1, -1, -1, -1, -1 };
		public float[] Confidence { get; } = new float[6];

		public static NoteEvidence Decode(float[] logits, int expectedString, int expectedMidi, bool bass = false)
		{
			var open = bass ? bassOpenStringMidi : openStringMidi;
			var stringCount = open.Length;
			if (logits == null || logits.Length != 126) throw new ArgumentException("Expected six strings with 21 logits each.", nameof(logits));
			var probabilities = new double[126];
			var result = new NoteEvidence();
			for (var guitarString = 0; guitarString < stringCount; guitarString++)
			{
				var start = guitarString * 21;
				var maximum = double.NegativeInfinity;
				var bestClass = 0;
				for (var index = 0; index < 21; index++)
				{
					var value = logits[start + index];
					if (float.IsNaN(value) || float.IsInfinity(value)) throw new ArgumentException("Model logits must be finite.", nameof(logits));
					if (value > maximum)
					{
						maximum = value;
						bestClass = index;
					}
				}
				var sum = 0.0;
				for (var index = 0; index < 21; index++) sum += probabilities[start + index] = Math.Exp(logits[start + index] - maximum);
				for (var index = 0; index < 21; index++) probabilities[start + index] /= sum;
				if (bestClass > 0 && probabilities[start + bestClass] >= 0.5)
				{
					result.Frets[guitarString] = bestClass - 1;
					result.Confidence[guitarString] = (float)Math.Round(probabilities[start + bestClass], 3);
				}
			}
			if (expectedString >= 0 && expectedString < stringCount)
			{
				var expectedFret = expectedMidi - open[expectedString];
				if (expectedFret >= 0 && expectedFret <= 19)
				{
					var confidence = 0.0;
					for (var guitarString = 0; guitarString < stringCount; guitarString++)
					{
						var fret = expectedMidi - open[guitarString];
						if (fret >= 0 && fret <= 19) confidence = Math.Max(confidence, probabilities[guitarString * 21 + fret + 1]);
					}
					if (confidence >= 0.35)
					{
						result = new NoteEvidence();
						result.Frets[expectedString] = expectedFret;
						result.Confidence[expectedString] = (float)Math.Round(confidence, 3);
					}
				}
			}
			return result;
		}
	}
}
