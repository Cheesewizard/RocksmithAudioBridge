#include "stdafx.h"
#include "VersioningStruct.h"

namespace {
	static DWORD GetChecksum() {
		DWORD headerSum = 0;
		DWORD checksum = 0;

		char executable[MAX_PATH];
		GetModuleFileNameA(NULL, executable, MAX_PATH);

		MapFileAndCheckSumA(executable, &headerSum, &checksum);

		return checksum;
	}

	static const std::map<DWORD, VersionType> ourVersionChecksums
	{
		{0x00B13D7C, VersionType::RemasteredSeptember2022},
		{0x0176EC34, VersionType::LPDecember2024}
	};
}

template class VersioningStruct<unsigned int>;

bool IsExactGameBuild(VersionType type)
{
	static const DWORD checksum = GetChecksum();
	const auto it = ourVersionChecksums.find(checksum);
	return it != ourVersionChecksums.cend() && it->second == type;
}

template <typename T> T& VersioningStruct<T>::Get()
{
	return myVersions.at(static_cast<int>(GetVersion()));
}

template <typename T> T VersioningStruct<T>::GetValue()
{
	return myVersions.at(static_cast<int>(GetVersion()));
}

template <typename T> VersionType VersioningStruct<T>::GetVersion()
{
	static DWORD checksum = GetChecksum();

	static bool cachedVersion = false;
	static VersionType version;

	if (cachedVersion)
	{
		return version;
	}

	const std::map<DWORD, VersionType>::const_iterator it = ourVersionChecksums.find(checksum);
	if (it != ourVersionChecksums.cend())
	{
		version = (*it).second;
	}
	else
	{
		MessageBoxA(NULL, std::to_string(checksum).c_str(), "Unsupported Checksum!", NULL);
		version = VersionType::RemasteredSeptember2022;
	}

	cachedVersion = true;
	return version;
}