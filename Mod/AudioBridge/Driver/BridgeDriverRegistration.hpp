#pragma once

// Boot-time self-heal for the "Rocksmith Audio Bridge ASIO" proxy registration.
//
// RS_ASIO lists drivers from HKLM\Software\ASIO and resolves each DLL through HKEY_CLASSES_ROOT\clsid\{id}\
// InprocServer32 (the ASIO SDK asiolist code). If that entry names the wrong file (e.g. the pre-rename
// RocksmithAudioBridge.dll, which is now the managed library), RS_ASIO fails to load the driver, ends up with
// no devices at all, and Rocksmith stops at "No audio output device". HKCR merges HKCU over HKLM, so writing a
// per-user InprocServer32 that points at the shipped proxy fixes it without admin rights. Runs from the host's
// main thread at load, long before RS_ASIO enumerates (~30 s into boot). Never touches RS_ASIO.ini and never
// registers the driver when it is not already in the ASIO list (that needs the GUI's elevated Install).
namespace AsioProxyRegistration
{
	void Heal();
}
