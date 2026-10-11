using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RSMods
{
	// A minimal INI file kept as its lines, for files another program reads (RS_ASIO.ini, Rocksmith.ini). Changing a
	// value rewrites only that line; comments, blank lines, key order, unknown keys and the file's line endings stay as
	// they were. Written without a byte order mark: RS_ASIO and the game read the file with the Windows profile API,
	// and a [Config] header they cannot see leaves RS_ASIO with everything off (no device at all).
	internal sealed class IniText
	{
		private readonly List<string> lines;
		private readonly string newline;

		private IniText(List<string> lines, string newline)
		{
			this.lines = lines;
			this.newline = newline;
		}

		public static IniText Load(string path)
		{
			string text = File.Exists(path) ? File.ReadAllText(path) : "";
			return Parse(text);
		}

		public static IniText Parse(string text)
		{
			string newline = text.Contains("\r\n") || !text.Contains("\n") ? "\r\n" : "\n";
			var lines = new List<string>(text.Replace("\r\n", "\n").Split('\n'));
			if (lines.Count > 0 && lines[lines.Count - 1].Length == 0) lines.RemoveAt(lines.Count - 1);   // trailing newline
			if (lines.Count > 0 && lines[0].Length > 0 && lines[0][0] == '﻿') lines[0] = lines[0].Substring(1);
			return new IniText(lines, newline);
		}

		public string Text => string.Join(newline, lines) + newline;

		public void Save(string path) => File.WriteAllText(path, Text, new UTF8Encoding(false));

		public bool HasSection(string section) => FindSection(section) >= 0;

		/// <summary>The value, or null when the key (or its section) is missing.</summary>
		public string Get(string section, string key)
		{
			int line = FindKey(section, key);
			if (line < 0) return null;
			string text = lines[line];
			return text.Substring(text.IndexOf('=') + 1).Trim();
		}

		/// <summary>Sets the value in place; a missing key goes at the end of its section, a missing section at the
		/// end of the file.</summary>
		public void Set(string section, string key, string value)
		{
			int line = FindKey(section, key);
			if (line >= 0) { lines[line] = key + "=" + value; return; }
			int start = FindSection(section);
			if (start < 0)
			{
				if (lines.Count > 0 && lines[lines.Count - 1].Trim().Length > 0) lines.Add("");
				lines.Add("[" + section + "]");
				lines.Add(key + "=" + value);
				return;
			}
			int insert = start + 1;
			for (int i = start + 1; i < lines.Count && !IsSectionHeader(lines[i]); i++)
				if (lines[i].Trim().Length > 0) insert = i + 1;   // after the section's last non-blank line
			lines.Insert(insert, key + "=" + value);
		}

		private int FindSection(string section)
		{
			for (int i = 0; i < lines.Count; i++)
				if (IsSectionHeader(lines[i]) && string.Equals(lines[i].Trim().Trim('[', ']').Trim(), section, StringComparison.OrdinalIgnoreCase))
					return i;
			return -1;
		}

		private int FindKey(string section, string key)
		{
			int start = FindSection(section);
			if (start < 0) return -1;
			for (int i = start + 1; i < lines.Count && !IsSectionHeader(lines[i]); i++)
			{
				string trimmed = lines[i].TrimStart();
				if (trimmed.StartsWith(";") || trimmed.StartsWith("#")) continue;
				int equals = trimmed.IndexOf('=');
				if (equals > 0 && string.Equals(trimmed.Substring(0, equals).Trim(), key, StringComparison.OrdinalIgnoreCase)) return i;
			}
			return -1;
		}

		private static bool IsSectionHeader(string line)
		{
			string trimmed = line.Trim();
			return trimmed.StartsWith("[") && trimmed.EndsWith("]");
		}
	}
}
