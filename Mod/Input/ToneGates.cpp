#include "ToneGates.hpp"

#include "InputConditioner.hpp"
#include "../Core/DataFolder.hpp"

#include <Windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace ToneGates
{
	namespace
	{
		struct CaseLess
		{
			bool operator()(const std::string& a, const std::string& b) const { return _stricmp(a.c_str(), b.c_str()) < 0; }
		};

		std::mutex g_mutex;
		Gate g_global;
		std::string g_active;                        // the playing tone, empty = global
		std::map<std::string, Gate, CaseLess> g_tones;
		bool g_loaded = false;
		uint32_t g_revision = 1;
		bool g_pushedOn = false;

		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
			return out;
		}

		std::string Narrow(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring File() { return AudioBridge::VstHostDataFolder() + L"\\ToneGates.ini"; }

		// Tone names become ini keys: '=' ends a key and ';' or '[' at the start make a comment or a section, so those
		// (and '%' itself) are written as %XX.
		std::wstring EncodeKey(const std::string& name)
		{
			std::wstring out;
			for (const wchar_t c : Wide(name))
			{
				if (c == L'%' || c == L'=' || (out.empty() && (c == L';' || c == L'['))) { wchar_t hex[4]; swprintf_s(hex, L"%%%02X", static_cast<unsigned>(c)); out += hex; }
				else out += c;
			}
			return out;
		}

		std::string DecodeKey(const std::wstring& key)
		{
			std::wstring out;
			for (size_t i = 0; i < key.size(); ++i)
			{
				if (key[i] == L'%' && i + 2 < key.size())
				{
					const wchar_t hex[3] = { key[i + 1], key[i + 2], 0 };
					wchar_t* end = nullptr;
					const long value = wcstol(hex, &end, 16);
					if (end == hex + 2) { out += static_cast<wchar_t>(value); i += 2; continue; }
				}
				out += key[i];
			}
			return Narrow(out);
		}

		Gate Clamp(Gate gate)
		{
			gate.thresholdTenths = std::clamp(gate.thresholdTenths, -800, -200);
			gate.decayMs = std::clamp(gate.decayMs, 50, 2000);
			gate.reductionDb = std::clamp(gate.reductionDb, -80, -10);
			gate.attackTenths = std::clamp(gate.attackTenths, 1, 200);
			return gate;
		}

		// "<on>,<threshold tenths>,<decay ms>,<reduction dB>,<attack tenths>"
		bool Parse(const std::wstring& value, Gate& gate)
		{
			int on = 0;
			if (swscanf_s(value.c_str(), L"%d,%d,%d,%d,%d", &on, &gate.thresholdTenths, &gate.decayMs, &gate.reductionDb, &gate.attackTenths) != 5) return false;
			gate.on = on != 0;
			gate = Clamp(gate);
			return true;
		}

		void Load()
		{
			if (g_loaded) return;
			g_loaded = true;
			std::vector<wchar_t> buffer(65536);
			const DWORD length = GetPrivateProfileSectionW(L"Tones", buffer.data(), static_cast<DWORD>(buffer.size()), File().c_str());
			for (const wchar_t* line = buffer.data(); line < buffer.data() + length && *line; line += wcslen(line) + 1)
			{
				const std::wstring entry = line;
				const size_t equals = entry.find(L'=');
				Gate gate;
				if (equals == std::wstring::npos || !Parse(entry.substr(equals + 1), gate)) continue;
				g_tones[DecodeKey(entry.substr(0, equals))] = gate;
			}
		}

		void Store(const std::string& name, const Gate* gate)
		{
			wchar_t value[64] = {};
			if (gate) swprintf_s(value, L"%d,%d,%d,%d,%d", gate->on ? 1 : 0, gate->thresholdTenths, gate->decayMs, gate->reductionDb, gate->attackTenths);
			WritePrivateProfileStringW(L"Tones", EncodeKey(name).c_str(), gate ? value : nullptr, File().c_str());
		}

		Gate Effective()
		{
			if (!g_active.empty())
			{
				const auto it = g_tones.find(g_active);
				if (it != g_tones.end()) return it->second;
			}
			return g_global;
		}

		// Into the game's input chain (and from there, every tick, the amp sim's feed). The gate is only reset when it
		// switches on (it was not running); a new threshold or tone while it runs keeps a ringing note.
		void Push(const Gate& gate)
		{
			Audio::AsioHook::Detail::NoiseGateShape shape;
			shape.decayMs = static_cast<float>(gate.decayMs);
			shape.reductionDb = static_cast<float>(gate.reductionDb);
			shape.attackMs = gate.attackTenths / 10.0f;
			AudioBridge::Input::SharedConditioner().SetNoiseGateShape(shape);
			AudioBridge::Input::SharedConditioner().SetNoiseGateThresholdDb(
				gate.on ? gate.thresholdTenths / 10.0f : 0.0f, gate.on && !g_pushedOn);
			g_pushedOn = gate.on;
			++g_revision;
		}
	}

	void InitGlobal(const Gate& global)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_global = Clamp(global);
		g_pushedOn = false;
		Push(Effective());
	}

	void SetActiveTone(const std::string& tone)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (_stricmp(tone.c_str(), g_active.c_str()) == 0) return;
		Load();
		const Gate before = Effective();
		g_active = tone;
		const Gate after = Effective();
		if (after.on != before.on || after.thresholdTenths != before.thresholdTenths || after.decayMs != before.decayMs
			|| after.reductionDb != before.reductionDb || after.attackTenths != before.attackTenths)
			Push(after);
		else ++g_revision;   // same sound, but the pedal shows another tone
	}

	View Current()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		View view;
		view.tone = g_active;
		view.own = !g_active.empty() && g_tones.count(g_active) != 0;
		view.gate = Effective();
		view.revision = g_revision;
		return view;
	}

	bool Apply(const Gate& requested, bool save)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		const Gate gate = Clamp(requested);
		const bool toTone = !g_active.empty();
		if (toTone)
		{
			g_tones[g_active] = gate;
			if (save) Store(g_active, &gate);
		}
		else g_global = gate;
		Push(gate);
		return toTone;
	}

	bool UseGlobal()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		if (g_active.empty() || g_tones.erase(g_active) == 0) return false;
		Store(g_active, nullptr);
		Push(Effective());
		return true;
	}

	void RenameTone(const std::string& from, const std::string& to)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		const auto it = g_tones.find(from);
		if (it == g_tones.end()) return;
		const Gate gate = it->second;
		g_tones.erase(it);
		g_tones[to] = gate;
		Store(from, nullptr);
		Store(to, &gate);
		if (_stricmp(g_active.c_str(), from.c_str()) == 0) g_active = to;
		++g_revision;
	}

	void DeleteTone(const std::string& name)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		if (g_tones.erase(name) == 0) return;
		Store(name, nullptr);
		if (_stricmp(g_active.c_str(), name.c_str()) == 0) Push(Effective());
		else ++g_revision;
	}

	void ToneSaved(const std::string& name)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Load();
		if (name.empty() || g_tones.count(name) != 0 || g_active.empty()) return;
		const auto it = g_tones.find(g_active);
		if (it == g_tones.end()) return;
		const Gate gate = it->second;
		g_tones[name] = gate;
		Store(name, &gate);
		++g_revision;
	}
}
