using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Microsoft.Win32;

namespace RS2014_Mod_Installer
{
    // Removes Rocksmith Audio Bridge from a game folder. Recordings are never touched. Settings (RSMods.ini,
    // AudioRouting.ini, the bridge's HKCU keys) are removed only when asked. Order matters:
    //   1. driver registration (one elevation prompt; cancelling stops the uninstall before anything is removed),
    //   2. the Note by Note row, removed surgically from the CURRENT cache.psarc by RSMods.exe (needs our files),
    //   3. our files (install manifest, plus the core files for installs made before the manifest existed),
    //      then only folders that end up empty, and the extracted runtime cache in %LOCALAPPDATA%.
    internal static class Uninstaller
    {
        private const string ProxyName = "Rocksmith Audio Bridge ASIO";
        private const string ProxyClsid = "{7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}";

        // Core files written by every version (the fallback when there is no install manifest).
        private static readonly string[] CoreFiles =
        {
            "xinput1_3.dll", "xinput1_3.pdb", "RocksmithAudioBridgeAsio.dll", "RocksmithAudioBridgeAsio64.dll", "RocksmithAudioBridge.dll",
            "RSMods.exe", "RSMods.exe.config", "RSMods.pdb", "NoteByNoteMenuInstall.log", ".rsmods-audio-mode.lock",
        };
        private static readonly string[] SettingsFiles = { "RSMods.ini", "AudioRouting.ini" };

        public static bool IsInstalled(string rocksmithLocation)
        {
            return !string.IsNullOrEmpty(rocksmithLocation)
                && (File.Exists(Path.Combine(rocksmithLocation, "RocksmithAudioBridge.dll"))
                    || File.Exists(DLLStuff.InstallManifestPath(rocksmithLocation)));
        }

