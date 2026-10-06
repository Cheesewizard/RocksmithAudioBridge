using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace RSMods.Audio
{
	internal sealed class AudioBufferSnapshot
	{
		public uint Minimum { get; private set; }
		public uint Fundamental { get; private set; }
		public uint Maximum { get; private set; }
		public uint Period { get; private set; }
		public string Endpoint { get; private set; }

		public static AudioBufferSnapshot Parse(AudioControlStatus status)
		{
			if (status == null) throw new ArgumentNullException(nameof(status));
			if (status.OutputError < 0)
				throw new InvalidOperationException("The output stream is unavailable.");
			var fields = new Dictionary<string, ulong>();
			foreach (string item in status.FilePath.Split(' '))
			{
				string[] pair = item.Split('=');
				if (pair.Length == 2 && ulong.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out ulong value))
					fields.Add(pair[0], value);
			}
			foreach (string key in new[] { "engineMinimum", "engineFundamental", "engineMaximum", "enginePeriod" })
			{
				if (!fields.ContainsKey(key)) throw new IOException("This bridge does not provide output buffer details. Update the game DLL.");
			}
			var result = new AudioBufferSnapshot
			{
				Minimum = checked((uint)fields["engineMinimum"]), Fundamental = checked((uint)fields["engineFundamental"]),
				Maximum = checked((uint)fields["engineMaximum"]), Period = checked((uint)fields["enginePeriod"]),
				Endpoint = status.EndpointId
			};
			if (result.Minimum == 0 || result.Fundamental == 0 || result.Maximum < result.Minimum || result.Maximum > 48000)
				throw new IOException("The device returned invalid buffer limits.");
			return result;
		}
	}
}
