# Plugin browser responsiveness fix

25 October 2026, worktree codex/portable-tone-designer.

The running diagnostic build was confirmed by installed binary hashes and the live
startup log's M0a message. Its visible interface remains the existing VST UI; all
redesigned screens are mockups. The live host log shows AmpliTube initialisation
between 16:30:57.524 and 16:31:10.311 (about 12.8 seconds), in the separate host process.
That duration alone does not establish why the game UI stopped drawing.

Source investigation found that VstHostLink::Plugins called Scan synchronously on
first use; Rescan did the same. The plugin browser invokes Plugins while drawing.
The scan traverses folders and opens VST3 metadata/PE files, so slow filesystem reads
can block the game render thread and prevent the existing loading indicator from drawing.

The replacement:

- PluginCatalog requests enumeration without doing disk work on the render thread.
- The existing background settings worker performs first scans and rescans.
- Completed results are taken with try_lock at the beginning of a frame. The plugin
  vector is stable throughout each draw, including pointers used by browser rows.
- Requests during a running scan remain pending for another scan.
- The browser shows an animated Scanning plugins label, or an explicit scan error.
- Host plugin loading continues to use its existing separate loading status.

Release Win32 build succeeded: build-async-plugin-scan.log, 4 warnings, 0 errors.
Concurrency tests compiled. Execution was blocked because Bitdefender Virus
Shield held the test executable (Windows Restart Manager reported PID 4312).
The executable is now absent; test execution has not passed.
No antivirus settings were changed. Passing execution and a live first-open/rescan
check are still required. The running game has not been interrupted.

Tests: VstHost/Tests/PluginCatalogTests.cpp. The suite gates a deliberately
blocked scanner and checks that UI polling returns immediately with its cached snapshot,
that a queued rescan survives, and that scan errors clear the pending state and reach UI.
