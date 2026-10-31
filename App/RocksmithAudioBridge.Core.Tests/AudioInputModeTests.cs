namespace RocksmithAudioBridge.Core.Tests;

public class AudioInputModeTests
{
	private static AudioInputModeConfiguration Config(TempFolder folder, bool running = false) => new(folder.Path, () => running);

	[Fact]
	public void No_RS_ASIO_files_is_unavailable()
	{
		using var folder = new TempFolder();
		Assert.Equal(AudioInputMode.Unavailable, Config(folder).ReadMode());
	}

	[Fact]
	public void Both_files_enabled_is_ASIO_and_both_disabled_is_cable()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll");
		Assert.Equal(AudioInputMode.Asio, Config(folder).ReadMode());

		using var cable = new TempFolder();
		cable.Touch("avrt.dll.disabled", "RS_ASIO.dll.disabled");
		Assert.Equal(AudioInputMode.Cable, Config(cable).ReadMode());
	}

	[Fact]
	public void Half_switched_files_are_reported_not_guessed()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll.disabled");
		Assert.Throws<InvalidOperationException>(() => Config(folder).ReadMode());
	}

	[Fact]
	public void Switching_to_cable_renames_both_files_and_back_again()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll", "RS_ASIO.ini");
		var config = Config(folder);

		config.SetAsioEnabled(false);
		Assert.True(File.Exists(folder.File("avrt.dll.disabled")));
		Assert.True(File.Exists(folder.File("RS_ASIO.dll.disabled")));
		Assert.False(File.Exists(folder.File("avrt.dll")));
		Assert.False(File.Exists(folder.File("RS_ASIO.dll")));
		Assert.True(File.Exists(folder.File("RS_ASIO.ini")));   // the ini is never renamed
		Assert.Equal("avrt.dll", File.ReadAllText(folder.File("avrt.dll.disabled")));
		Assert.Equal(AudioInputMode.Cable, config.ReadMode());

		config.SetAsioEnabled(true);
		Assert.Equal(AudioInputMode.Asio, config.ReadMode());
		Assert.Equal("RS_ASIO.dll", File.ReadAllText(folder.File("RS_ASIO.dll")));
	}

	[Fact]
	public void Selecting_the_current_mode_changes_nothing()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll");
		Config(folder).SetAsioEnabled(true);
		Assert.Equal(AudioInputMode.Asio, Config(folder).ReadMode());
	}

	[Fact]
	public void Refused_while_the_game_runs()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll");
		var error = Assert.Throws<InvalidOperationException>(() => Config(folder, running: true).SetAsioEnabled(false));
		Assert.Contains("Close Rocksmith", error.Message);
		Assert.True(File.Exists(folder.File("avrt.dll")));
		Assert.True(File.Exists(folder.File("RS_ASIO.dll")));
	}

	[Fact]
	public void Refused_without_RS_ASIO()
	{
		using var folder = new TempFolder();
		Assert.Throws<InvalidOperationException>(() => Config(folder).SetAsioEnabled(true));
	}

	[Fact]
	public void A_failed_rename_is_rolled_back()
	{
		using var folder = new TempFolder();
		folder.Touch("avrt.dll", "RS_ASIO.dll");
		// RS_ASIO.dll held open without delete sharing: the first rename succeeds, the second fails.
		using (new FileStream(folder.File("RS_ASIO.dll"), FileMode.Open, FileAccess.Read, FileShare.Read))
			Assert.ThrowsAny<IOException>(() => Config(folder).SetAsioEnabled(false));
		Assert.True(File.Exists(folder.File("avrt.dll")));
		Assert.False(File.Exists(folder.File("avrt.dll.disabled")));
		Assert.Equal(AudioInputMode.Asio, Config(folder).ReadMode());
	}
}