        public static bool IsDriverRegistered()
        {
            try
            {
                using (var key = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32).OpenSubKey(@"Software\ASIO\" + ProxyName))
                    return key != null;
            }
            catch { return false; }
        }

        // Deletes exactly the keys the proxy's DllRegisterServer writes, through one elevated reg.exe run (works
        // with the DLL present or missing). Throws OperationCanceledException if the prompt is declined.
        public static void RemoveDriverRegistration()
        {
            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            string reg = Path.Combine(system, "reg.exe");
            string command = "/c \"\"" + reg + "\" delete \"HKLM\\SOFTWARE\\ASIO\\" + ProxyName + "\" /f /reg:32 >nul 2>&1 & \""
                + reg + "\" delete \"HKLM\\SOFTWARE\\Classes\\CLSID\\" + ProxyClsid + "\" /f /reg:32 >nul 2>&1 & \""
                // The 64-bit registration of the External amp link's guest driver, same keys in the 64-bit view.
                + reg + "\" delete \"HKLM\\SOFTWARE\\ASIO\\" + ProxyName + "\" /f /reg:64 >nul 2>&1 & \""
                + reg + "\" delete \"HKLM\\SOFTWARE\\Classes\\CLSID\\" + ProxyClsid + "\" /f /reg:64 >nul 2>&1\"";
            var info = new ProcessStartInfo(Path.Combine(system, "cmd.exe"), command)
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            try
            {
                using (var process = Process.Start(info)) process.WaitForExit();
            }
            catch (Win32Exception error) when (error.NativeErrorCode == 1223)
            {
                throw new OperationCanceledException("Removing the audio bridge driver needs Windows admin approval, and the prompt was declined.");
            }
            if (IsDriverRegistered()) throw new InvalidOperationException("The audio bridge driver registration could not be removed.");
            // The game DLL may have written a per-user override; it wins over the machine entry for 32-bit hosts.
            try
            {
                using (var classes = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(@"Software\Classes\CLSID", writable: true))
                    classes?.DeleteSubKeyTree(ProxyClsid, throwOnMissingSubKey: false);
            }
            catch { }
        }

        // Runs the installed RSMods.exe to remove (or add) our Riff Repeater menu entry. Returns null on success,
        // otherwise a short reason.
        public static string RunMenuTool(string rocksmithLocation, bool add)
        {
            string exe = Path.Combine(rocksmithLocation, "RSMods.exe");
            if (!File.Exists(exe)) return "RSMods.exe is missing";
            var info = new ProcessStartInfo(exe, (add ? "--install-note-by-note-menu" : "--uninstall-note-by-note-menu") + " \"" + rocksmithLocation.TrimEnd('\\') + "\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = rocksmithLocation,
            };
            using (var process = Process.Start(info))
            {
                if (!process.WaitForExit(180000)) { try { process.Kill(); } catch { } return "it did not finish within 3 minutes"; }
                return process.ExitCode == 0 ? null : "see NoteByNoteMenuInstall.log in the game folder";
            }
        }

        // Deletes our files and returns the ones that could not be removed.
        public static List<string> RemoveFiles(string rocksmithLocation, bool removeSettings, bool menuEntryRemoved)
            => RemoveFiles(rocksmithLocation, removeSettings, menuEntryRemoved,
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RSModsPlus"),
                () => Registry.CurrentUser.DeleteSubKeyTree(@"Software\RSMods\AsioProxy", throwOnMissingSubKey: false));

        // appData is %LOCALAPPDATA%\RSModsPlus (a parameter so it can be exercised against a scratch folder).
        internal static List<string> RemoveFiles(string rocksmithLocation, bool removeSettings, bool menuEntryRemoved, string appData,
            Action removeUserSettingsKey)
        {
            var failed = new List<string>();
            var root = Path.GetFullPath(rocksmithLocation);
            var relative = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var line in DLLStuff.ReadInstallManifest(rocksmithLocation)) if (!string.IsNullOrWhiteSpace(line)) relative.Add(line.Trim());
            foreach (var core in CoreFiles) relative.Add(core);
            // The recovery copy of cache.psarc is only dropped once our menu entry is gone; otherwise it is kept.
            if (menuEntryRemoved) relative.Add("cache.pre-note-by-note-menu.psarc");
            if (removeSettings) foreach (var settings in SettingsFiles) relative.Add(settings);

            foreach (var rel in relative)
            {
                var full = Path.GetFullPath(Path.Combine(root, rel));
                if (!full.StartsWith(root.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase)) continue;   // never outside the game folder
                if (!File.Exists(full)) continue;
                try { File.Delete(full); } catch { failed.Add(rel); }
            }

            // Only folders that are now empty: the RSMods folder can hold the player's own files (custom mods, backups).
            foreach (var folder in new[] { @"RSMods\ddc", @"RSMods\tools", @"RSMods\CustomMods", "RSMods" })
            {
                var full = Path.Combine(root, folder);
                try { if (Directory.Exists(full) && !Directory.EnumerateFileSystemEntries(full).Any()) Directory.Delete(full); } catch { }
            }

            // Runtime extracted by RSMods.exe (ours alone), and logs with the settings.
            try { var runtime = Path.Combine(appData, "Runtime"); if (Directory.Exists(runtime)) Directory.Delete(runtime, true); } catch { failed.Add(@"%LOCALAPPDATA%\RSModsPlus\Runtime"); }
            if (removeSettings)
            {
                try { var logs = Path.Combine(appData, "Logs"); if (Directory.Exists(logs)) Directory.Delete(logs, true); } catch { }
                try { if (Directory.Exists(appData) && !Directory.EnumerateFileSystemEntries(appData).Any()) Directory.Delete(appData); } catch { }
                try { removeUserSettingsKey(); } catch { }
            }

            // The optional desktop shortcut the installer made, only if it points at this install.
            try
            {
                var shortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "RSMods.url");
                if (File.Exists(shortcut) && File.ReadAllText(shortcut).IndexOf(Path.Combine(root, "RSMods.exe").Replace('\\', '/'), StringComparison.OrdinalIgnoreCase) >= 0)
                    File.Delete(shortcut);
            }
            catch { }
            return failed;
        }

        // RSMods.exe instances started from this game folder (the settings window or a hidden helper).
        public static bool IsOurToolRunning(string rocksmithLocation)
        {
            var exe = Path.GetFullPath(Path.Combine(rocksmithLocation, "RSMods.exe"));
            foreach (var process in Process.GetProcessesByName("RSMods"))
            {
                try { if (string.Equals(process.MainModule?.FileName, exe, StringComparison.OrdinalIgnoreCase)) return true; }
                catch { return true; }   // cannot inspect it: assume it may be ours and ask the user to close it
                finally { process.Dispose(); }
            }
            return false;
        }
    }
}
