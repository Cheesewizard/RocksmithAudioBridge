using System;
using System.Globalization;

namespace RSMods
{
	internal static class PsarcEntryPath
	{
		private const string WEM_EXTENSION = ".wem";

		public static bool TryGetFileName(string entryPath, out string fileName)
		{
			if (entryPath == null)
			{
				fileName = null;
				return false;
			}

			var separatorIndex = Math.Max(entryPath.LastIndexOf('/'), entryPath.LastIndexOf('\\'));
			fileName = entryPath.Substring(separatorIndex + 1);
			return true;
		}

		public static bool TryGetWemMediaId(string entryPath, out uint mediaId)
		{
			if (!TryGetFileName(entryPath, out string fileName)
				|| !fileName.EndsWith(WEM_EXTENSION, StringComparison.OrdinalIgnoreCase))
			{
				mediaId = 0;
				return false;
			}

			var name = fileName.Substring(0, fileName.Length - WEM_EXTENSION.Length);
			return uint.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out mediaId);
		}
	}
}
