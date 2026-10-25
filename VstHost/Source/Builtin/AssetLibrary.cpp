#include "AssetLibrary.hpp"
#include "BuiltinId.hpp"
#include "BuiltinProcessor.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <NAM/get_dsp.h>
#include <NAM/wav.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VstHost::Builtin
{
	namespace
	{
		constexpr int MAX_IR_FRAMES = 8192;
		constexpr size_t MAX_ASSET_BYTES = 64 * 1024 * 1024;

		juce::MemoryBlock ReadBytes(const juce::File& file)
		{
			if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > MAX_ASSET_BYTES)
				throw std::runtime_error("Asset is missing, empty, or larger than 64 MB: " + file.getFullPathName().toStdString());
			juce::MemoryBlock bytes;
			if (!file.loadFileAsData(bytes)) throw std::runtime_error("Cannot read asset: " + file.getFullPathName().toStdString());
			return bytes;
		}

		juce::MemoryBlock NormalizeIr(const juce::File& file)
		{
			double rate = 0.0;
			int channels = 0;
			const auto samples = nam::detail::load_wav_ir(std::filesystem::path(file.getFullPathName().toWideCharPointer()), rate, channels);
			const int inputFrames = static_cast<int>(samples.size() / channels);
			const int frames = static_cast<int>(std::min<double>(MAX_IR_FRAMES, std::ceil(inputFrames * SAMPLE_RATE / rate)));
			juce::MemoryOutputStream output;
			output.write("RIFF", 4);
			output.writeInt(36 + frames * channels * 4);
			output.write("WAVEfmt ", 8);
			output.writeInt(16);
			output.writeShort(3);
			output.writeShort(static_cast<short>(channels));
			output.writeInt(static_cast<int>(SAMPLE_RATE));
			output.writeInt(static_cast<int>(SAMPLE_RATE) * channels * 4);
			output.writeShort(static_cast<short>(channels * 4));
			output.writeShort(32);
			output.write("data", 4);
			output.writeInt(frames * channels * 4);
			// Offline, band-limited windowed-sinc conversion; never runs in a callback.
			const double cutoff = std::min(1.0, SAMPLE_RATE / rate);
			constexpr double PI = 3.14159265358979323846;
			for (int frame = 0; frame < frames; ++frame)
			{
				const double position = frame * rate / SAMPLE_RATE;
				const int center = static_cast<int>(position);
				for (int channel = 0; channel < channels; ++channel)
				{
					double value = 0.0;
					if (rate == SAMPLE_RATE) value = samples[channel * inputFrames + frame];
					else
					{
						for (int tap = center - 63; tap <= center + 64; ++tap)
						{
							if (tap < 0 || tap >= inputFrames) continue;
							const double distance = position - tap;
							if (std::abs(distance) >= 64.0) continue;
							const double argument = PI * cutoff * distance;
							const double sinc = std::abs(argument) < 1.0e-12 ? 1.0 : std::sin(argument) / argument;
							const double window = 0.42 + 0.5 * std::cos(PI * distance / 64.0) + 0.08 * std::cos(2.0 * PI * distance / 64.0);
							value += samples[channel * inputFrames + tap] * cutoff * sinc * window;
						}
					}
					output.writeFloat(static_cast<float>(value * rate / SAMPLE_RATE));
				}
			}
			return output.getMemoryBlock();
		}

		float MeasureLoudness(nam::DSP& model)
		{
			constexpr int FRAMES = 128;
			float input[FRAMES]{};
			float output[FRAMES]{};
			float* inputs[] = { input };
			float* outputs[] = { output };
			double energy = 0.0;
			uint32_t noise = 0x524142;
			for (int block = 0; block < 375; ++block)
			{
				for (auto& value : input)
				{
					noise = noise * 1664525u + 1013904223u;
					value = (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f) * 0.05f;
				}
				model.process(inputs, outputs, FRAMES);
				for (float value : output)
				{
					if (!std::isfinite(value)) throw std::runtime_error("Model produces non-finite output during import.");
					energy += static_cast<double>(value) * value;
				}
			}
			return static_cast<float>(10.0 * std::log10(std::max(energy / SAMPLE_RATE, 1.0e-12)));
		}
	}

	void WriteJson(const juce::File& file, const juce::var& json)
	{
		if (file.getParentDirectory().createDirectory().failed()) throw std::runtime_error("Cannot create tone data folder.");
		juce::TemporaryFile temporary(file);
		if (!temporary.getFile().replaceWithText(juce::JSON::toString(json)) || !temporary.overwriteTargetFileWithTemporary())
			throw std::runtime_error("Cannot write " + file.getFullPathName().toStdString());
	}

	juce::var ReadJson(const juce::File& file)
	{
		if (!file.existsAsFile()) throw std::runtime_error("Missing JSON file: " + file.getFullPathName().toStdString());
		juce::var result;
		const auto parsed = juce::JSON::parse(file.loadFileAsString(), result);
		if (parsed.failed()) throw std::runtime_error(parsed.getErrorMessage().toStdString());
		return result;
	}

	juce::File AssetLibrary::DefaultFolder()
	{
		return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
			.getChildFile("RocksmithAudioBridge/Builtin/Library");
	}

	bool AssetLibrary::IsHash(const std::string& hash)
	{
		if (hash.size() != 64) return false;
		for (char character : hash)
		{
			if (!(character >= '0' && character <= '9') && !(character >= 'a' && character <= 'f')) return false;
		}
		return true;
	}

	std::string AssetLibrary::Hash(const void* data, size_t size)
	{
		if (size > std::numeric_limits<ULONG>::max()) throw std::invalid_argument("Asset is too large to hash.");
		unsigned char digest[32]{};
		if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
			static_cast<ULONG>(size), digest, sizeof(digest)) < 0) throw std::runtime_error("SHA-256 failed.");
		return juce::String::toHexString(digest, sizeof(digest), 0).toStdString();
	}

	std::unique_ptr<nam::DSP> AssetLibrary::LoadModel(const juce::File& file, AssetKind kind)
	{
		const auto bytes = ReadBytes(file);
		std::unique_ptr<nam::DSP> model;
		if (kind == AssetKind::NamModel)
		{
			const auto* first = static_cast<const char*>(bytes.getData());
			const auto json = nlohmann::json::parse(first, first + bytes.getSize());
			if (!json.contains("sample_rate") || !json["sample_rate"].is_number() || json["sample_rate"].get<double>() != SAMPLE_RATE)
				throw std::invalid_argument("NAM model must declare sample_rate 48000; other or missing rates are unsupported.");
			model = nam::get_dsp(json);
			if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
				throw std::invalid_argument("NAM amp models must have one input and one output channel.");
		}
		else
		{
			model = nam::get_dsp(std::filesystem::path(file.getFullPathName().toWideCharPointer()));
			if (model->NumInputChannels() != 1 || model->NumOutputChannels() < 1 || model->NumOutputChannels() > 2)
				throw std::invalid_argument("Cabinet IR must be mono or stereo.");
		}
		return model;
	}

	AssetLibrary::AssetLibrary(juce::File folder) : folder(std::move(folder))
	{
		const auto index = this->folder.getChildFile("library.json");
		if (!index.existsAsFile()) return;
		const auto json = ReadJson(index);
		for (const auto& entry : RequireObject(json).getProperties())
		{
			AssetInfo info;
			info.sha256 = entry.name.toString().toStdString();
			if (!IsHash(info.sha256)) throw std::runtime_error("Invalid asset hash in library.json.");
			auto& data = RequireObject(entry.value);
			const auto kind = data.getProperty("kind").toString();
			if (kind != "nam" && kind != "ir") throw std::runtime_error("Unknown asset kind in library.json.");
			info.kind = kind == "nam" ? AssetKind::NamModel : AssetKind::ImpulseResponse;
			info.displayName = data.getProperty("displayName").toString().toStdString();
			info.attribution = data.getProperty("attribution").toString().toStdString();
			info.permission = data.getProperty("permission").toString().toStdString();
			info.includesCabinet = static_cast<bool>(data.getProperty("includesCabinet"));
			info.loudnessDb = ReadNumber(entry.value, "loudnessDb", -120.0f, 120.0f);
			assets.emplace(info.sha256, std::move(info));
		}
	}

	AssetInfo AssetLibrary::Import(const juce::File& source)
	{
		auto bytes = ReadBytes(source);
		AssetInfo info;
		info.displayName = source.getFileNameWithoutExtension().toStdString();
		if (source.hasFileExtension("nam")) info.kind = AssetKind::NamModel;
		else if (source.hasFileExtension("wav"))
		{
			info.kind = AssetKind::ImpulseResponse;
			bytes = NormalizeIr(source);
		}
		else throw std::invalid_argument("Import a .nam model or a .wav cabinet IR.");
		info.sha256 = Hash(bytes.getData(), bytes.getSize());
		if (auto* existing = Find(info.sha256))
		{
			Verify(info.sha256);
			return *existing;
		}
		if (folder.createDirectory().failed()) throw std::runtime_error("Cannot create the managed asset library.");
		const auto destination = folder.getChildFile(info.sha256 + (info.kind == AssetKind::NamModel ? ".nam" : ".wav"));
		juce::TemporaryFile temporary(destination);
		if (!temporary.getFile().replaceWithData(bytes.getData(), bytes.getSize())) throw std::runtime_error("Cannot store imported asset.");
		// get_dsp dispatches WAV by extension; NAM is loaded as JSON from memory.
		const auto validationFile = info.kind == AssetKind::NamModel ? temporary.getFile() : source;
		auto model = LoadModel(validationFile, info.kind);
		model->Reset(SAMPLE_RATE, 4096);
		if (info.kind == AssetKind::NamModel)
		{
			info.loudnessDb = model->HasLoudness() ? static_cast<float>(model->GetLoudness()) : MeasureLoudness(*model);
			if (!std::isfinite(info.loudnessDb) || info.loudnessDb < -120.0f || info.loudnessDb > 120.0f)
				throw std::invalid_argument("Model loudness metadata is outside the supported range.");
		}
		if (!temporary.overwriteTargetFileWithTemporary()) throw std::runtime_error("Cannot finish importing asset.");
		assets.emplace(info.sha256, info);
		try { Save(); }
		catch (...) { assets.erase(info.sha256); throw; }
		return info;
	}

	juce::File AssetLibrary::PathOf(const std::string& hash) const
	{
		const auto* info = Find(hash);
		if (!info) throw std::invalid_argument("Asset is not in the managed library: " + hash);
		return folder.getChildFile(hash + (info->kind == AssetKind::NamModel ? ".nam" : ".wav"));
	}

	const AssetInfo* AssetLibrary::Find(const std::string& hash) const
	{
		if (!IsHash(hash)) throw std::invalid_argument("Asset hash must contain 64 lowercase hexadecimal characters.");
		const auto found = assets.find(hash);
		return found == assets.end() ? nullptr : &found->second;
	}

	std::vector<AssetInfo> AssetLibrary::List() const
	{
		std::vector<AssetInfo> result;
		for (const auto& entry : assets) result.push_back(entry.second);
		return result;
	}

	void AssetLibrary::SetRights(const std::string& hash, const std::string& attribution, const std::string& permission)
	{
		if (!Find(hash)) throw std::invalid_argument("Cannot set rights for an unknown asset.");
		auto& asset = assets.at(hash);
		const auto previous = asset;
		asset.attribution = attribution;
		asset.permission = permission;
		try { Save(); }
		catch (...) { asset = previous; throw; }
	}

	void AssetLibrary::Verify(const std::string& hash) const
	{
		const auto bytes = ReadBytes(PathOf(hash));
		if (Hash(bytes.getData(), bytes.getSize()) != hash) throw std::runtime_error("Asset SHA-256 mismatch: " + hash);
	}

	void AssetLibrary::Save()
	{
		juce::var json(new juce::DynamicObject());
		for (const auto& entry : assets)
		{
			const auto& info = entry.second;
			juce::var data(new juce::DynamicObject());
			auto& object = *data.getDynamicObject();
			object.setProperty("kind", info.kind == AssetKind::NamModel ? "nam" : "ir");
			object.setProperty("displayName", juce::String(info.displayName));
			object.setProperty("attribution", juce::String(info.attribution));
			object.setProperty("permission", juce::String(info.permission));
			object.setProperty("includesCabinet", info.includesCabinet);
			object.setProperty("loudnessDb", info.loudnessDb);
			json.getDynamicObject()->setProperty(juce::Identifier(juce::String(entry.first)), data);
		}
		WriteJson(folder.getChildFile("library.json"), json);
	}
}
