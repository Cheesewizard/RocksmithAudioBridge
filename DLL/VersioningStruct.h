#pragma once

enum class VersionType : int
{
	Start,
	RemasteredSeptember2022 = Start,
	LPDecember2024,
	Count,
	End = Count,
};

template <typename T>
class VersioningStruct
{
public:
	VersioningStruct();
	~VersioningStruct() = default;

	VersioningStruct(const std::vector<T>& versions);

	operator T();

public:
	T& Get();
	T GetValue();

private:
	static VersionType GetVersion();

private:
	std::vector<T> myVersions;
};

// True only when the running exe's checksum is exactly this build's. GetVersion falls back to September 2022 for an
// unknown exe (after a message box), so code patching fixed addresses of one build must ask this instead.
bool IsExactGameBuild(VersionType type);

template <typename T> VersioningStruct<T>::VersioningStruct()
	: myVersions(std::vector<T>(static_cast<int>(VersionType::Count), 0))
{
}

template <typename T> VersioningStruct<T>::VersioningStruct(const std::vector<T>& versions)
	: myVersions(versions)
{
	if (myVersions.size() != static_cast<size_t>(VersionType::Count))
	{
		myVersions.resize(static_cast<int>(VersionType::Count), NULL);
	}
}

template <typename T> VersioningStruct<T>::operator T()
{
	return Get();
}