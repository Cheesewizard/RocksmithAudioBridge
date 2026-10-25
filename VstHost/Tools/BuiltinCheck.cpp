#include "BuiltinCheck.hpp"
#include "../Source/Builtin/BuiltinId.hpp"
#include "../Source/Builtin/BuiltinRegistry.hpp"
#include "../Source/Builtin/BuiltinTone.hpp"
#include "../Source/Chain.hpp"
#include "../Source/SourceRouter.hpp"

#include <shellapi.h>
#include <NAM/dsp.h>
#include <cmath>
#include <stdexcept>

namespace VstHost::Tools
{
	namespace
	{
		using namespace Builtin;

		void Check(bool condition, const char* message)
		{
			if (!condition) throw std::runtime_error(message);
		}

		template <typename Action>
		void ExpectRejection(Action action, const char* message)
		{
			try { action(); }
			catch (const std::exception&) { return; }
			throw std::runtime_error(message);
		}

		void WriteImpulse(const juce::File& file, int channels, int sampleRate)
		{
			constexpr int FRAMES = 9000;
			juce::MemoryOutputStream output;
			output.write("RIFF", 4);
			output.writeInt(36 + FRAMES * channels * 4);
			output.write("WAVEfmt ", 8);
			output.writeInt(16);
			output.writeShort(3);
			output.writeShort(static_cast<short>(channels));
			output.writeInt(sampleRate);
			output.writeInt(sampleRate * channels * 4);
			output.writeShort(static_cast<short>(channels * 4));
			output.writeShort(32);
			output.write("data", 4);
			output.writeInt(FRAMES * channels * 4);
			for (int frame = 0; frame < FRAMES; ++frame)
			{
				for (int channel = 0; channel < channels; ++channel)
					output.writeFloat(frame == 0 ? (channel == 0 ? 0.5f : 0.25f) : 0.0f);
			}
			Check(file.replaceWithData(output.getData(), output.getDataSize()), "Cannot write IR fixture.");
		}

		ToneBlock MakeModelBlock(const char* id, const std::string& hash)
		{
			auto state = NewState();
			state.getDynamicObject()->setProperty("asset", juce::String(hash));
			if (std::string(id) == "builtin:nam")
			{
				state.getDynamicObject()->setProperty("inputDb", 0.0);
				state.getDynamicObject()->setProperty("outputDb", -6.0);
				state.getDynamicObject()->setProperty("slimmable", 1.0);
			}
			else state.getDynamicObject()->setProperty("mix", 1.0);
			return { std::wstring(juce::String(id).toWideCharPointer()), false, state };
		}
	}

	class DoubleChainSink final : public AudioSink
	{
	public:
		DoubleChainSink(Chain& first, Chain& second, int frames)
			: first(first), second(second), left(frames), right(frames) { }

		void Process(const float* guitar, float* outputLeft, float* outputRight, int frames) override
		{
			first.Process(guitar, outputLeft, outputRight, frames);
			second.Process(guitar, left.data(), right.data(), frames);
		}

	private:
		Chain& first;
		Chain& second;
		std::vector<float> left;
		std::vector<float> right;
	};

