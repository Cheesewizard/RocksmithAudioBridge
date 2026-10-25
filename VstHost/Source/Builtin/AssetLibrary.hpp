#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nam { class DSP; }

namespace VstHost::Builtin
{
	enum class AssetKind { NamModel, ImpulseResponse };

	struct AssetInfo
	{
		std::string sha256;
		AssetKind kind = AssetKind::NamModel;
		std::string displayName;
		std::string attribution;
		std::string permission;
		bool includesCabinet = false;
		float loudnessDb = 0.0f;
	};

	class AssetLibrary
	{
	public:
		explicit AssetLibrary(juce::File folder = DefaultFolder());

		static juce::File DefaultFolder();
		static bool IsHash(const std::string& hash);
		static std::string Hash(const void* data, size_t size);
		static std::unique_ptr<nam::DSP> LoadModel(const juce::File& file, AssetKind kind);

		AssetInfo Import(const juce::File& source);
		juce::File PathOf(const std::string& hash) const;
		const AssetInfo* Find(const std::string& hash) const;
		std::vector<AssetInfo> List() const;
		void SetRights(const std::string& hash, const std::string& attribution, const std::string& permission);
		void Verify(const std::string& hash) const;

	private:
		void Save();

		juce::File folder;
		std::map<std::string, AssetInfo> assets;
	};

	void WriteJson(const juce::File& file, const juce::var& json);
	juce::var ReadJson(const juce::File& file);
}
