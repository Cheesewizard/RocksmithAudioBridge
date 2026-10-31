namespace RocksmithAudioBridge.Core.Tests;

/// <summary>A throwaway folder under %TEMP% standing in for the game folder. Tests never touch the real one.</summary>
public sealed class TempFolder : IDisposable
{
	public TempFolder()
	{
		Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "RabCoreTests_" + Guid.NewGuid().ToString("N"));
		Directory.CreateDirectory(Path);
	}

	public string Path { get; }

	public string File(string name) => System.IO.Path.Combine(Path, name);

	public void Touch(params string[] names)
	{
		foreach (string name in names) System.IO.File.WriteAllText(File(name), name);
	}

	public void Dispose()
	{
		try { Directory.Delete(Path, recursive: true); } catch { }
	}
}
