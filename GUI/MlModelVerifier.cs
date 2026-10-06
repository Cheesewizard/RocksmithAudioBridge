using System;
using System.IO;
using RSModsPlus.MachineLearning;

namespace RSMods
{
	internal static class MlModelVerifier
	{
		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || (arguments[0] != "--ml-verify-model" && arguments[0] != "--ml-verify-audio")) return false;

			try
			{
				var isAudio = arguments[0] == "--ml-verify-audio";
				if (arguments.Length != (isAudio ? 4 : 3)) throw new ArgumentException("Usage: RSMods.exe --ml-verify-model <input.f32> <output.f32> or --ml-verify-audio <input.f32> <output.f32> <sample rate>");

				var bytes = File.ReadAllBytes(arguments[1]);
				if (bytes.Length == 0 || bytes.Length % sizeof(float) != 0 || (!isAudio && bytes.Length != FretNetSession.FEATURE_COUNT * sizeof(float))) throw new ArgumentException("Invalid float32 input length.");

				var features = new float[bytes.Length / sizeof(float)];
				Buffer.BlockCopy(bytes, 0, features, 0, bytes.Length);
				var centerSample = 0;
				if (isAudio) features = new CqtTransform().ComputeLatest(features, int.Parse(arguments[3]), out centerSample);
				using (var session = new FretNetSession())
				{
					var outputs = session.Predict(features);
					using (var writer = new BinaryWriter(File.Open(arguments[2], FileMode.CreateNew)))
					{
						if (isAudio)
						{
							writer.Write(centerSample);
							foreach (var value in features) writer.Write(value);
						}
						foreach (var output in outputs)
						{
							foreach (var value in output)
							{
								writer.Write(value);
							}
						}
					}
				}
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				System.Diagnostics.Trace.TraceError(exception.ToString());
				exitCode = 1;
			}
			return true;
		}
	}
}
