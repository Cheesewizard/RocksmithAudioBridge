using System;
using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace RSModsPlus.MachineLearning
{
	public sealed class FretNetSession : IDisposable
	{
		public const int FEATURE_COUNT = 192 * 9;

		private readonly InferenceSession session;
		private bool isDisposed;

		public FretNetSession(bool bass = false)
		{
			if (!Environment.Is64BitProcess) throw new PlatformNotSupportedException("FretNet must run in the 64-bit RSMods process.");

			using (var options = new SessionOptions())
			{
				options.IntraOpNumThreads = 1;
				options.InterOpNumThreads = 1;
				options.GraphOptimizationLevel = GraphOptimizationLevel.ORT_ENABLE_ALL;
				session = new InferenceSession(EmbeddedModel.ReadBytes(bass), options);
			}
		}

		// Input is the normalized training CQT tile in [bin, context frame] order, not raw audio.
		public float[][] Predict(float[] features)
		{
			if (isDisposed) throw new ObjectDisposedException(nameof(FretNetSession));
			if (features == null) throw new ArgumentNullException(nameof(features));
			if (features.Length != FEATURE_COUNT) throw new ArgumentException("Expected a 192 by 9 CQT tile.", nameof(features));

			for (var index = 0; index < features.Length; index++)
			{
				if (float.IsNaN(features[index]) || float.IsInfinity(features[index]))
				{
					throw new ArgumentException("CQT features must be finite.", nameof(features));
				}
			}

			var tensor = new DenseTensor<float>(features, new[] { 1, 1, 192, 9 });
			var inputs = new[] { NamedOnnxValue.CreateFromTensor("cqt_tile", tensor) };
			var names = new[] { "string_fret_logits", "pitch_deviation", "onset_logits" };
			using (var outputs = session.Run(inputs, names))
			{
				var result = new float[3][];
				var outputIndex = 0;
				foreach (var output in outputs)
				{
					var values = output.AsTensor<float>();
					var expectedLength = outputIndex == 0 ? 126 : 6;
					if (values.Length != expectedLength) throw new InvalidOperationException("FretNet output does not match the trained model contract.");

					result[outputIndex] = new float[expectedLength];
					for (var index = 0; index < expectedLength; index++)
					{
						result[outputIndex][index] = values.GetValue(index);
					}
					outputIndex++;
				}
				return result;
			}
		}

		public void Dispose()
		{
			if (isDisposed) return;

			session.Dispose();
			isDisposed = true;
		}
	}
}
