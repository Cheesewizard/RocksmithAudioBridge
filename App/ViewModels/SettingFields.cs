using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using RocksmithAudioBridge.Core;
using RSMods;
using RSMods.Core.Settings;

namespace RocksmithAudioBridge.App.ViewModels;

/// <summary>
/// One manifest setting as an editor. The manifest says the type, range, scale, choices and visibility; the value lives
/// in RSMods.ini at descriptor.Ini. Value conventions follow upstream's GUI.Core field view models: Bool is "on"/"off"
/// (or "1"/"0" where the file or default uses numbers), Int is stored unscaled and shown times Scale, Key is a VK_ name.
/// A change is written at once (RsModsIni reloads, sets the one key and saves) and the running game is told to reload.
/// </summary>
public sealed partial class SettingFieldViewModel : ObservableObject
{
	private readonly SettingsGroupViewModel group;
	private bool loading;
	private bool numericBool;

	public SettingFieldViewModel(SettingDescriptor descriptor, SettingsGroupViewModel group)
	{
		Descriptor = descriptor;
		this.group = group;
		if (descriptor.Type == SettingType.Key) Choices = new ObservableCollection<string>(KeyNames);
		else if (descriptor.Choices is { Count: > 0 }) Choices = new ObservableCollection<string>(descriptor.Choices);
		else Choices = new ObservableCollection<string>();
	}

	public SettingDescriptor Descriptor { get; }
	public string Label => Descriptor.Label;
	public string Hint => (Descriptor.Hint ?? "").Replace("\n", " ");
	public bool HasHint => Hint.Length > 0;
	public string Location => "[" + Descriptor.Ini.Section.Trim('[', ']') + "] " + Descriptor.Ini.Name;

	public bool IsBool => Descriptor.Type == SettingType.Bool;
	public bool IsNumber => Descriptor.Type == SettingType.Int;
	public bool IsChoice => Descriptor.Type is SettingType.Enum or SettingType.Key;
	public bool IsText => Descriptor.Type is SettingType.String or SettingType.Color;

	public ObservableCollection<string> Choices { get; }

	public double Scale => Descriptor.Scale is > 0 ? Descriptor.Scale.Value : 1.0;
	public decimal Minimum => (decimal)((Descriptor.Min ?? int.MinValue / 2) * Scale);
	public decimal Maximum => (decimal)((Descriptor.Max ?? int.MaxValue / 2) * Scale);
	public decimal Increment => (decimal)Scale;
	public string NumberFormat => Scale < 1 ? "0.0" : "0";

	[ObservableProperty] private bool _boolValue;
	[ObservableProperty] private decimal? _numberValue;
	[ObservableProperty] private string? _choiceValue;
	[ObservableProperty] private string _textValue = "";
	[ObservableProperty] private bool _isVisible = true;
	[ObservableProperty] private string _error = "";

	/// <summary>The value as written in RSMods.ini.</summary>
	public string RawValue { get; private set; } = "";

	public void Load(IniManager snapshot)
	{
		loading = true;
		try
		{
			string section = "[" + Descriptor.Ini.Section.Trim('[', ']') + "]";
			RawValue = snapshot.TryGetString(section, Descriptor.Ini.Name, out string value) ? value.Trim() : Descriptor.Default;
			switch (Descriptor.Type)
			{
				case SettingType.Bool:
					numericBool = RawValue is "0" or "1" || (!RawValue.Equals("on", StringComparison.OrdinalIgnoreCase)
						&& !RawValue.Equals("off", StringComparison.OrdinalIgnoreCase) && Descriptor.Default is "0" or "1");
					BoolValue = RawValue.Equals("on", StringComparison.OrdinalIgnoreCase) || RawValue == "1" || RawValue.Equals("true", StringComparison.OrdinalIgnoreCase);
					break;
				case SettingType.Int:
					NumberValue = int.TryParse(RawValue, NumberStyles.Integer, CultureInfo.InvariantCulture, out int number)
						? (decimal)(number * Scale)
						: int.TryParse(Descriptor.Default, NumberStyles.Integer, CultureInfo.InvariantCulture, out int fallback) ? (decimal)(fallback * Scale) : 0;
					break;
				case SettingType.Enum:
				case SettingType.Key:
					if (RawValue.Length > 0 && !Choices.Contains(RawValue)) Choices.Insert(0, RawValue);
					ChoiceValue = RawValue;
					break;
				default:
					TextValue = RawValue;
					break;
			}
		}
		finally { loading = false; }
	}

	partial void OnBoolValueChanged(bool value) => Write(numericBool ? (value ? "1" : "0") : (value ? "on" : "off"));

	partial void OnNumberValueChanged(decimal? value)
	{
		if (value == null) return;
		int stored = (int)Math.Round((double)value.Value / Scale);
		if (Descriptor.Min is int min) stored = Math.Max(min, stored);
		if (Descriptor.Max is int max) stored = Math.Min(max, stored);
		Write(stored.ToString(CultureInfo.InvariantCulture));
	}

	partial void OnChoiceValueChanged(string? value)
	{
		if (!string.IsNullOrEmpty(value)) Write(value);
	}

