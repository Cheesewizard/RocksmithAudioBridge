namespace RSMods.Audio
{
	internal sealed class AudioDeviceChoice
	{
		public string Id { get; }
		public string Name { get; }

		public AudioDeviceChoice(string id, string name)
		{
			Id = id;
			Name = name;
		}

		public override string ToString()
		{
			return Name;
		}
	}
}
