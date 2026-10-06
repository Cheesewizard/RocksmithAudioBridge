using System.IO;
using System.Windows.Forms;

namespace RS2014_Mod_Installer
{
    class DLLStuff
    {
        // Every file the installer writes is recorded and saved to InstallManifestName, so Uninstall removes exactly
        // what was installed (and nothing else in the game folder) even as the file list changes between versions.
        public const string InstallManifestName = "RSModsPlus-installed-files.txt";
        private static readonly System.Collections.Generic.List<string> writtenFiles = new System.Collections.Generic.List<string>();

        private static void WriteFile(string path, byte[] data) { File.WriteAllBytes(path, data); Record(path); }
        private static void WriteTextFile(string path, string text) { File.WriteAllText(path, text); Record(path); }
        private static void Record(string path)
        {
            if (!writtenFiles.Exists(p => string.Equals(p, path, System.StringComparison.OrdinalIgnoreCase))) writtenFiles.Add(path);
        }

        public static string InstallManifestPath(string rocksmithLocation) => Path.Combine(rocksmithLocation, "RSMods", InstallManifestName);

        // Merges with an existing manifest (a repair over an older install keeps what that one wrote), stored as
        // paths relative to the game folder.
        public static void SaveInstallManifest(string rocksmithLocation)
        {
            var root = Path.GetFullPath(rocksmithLocation).TrimEnd('\\') + "\\";
            var lines = new System.Collections.Generic.SortedSet<string>(System.StringComparer.OrdinalIgnoreCase);
            foreach (var existing in ReadInstallManifest(rocksmithLocation)) lines.Add(existing);
            foreach (var path in writtenFiles)
            {
                var full = Path.GetFullPath(path);
                if (full.StartsWith(root, System.StringComparison.OrdinalIgnoreCase)) lines.Add(full.Substring(root.Length));
            }
            lines.Add(Path.Combine("RSMods", InstallManifestName));
            File.WriteAllLines(InstallManifestPath(rocksmithLocation), lines);
        }

        public static string[] ReadInstallManifest(string rocksmithLocation)
        {
            var path = InstallManifestPath(rocksmithLocation);
            return File.Exists(path) ? File.ReadAllLines(path) : new string[0];
        }

        public static bool InjectDLL(string rocksmithLocation)
        {
            try
            {
                WriteFile(Path.Combine(@rocksmithLocation, "xinput1_3.dll"), Properties.Resources.xinput1_3);

                // The Rocksmith Audio Bridge ASIO driver. Written here and registered by the installer window once
                // RSMods.exe is in place (Uninstaller.RunDriverInstall), so RS_ASIO can use it on first launch.
                WriteFile(Path.Combine(@rocksmithLocation, "RocksmithAudioBridgeAsio.dll"), Properties.Resources.RocksmithAudioBridge);
                // Its 64-bit build, which lets any 64-bit ASIO host (amp sims, DAWs) join the External amp link.
                WriteFile(Path.Combine(@rocksmithLocation, "RocksmithAudioBridgeAsio64.dll"), Properties.Resources.RocksmithAudioBridgeGuest64);

                if (File.Exists(Path.Combine(@rocksmithLocation, "D3DX9_42.dll")) && new FileInfo(Path.Combine(@rocksmithLocation, "D3DX9_42.dll")).Length >= 300000)
                    File.Delete(Path.Combine(@rocksmithLocation, "D3DX9_42.dll"));

                return true;
            }
            catch (System.UnauthorizedAccessException ex)
            {
                MessageBox.Show("Windows refused to write a file in the game folder (" + ex.Message + ").\nIf your antivirus blocked it, allow the installer and press this button again.", "Error: access denied", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
            catch (IOException)
            {
                MessageBox.Show("Please close Rocksmith, then press this button again.\nWe cannot create the necessary files while the game is open.", "Error: Rocksmith is open", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        public static bool InjectGUI(string rocksmithLocation)
        {
            // RSMods.exe lives in <game>\RSMods with its data folders (CustomMods, Temp), as in upstream RSMods. Its
            // third-party libraries and the toolkit helpers (tools, ddc, 7-Zip) are bundled inside RSMods.exe and
            // unpacked to %LOCALAPPDATA% at startup (RuntimeBootstrap), so none of them are written here.
            string rootModFolder = Path.Combine(rocksmithLocation, "RSMods");
            string customModsFolder = Path.Combine(rootModFolder, "CustomMods");

            Directory.CreateDirectory(rootModFolder);
            Directory.CreateDirectory(customModsFolder);
            try
            {
                // Earlier 4.0 release candidates put the settings app in the game folder itself.
                // Debug symbols are not shipped; remove the ones earlier versions installed.
                foreach (var stale in new[] { "RSMods.exe", "RSMods.exe.config", "RocksmithAudioBridge.dll", "RSMods.pdb", "xinput1_3.pdb", @"RSMods\RSMods.pdb" })
                {
                    var stalePath = Path.Combine(rocksmithLocation, stale);
                    if (File.Exists(stalePath)) File.Delete(stalePath);
                }

                WriteFile(Path.Combine(rootModFolder, "RSMods.exe"), Properties.Resources.RSMods);
                WriteTextFile(Path.Combine(rootModFolder, "RSMods.exe.config"), Properties.Resources.RSMods_exe);
                // The managed library RSMods.exe loads from its own folder at startup; without it the app cannot open.
                WriteFile(Path.Combine(rootModFolder, "RocksmithAudioBridge.dll"), Properties.Resources.RocksmithAudioBridgeLibrary);
                WriteTextFile(Path.Combine(rootModFolder, "LICENSE.txt"), Properties.Resources.ProjectLicense);
                WriteTextFile(Path.Combine(rootModFolder, "NOTICE.txt"), Properties.Resources.ProjectNotice);
            }
            catch (System.UnauthorizedAccessException ex)
            {
                MessageBox.Show("Windows refused to write a file in the game folder (" + ex.Message + ").\nIf your antivirus blocked it, allow the installer and press this button again.", "Error: access denied", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
            catch (IOException ex)
            {

                if (ex.Message.Contains("cannot access"))
                    MessageBox.Show("Please make sure you don't have an instance of RSMods already running!", "Error");
                else
                    MessageBox.Show($"Error: {ex.Message}");

                return false;
            }

            return true;
        }
    }
} 