	partial void OnTextValueChanged(string value) => Write(value.Trim());

	private void Write(string raw)
	{
		if (loading || raw == RawValue || AppState.ReadOnly) return;
		try
		{
			group.Ini?.Set(Descriptor.Ini.Section.Trim('[', ']'), Descriptor.Ini.Name, raw);
			RawValue = raw;
			Error = "";
			group.UpdateVisibility();
		}
		catch (Exception error) { Error = "Not saved: " + error.Message; }
	}

	// Keys the DLL resolves by VK_ name. The overlay's General page offers every key; this list covers the usual ones.
	private static readonly string[] KeyNames = BuildKeyNames();

	private static string[] BuildKeyNames()
	{
		var names = new List<string>();
		for (int f = 1; f <= 12; f++) names.Add("VK_F" + f);
		names.AddRange(new[]
		{
			"VK_INSERT", "VK_DELETE", "VK_HOME", "VK_END", "VK_PRIOR", "VK_NEXT", "VK_PAUSE", "VK_SCROLL",
			"VK_OEM_1", "VK_OEM_2", "VK_OEM_3", "VK_OEM_4", "VK_OEM_5", "VK_OEM_6", "VK_OEM_7",
			"VK_OEM_COMMA", "VK_OEM_PERIOD", "VK_OEM_MINUS", "VK_OEM_PLUS",
			"VK_NUMPAD0", "VK_NUMPAD1", "VK_NUMPAD2", "VK_NUMPAD3", "VK_NUMPAD4", "VK_NUMPAD5", "VK_NUMPAD6", "VK_NUMPAD7", "VK_NUMPAD8", "VK_NUMPAD9",
			"VK_MULTIPLY", "VK_ADD", "VK_SUBTRACT", "VK_DECIMAL", "VK_DIVIDE",
		});
		return names.ToArray();
	}
}

/// <summary>A titled list of manifest settings (one mod, or one page's share of them).</summary>
public sealed partial class SettingsGroupViewModel : ObservableObject
{
	// Must match AudioBridge::BESPOKE_EDITOR in Mod/Core/BespokeSettings.hpp.
	private const string BespokeEditor = "RocksmithAudioBridge";

	private readonly AppState state;

	public SettingsGroupViewModel(AppState state, string title, IEnumerable<SettingDescriptor> settings)
	{
		this.state = state;
		Title = title;
		// Our mods hand their settings to "RocksmithAudioBridge" (Mod/Core/BespokeSettings.hpp) so upstream's generic forms
		// skip them; this page is one of those bespoke editors, so it shows them. Settings a narrower screen writes
		// (editedBy: the overlay's HUD placement, the gate's remembered threshold) stay out.
		foreach (var descriptor in settings.Where(d => string.IsNullOrEmpty(d.EditedBy) || d.EditedBy == BespokeEditor))
			Fields.Add(new SettingFieldViewModel(descriptor, this));
	}

	public string Title { get; }
	public ObservableCollection<SettingFieldViewModel> Fields { get; } = new();
	public bool IsEmpty => Fields.Count == 0;
	internal RsModsIni? Ini => state.Ini;

	public void Load()
	{
		var ini = state.Ini;
		if (ini == null) return;
		var snapshot = ini.Snapshot();
		foreach (var field in Fields) field.Load(snapshot);
		UpdateVisibility();
	}

	/// <summary>visibleWhen: shown only while another setting (any group) has the given raw value.</summary>
	public void UpdateVisibility()
	{
		var ini = state.Ini;
		IniManager? snapshot = null;
		foreach (var field in Fields)
		{
			var condition = field.Descriptor.VisibleWhen;
			if (condition == null) { field.IsVisible = true; continue; }
			var other = Fields.FirstOrDefault(f => string.Equals(f.Descriptor.Key, condition.Key, StringComparison.OrdinalIgnoreCase));
			string raw;
			if (other != null) raw = other.RawValue;
			else
			{
				var descriptor = state.Manifest.Settings.FirstOrDefault(s => string.Equals(s.Key, condition.Key, StringComparison.OrdinalIgnoreCase));
				if (descriptor == null || ini == null) { field.IsVisible = true; continue; }
				snapshot ??= ini.Snapshot();
				raw = snapshot.TryGetString("[" + descriptor.Ini.Section.Trim('[', ']') + "]", descriptor.Ini.Name, out string value) ? value.Trim() : descriptor.Default;
			}
			field.IsVisible = string.Equals(raw, condition.ExpectedValue, StringComparison.OrdinalIgnoreCase);
		}
	}

	/// <summary>Friendly names for our mods' ids.</summary>
	public static string ModTitle(string? mod) => mod switch
	{
		"AudioBridgeShellMod" => "Overlay",
		"AudioBridgeRoutingMod" => "Output and Rocksmith's gate",
		"AudioRecorderMod" => "Recorder",
		"DropPedalMod" => "Drop Pedal",
		"InputConditionerMod" => "Guitar input",
		"NoteByNoteMod" => "Note by Note",
		_ => mod ?? "Other",
	};
}
