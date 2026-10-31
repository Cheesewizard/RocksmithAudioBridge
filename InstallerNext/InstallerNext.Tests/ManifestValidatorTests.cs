namespace RocksmithAudioBridge.Setup.Tests;

public class ManifestValidatorTests
{
	[Fact]
	public void A_good_manifest_passes()
	{
		var result = ModManifestValidator.Validate(TestRig.ValidManifest);
		Assert.True(result.Ok, string.Join("\n", result.Errors));
		Assert.Equal(7, result.SettingCount);
		Assert.Equal(6, result.OurSettingCount);
	}

	[Theory]
	[InlineData("not json", "does not parse")]
	[InlineData("[]", "no settings")]
	[InlineData("""[{ "key": "A", "ini": { "section": "S", "name": "A" }, "type": "Banana", "default": "", "label": "", "category": "" }]""", "does not parse")]
	public void Broken_manifests_fail(string json, string expected)
	{
		var result = ModManifestValidator.Validate(json, Array.Empty<string>());
		Assert.False(result.Ok);
		Assert.Contains(result.Errors, e => e.Contains(expected));
	}

	[Fact]
	public void Missing_fields_duplicate_keys_and_missing_mods_are_errors()
	{
		const string json = """
		[
		  { "key": "A", "ini": { "section": "", "name": "A" }, "type": "Bool", "default": "on", "label": "", "category": "", "mod": "NoteByNoteMod" },
		  { "key": "a", "ini": { "section": "S", "name": "" }, "type": "Bool", "default": "on", "label": "", "category": "", "mod": "NoteByNoteMod" },
		  { "key": "", "type": "Enum", "default": "", "label": "", "category": "" }
		]
		""";
		var result = ModManifestValidator.Validate(json);
		Assert.Contains(result.Errors, e => e.Contains("no ini section"));
		Assert.Contains(result.Errors, e => e.Contains("no ini name"));
		Assert.Contains(result.Errors, e => e.Contains("no key"));
		Assert.Contains(result.Errors, e => e.Contains("no ini location"));
		Assert.Contains(result.Errors, e => e.Contains("Enum with no choices"));
		Assert.Contains(result.Errors, e => e.Contains("declared 2 times"));
		Assert.Contains(result.Errors, e => e.Contains("DropPedalMod"));
	}

	[Fact]
	public void The_last_dumped_manifest_in_this_checkout_is_valid()
	{
		// build/mod/mods.manifest.json from Mod/Build-ModDll.ps1, when this checkout has one.
		string? manifest = null;
		for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null && manifest == null; folder = folder.Parent)
		{
			string candidate = Path.Combine(folder.FullName, "build", "mod", "mods.manifest.json");
			if (File.Exists(candidate)) manifest = candidate;
		}
		if (manifest == null) return;
		var result = ModManifestValidator.Validate(File.ReadAllText(manifest));
		Assert.True(result.Ok, string.Join("\n", result.Errors));
	}
}
