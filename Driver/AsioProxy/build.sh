#!/bin/bash
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
output_dir="$repo_root/build/AsioProxy"
mkdir -p "$output_dir"
msvc="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
sdk="/c/Program Files (x86)/Windows Kits/10"
sdkv="10.0.26100.0"
export INCLUDE="$(cygpath -w "$msvc/include");$(cygpath -w "$sdk/Include/$sdkv/um");$(cygpath -w "$sdk/Include/$sdkv/shared");$(cygpath -w "$sdk/Include/$sdkv/ucrt");$(cygpath -w "$sdk/Include/$sdkv/winrt")"
export LIB="$(cygpath -w "$msvc/lib/x86");$(cygpath -w "$sdk/Lib/$sdkv/um/x86");$(cygpath -w "$sdk/Lib/$sdkv/ucrt/x86")"
cl="$msvc/bin/Hostx86/x86/cl.exe"
cd "$output_dir"
MSYS_NO_PATHCONV=1 "$cl" -nologo -LD -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$script_dir/AsioProxyDriver.cpp")" "$(cygpath -w "$script_dir/VirtualCableCapture.cpp")" -Fe:RocksmithAudioBridgeAsio.dll -link -DEF:"$(cygpath -w "$script_dir/RocksmithAudioBridge.def")" ole32.lib advapi32.lib propsys.lib
