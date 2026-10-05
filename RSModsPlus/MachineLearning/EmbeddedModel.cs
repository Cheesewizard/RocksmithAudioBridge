using System;
using System.IO;
using System.Security.Cryptography;

namespace RSModsPlus.MachineLearning
{
	public static class EmbeddedModel
	{
		private const string RESOURCE_NAME = "RSModsPlus.MachineLearning.FretNet.onnx";
		// Bass arrangements: same input/output contract, string slots 0..3 = E1 A1 D2 G2.
		private const string BASS_RESOURCE_NAME = "RSModsPlus.MachineLearning.FretNetBass.onnx";

		public static byte[] ReadBytes(bool bass = false)
		{
			using (var stream = typeof(EmbeddedModel).Assembly.GetManifestResourceStream(bass ? BASS_RESOURCE_NAME : RESOURCE_NAME))
			{
				if (stream == null) throw new InvalidOperationException("The RSModsPlus assembly does not contain its FretNet model.");
				if (stream.Length == 0) throw new InvalidOperationException("The embedded FretNet model is empty.");

				using (var buffer = new MemoryStream())
				{
					stream.CopyTo(buffer);
					return buffer.ToArray();
				}
			}
		}

		public static string GetSha256(bool bass = false)
		{
			using (var algorithm = SHA256.Create())
			{
				return BitConverter.ToString(algorithm.ComputeHash(ReadBytes(bass))).Replace("-", "").ToLowerInvariant();
			}
		}
	}
}