	bool TryRunBuiltinCheck(int& exitCode)
	{
		int count = 0;
		auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
		if (!arguments) return false;
		const bool requested = count >= 2 && std::wstring(arguments[1]) == L"--portable-self-test";
		std::vector<std::wstring> args;
		for (int index = 2; index < count; ++index) args.emplace_back(arguments[index]);
		LocalFree(arguments);
		if (!requested) return false;
		exitCode = 1;
		if (args.empty()) return true;
		const juce::File folder(args[0].c_str());
		juce::var report(new juce::DynamicObject());
		auto& result = *report.getDynamicObject();
		try
		{
			Check(folder.createDirectory().wasOk(), "Cannot create isolated test folder.");
			// Every file and library used by this command is isolated from the player's setup.
			const auto source = folder.getChildFile("source");
			Check(source.createDirectory().wasOk(), "Cannot create fixture folder.");
			auto library = std::make_shared<AssetLibrary>(folder.getChildFile("Library"));
			Check(AssetLibrary::Hash("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known-answer test failed.");
			const auto modelFile = source.getChildFile("amp.nam");
			const juce::String modelJson = R"({"version":"0.5.4","architecture":"Linear","sample_rate":48000,"config":{"receptive_field":1,"bias":false},"weights":[0.5],"metadata":{"loudness":-18.0}})";
			Check(modelFile.replaceWithText(modelJson), "Cannot create NAM fixture.");
			const auto amp = library->Import(modelFile);
			Check(library->Import(modelFile).sha256 == amp.sha256, "Asset deduplication failed.");
			for (const auto& replacement : { std::make_pair(juce::String("48000"), juce::String("44100")),
				std::make_pair(juce::String("Linear"), juce::String("UnrecognisedModel")) })
			{
				Check(modelFile.replaceWithText(modelJson.replace(replacement.first, replacement.second)), "Cannot create invalid NAM fixture.");
				ExpectRejection([&] { library->Import(modelFile); }, "Invalid NAM was accepted.");
			}
			Check(modelFile.replaceWithText(modelJson.replace("\"receptive_field\":1", "\"in_channels\":2,\"out_channels\":2,\"receptive_field\":1")), "Cannot create multichannel NAM fixture.");
			ExpectRejection([&] { library->Import(modelFile); }, "Multichannel NAM was accepted.");
			Check(modelFile.replaceWithText(modelJson.replace("\"sample_rate\":48000,", "")), "Cannot create missing-rate fixture.");
			ExpectRejection([&] { library->Import(modelFile); }, "NAM without a sample rate was accepted.");
			const auto irFile = source.getChildFile("cab.wav");
			WriteImpulse(irFile, 2, 48000);
			const auto cab = library->Import(irFile);
			Check(library->PathOf(cab.sha256).getSize() == 44 + 8192 * 2 * 4, "IR was not truncated to 8192 stereo float32 frames.");
			WriteImpulse(irFile, 1, 44100);
			const auto resampled = library->Import(irFile);
			auto irModel = AssetLibrary::LoadModel(library->PathOf(resampled.sha256), AssetKind::ImpulseResponse);
			Check(irModel->GetExpectedSampleRate() == SAMPLE_RATE, "IR import did not resample to 48000 Hz.");
			library->SetRights(amp.sha256, "Synthetic test fixture", "author-original");
			AssetLibrary reloaded(folder.getChildFile("Library"));
			Check(reloaded.Find(amp.sha256)->permission == "author-original", "Asset rights were not persisted.");
			Check(modelFile.deleteFile() && irFile.deleteFile(), "Cannot remove fixture source files.");
			library->Verify(amp.sha256);
			library->Verify(cab.sha256);

			for (const auto& id : BuiltinRegistry::List())
			{
				if (id == L"builtin:nam" || id == L"builtin:ir") continue;
				auto processor = BuiltinRegistry::Create(id, library);
				const auto state = processor->SaveParameters();
				processor->LoadParameters(state);
				processor->prepareToPlay(SAMPLE_RATE, 128);
				juce::AudioBuffer<float> buffer(2, 128);
				buffer.clear();
				buffer.setSample(0, 0, 0.1f);
				juce::MidiBuffer midi;
				processor->processBlock(buffer, midi);
				for (int channel = 0; channel < 2; ++channel)
				{
					for (int frame = 0; frame < 128; ++frame) Check(std::isfinite(buffer.getSample(channel, frame)), "Airwindows produced non-finite output.");
				}
				auto invalid = state.clone();
				invalid.getDynamicObject()->setProperty("version", 99);
				ExpectRejection([&] { processor->LoadParameters(invalid); }, "Unknown algorithm version was accepted.");
			}
			ExpectRejection([&] { BuiltinRegistry::Create(L"builtin:airwindows/Unknown"); }, "Unknown Airwindows id was accepted.");

			BuiltinTone tone;
			tone.blocks.push_back(MakeModelBlock("builtin:nam", amp.sha256));
			tone.blocks.push_back(MakeModelBlock("builtin:ir", cab.sha256));
			const auto toneFile = folder.getChildFile("fixture.rabtone");
			tone.Write(toneFile);
			Chain chain(true, library);
			std::string error;
			Check(chain.LoadBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error), error.c_str());
			Check(chain.Prepare(128, error), error.c_str());
			std::vector<float> input(128, 0.1f), left(128), right(128);
			chain.Process(input.data(), left.data(), right.data(), 128);
			Check(std::abs(left[127] - 0.012529681f) < 1.0e-5f && std::abs(right[127] - left[127] * 0.5f) < 1.0e-5f, "NAM/IR stereo render differs from analytic fixture.");
			Check(chain.SaveBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error), error.c_str());
			Chain restored(true, library);
			Check(restored.LoadBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error) && restored.Prepare(128, error), error.c_str());
			restored.Process(input.data(), left.data(), right.data(), 128);
			Check(std::abs(left[127] - 0.012529681f) < 1.0e-5f, "Portable tone round trip changed its sound.");
			Check(restored.Duplicate(0, error), error.c_str());
			restored.Remove(1);
			Check(restored.SaveBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error), error.c_str());
			Check(!restored.Add(L"C:\\not-a-plugin.vst3", error), "Built-in chain accepted a VST path.");
			Chain vst;
			Check(!vst.Add(L"builtin:airwindows/Drive", error), "VST chain accepted a built-in block.");
			result.setProperty("validation", "passed: SHA-256, model refusals, IR resampling/truncation, deduplication, managed copies, all effects, version refusals, analytic stereo render, tone round trip, source separation");

			if (args.size() > 1)
			{
				const auto benchmarkAmp = library->Import(juce::File(args[1].c_str()));
				tone.blocks[0] = MakeModelBlock("builtin:nam", benchmarkAmp.sha256);
				result.setProperty("benchmarkModel", juce::String(benchmarkAmp.sha256));
			}
			else result.setProperty("benchmarkModel", "synthetic Linear fixture; not representative of neural amp CPU cost");
			float quality = 1.0f;
			if (args.size() > 2)
			{
				size_t consumed = 0;
				quality = std::stof(args[2], &consumed);
				Check(consumed == args[2].size() && std::isfinite(quality) && quality >= 0.0f && quality <= 1.0f, "Benchmark quality must be between 0 and 1.");
				tone.blocks[0].state.getDynamicObject()->setProperty("slimmable", quality);
			}
			result.setProperty("slimmable", quality);
			for (const auto* effect : { L"builtin:airwindows/Chamber2", L"builtin:airwindows/TapeDelay2" })
			{
				auto processor = BuiltinRegistry::Create(effect, library);
				tone.blocks.push_back({ effect, false, processor->SaveParameters() });
			}
			tone.Write(toneFile);
			juce::Array<juce::var> timings;
			bool cpuBudgetPassed = true;
			for (int frames : { 64, 128 })
			{
				Chain first(true, library), second(true, library);
				Check(first.LoadBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error) && first.Prepare(frames, error), error.c_str());
				Check(second.LoadBuiltinTone(toneFile.getFullPathName().toWideCharPointer(), error) && second.Prepare(frames, error), error.c_str());
				DoubleChainSink doubled(first, second, frames);
				for (bool twice : { false, true })
				{
					SourceRouter router;
					router.SetSource(twice ? static_cast<AudioSink*>(&doubled) : &first, true);
					for (int block = 0; block < 20000; ++block)
					{
						for (int frame = 0; frame < frames; ++frame) input[frame] = 0.1f * std::sin((block * frames + frame) * 0.031f);
						router.Process(input.data(), left.data(), right.data(), frames);
					}
					Check(router.BlockCounter() == 20000, "Router block acknowledgement counter is incorrect.");
					Check(std::isfinite(router.LimiterReduction()), "Limiter meter is non-finite.");
					const auto timing = router.TakeTiming();
					juce::var entry(new juce::DynamicObject());
					auto& data = *entry.getDynamicObject();
					data.setProperty("frames", frames);
					data.setProperty("graphs", twice ? 2 : 1);
					data.setProperty("blocks", static_cast<juce::int64>(timing.blocks));
					data.setProperty("p50Us", timing.medianMicroseconds);
					data.setProperty("p99Us", timing.percentile99Microseconds);
					data.setProperty("p999Us", timing.percentile999Microseconds);
					data.setProperty("maxUs", timing.maximumMicroseconds);
					const double period = frames * 1000000.0 / SAMPLE_RATE;
					data.setProperty("blockPeriodUs", period);
					data.setProperty("withinHalfBlockBudget", timing.percentile999Microseconds < period * 0.5);
					cpuBudgetPassed = cpuBudgetPassed && timing.percentile999Microseconds < period * 0.5;
					timings.add(entry);
				}
			}
			result.setProperty("timing", timings);
			result.setProperty("cpuBudgetPassed", cpuBudgetPassed);
			result.setProperty("timingScope", "Offline, one or two graphs (NAM, stereo 8192-sample IR, Chamber2, TapeDelay2). No tail pool, no AmpLink deadline acceptance. Histogram upper bounds at 2 microsecond resolution.");
			result.setProperty("status", "passed");
			exitCode = 0;
		}
		catch (const std::exception& exception)
		{
			result.setProperty("status", "failed");
			result.setProperty("error", exception.what());
		}
		WriteJson(folder.getChildFile("report.json"), report);
		return true;
	}
}
