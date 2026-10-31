using System.Text;

namespace RocksmithAudioBridge.Core.Tests;

public class IniTextTests
{
	[Fact]
	public void Set_existing_key_rewrites_only_that_line()
	{
		const string text = "; RS_ASIO config\r\n[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=0\r\n\r\n[Asio.Output]\r\nDriver=Focusrite USB ASIO\r\nBaseChannel=2\r\n";
		var ini = IniText.Parse(text);
		ini.Set("Config", "EnableAsio", "1");
		Assert.Equal(text.Replace("EnableAsio=0", "EnableAsio=1"), ini.Text);
	}

	[Fact]
	public void Missing_key_goes_after_the_last_line_of_its_section()
	{
		var ini = IniText.Parse("[Audio]\r\nExclusiveMode=1\r\n\r\n[Renderer.Win32]\r\nScreenWidth=1920\r\n");
		ini.Set("Audio", "Win32UltraLowLatencyMode", "1");
		Assert.Equal("[Audio]\r\nExclusiveMode=1\r\nWin32UltraLowLatencyMode=1\r\n\r\n[Renderer.Win32]\r\nScreenWidth=1920\r\n", ini.Text);
	}

	[Fact]
	public void Missing_section_is_appended_after_a_blank_line()
	{
		var ini = IniText.Parse("[Config]\r\nEnableAsio=1\r\n");
		ini.Set("Asio.Input.0", "Driver", "Rocksmith Audio Bridge ASIO");
		Assert.Equal("[Config]\r\nEnableAsio=1\r\n\r\n[Asio.Input.0]\r\nDriver=Rocksmith Audio Bridge ASIO\r\n", ini.Text);
	}

	[Fact]
	public void Unix_line_endings_and_comments_are_kept()
	{
		var ini = IniText.Parse("[Audio]\n;MaxOutputBufferSize=512\nMaxOutputBufferSize=1024\n");
		Assert.Equal("1024", ini.Get("Audio", "MaxOutputBufferSize"));
		ini.Set("Audio", "MaxOutputBufferSize", "0");
		Assert.Equal("[Audio]\n;MaxOutputBufferSize=512\nMaxOutputBufferSize=0\n", ini.Text);
	}

	[Fact]
	public void Get_is_case_insensitive_and_null_when_missing()
	{
		var ini = IniText.Parse("[AUDIO]\r\n win32ultralowlatencymode = 1 \r\n");
		Assert.Equal("1", ini.Get("Audio", "Win32UltraLowLatencyMode"));
		Assert.Null(ini.Get("Audio", "ExclusiveMode"));
		Assert.Null(ini.Get("Net", "Anything"));
	}

	[Fact]
	public void Save_strips_the_byte_order_mark_RS_ASIO_cannot_read()
	{
		using var folder = new TempFolder();
		string path = folder.File("RS_ASIO.ini");
		File.WriteAllText(path, "[Config]\r\nEnableAsio=0\r\n", new UTF8Encoding(true));
		var ini = IniText.Load(path);
		Assert.Equal("0", ini.Get("Config", "EnableAsio"));
		ini.Set("Config", "EnableAsio", "1");
		ini.Save(path);
		byte[] bytes = File.ReadAllBytes(path);
		Assert.Equal((byte)'[', bytes[0]);
		Assert.Equal("[Config]\r\nEnableAsio=1\r\n", File.ReadAllText(path));
	}
}
