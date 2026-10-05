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

                // The Rocksmith Audio Bridge proxy ASIO driver (used for wet recording). Place the file next to
                // the host so it is present on disk, but do NOT register it here: registration is the user's
                // on-demand opt-in from the audio bridge GUI when they open a feature that needs it. Placing the
                // file at install (instead of the GUI writing it out at runtime) keeps this out of dropper-style
                // antivirus heuristics.
                WriteFile(Path.Combine(@rocksmithLocation, "RocksmithAudioBridgeAsio.dll"), Properties.Resources.RocksmithAudioBridge);
                // Its 64-bit build, for 64-bit amp sims (AmpliTube 5) on the External amp link. Registered by the
                // same driver Install / Repair in RSMods, never here.
                WriteFile(Path.Combine(@rocksmithLocation, "RocksmithAudioBridgeAsio64.dll"), Properties.Resources.RocksmithAudioBridgeGuest64);

                if (File.Exists(Path.Combine(@rocksmithLocation, "D3DX9_42.dll")) && new FileInfo(Path.Combine(@rocksmithLocation, "D3DX9_42.dll")).Length >= 300000)
                    File.Delete(Path.Combine(@rocksmithLocation, "D3DX9_42.dll"));

                return true;
            }
            catch (IOException)
            {
                MessageBox.Show("Please close Rocksmith, then press this button again.\nWe cannot create the necessary files while the game is open.", "Error: Rocksmith is open", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        public static bool InjectGUI(string rocksmithLocation)
        {
            string rootModFolder = Path.Combine(rocksmithLocation, "RSMods");
            string customModsFolder = Path.Combine(rootModFolder, "CustomMods");
            string ddcFolder = Path.Combine(rootModFolder, "ddc");
            string toolsFolder = Path.Combine(rootModFolder, "tools");

            Directory.CreateDirectory(rootModFolder);
            Directory.CreateDirectory(customModsFolder);
            Directory.CreateDirectory(ddcFolder);
            Directory.CreateDirectory(toolsFolder);
            try
            {
                // Root Folder
                WriteFile(Path.Combine(rootModFolder, "7z.dll"), Properties.Resources._7z);
                WriteFile(Path.Combine(rootModFolder, "7z64.dll"), Properties.Resources._7z64);
                WriteFile(Path.Combine(rootModFolder, "BouncyCastle.Crypto.dll"), Properties.Resources.BouncyCastle_Crypto);
                WriteFile(Path.Combine(rootModFolder, "ICSharpCode.SharpZipLib.dll"), Properties.Resources.ICSharpCode_SharpZipLib);
                WriteFile(Path.Combine(rootModFolder, "Microsoft.Extensions.Logging.Abstractions.dll"), Properties.Resources.Microsoft_Extensions_Logging_Abstractions);
                WriteFile(Path.Combine(rootModFolder, "Microsoft.Win32.Registry.dll"), Properties.Resources.Microsoft_Win32_Registry);
                WriteFile(Path.Combine(rootModFolder, "MiscUtil.dll"), Properties.Resources.MiscUtil);
                WriteFile(Path.Combine(rootModFolder, "NAudio.Asio.dll"), Properties.Resources.NAudio_Asio);
                WriteFile(Path.Combine(rootModFolder, "NAudio.Core.dll"), Properties.Resources.NAudio_Core);
                WriteFile(Path.Combine(rootModFolder, "NAudio.dll"), Properties.Resources.NAudio);
                WriteFile(Path.Combine(rootModFolder, "NAudio.Midi.dll"), Properties.Resources.NAudio_Midi);
                WriteFile(Path.Combine(rootModFolder, "NAudio.Wasapi.dll"), Properties.Resources.NAudio_Wasapi);
                WriteFile(Path.Combine(rootModFolder, "NAudio.WinForms.dll"), Properties.Resources.NAudio_WinForms);
                WriteFile(Path.Combine(rootModFolder, "NAudio.WinMM.dll"), Properties.Resources.NAudio_WinMM);
                WriteFile(Path.Combine(rootModFolder, "NDesk.Options.dll"), Properties.Resources.NDesk_Options);
                WriteFile(Path.Combine(rootModFolder, "Newtonsoft.Json.dll"), Properties.Resources.Newtonsoft_Json);
                WriteFile(Path.Combine(rootModFolder, "NLog.dll"), Properties.Resources.NLog);
                WriteFile(Path.Combine(rootModFolder, "Ookii.Dialogs.dll"), Properties.Resources.Ookii_Dialogs);
                WriteFile(Path.Combine(rootModFolder, "Pfim.dll"), Properties.Resources.Pfim);
                WriteFile(Path.Combine(rootModFolder, "Rocksmith2014PsarcLib.dll"), Properties.Resources.Rocksmith2014PsarcLib);
                WriteFile(Path.Combine(rootModFolder, "PSTaskDialog.dll"), Properties.Resources.PSTaskDialog);
                WriteFile(Path.Combine(rootModFolder, "RocksmithToolkitLib.dll"), Properties.Resources.RocksmithToolkitLib);
                WriteFile(Path.Combine(rootModFolder, "RocksmithToTabLib.dll"), Properties.Resources.RocksmithToTabLib);
                WriteFile(Path.Combine(rocksmithLocation, "RSMods.exe"), Properties.Resources.RSMods);
                WriteTextFile(Path.Combine(rocksmithLocation, "RSMods.exe.config"), Properties.Resources.RSMods_exe);
                // Managed runtime that RSMods.exe loads from its own folder at startup (RuntimeBootstrap); without
                // it the settings app fails to open.
                WriteFile(Path.Combine(rocksmithLocation, "RocksmithAudioBridge.dll"), Properties.Resources.RocksmithAudioBridgeLibrary);
                WriteTextFile(Path.Combine(rootModFolder, "LICENSE.txt"), Properties.Resources.ProjectLicense);
                WriteTextFile(Path.Combine(rootModFolder, "NOTICE.txt"), Properties.Resources.ProjectNotice);
                WriteFile(Path.Combine(rootModFolder, "SevenZipSharp.dll"), Properties.Resources.SevenZipSharp);
                WriteFile(Path.Combine(rootModFolder, "SharpConfig.dll"), Properties.Resources.SharpConfig);
                WriteFile(Path.Combine(rootModFolder, "System.Security.AccessControl.dll"), Properties.Resources.System_Security_AccessControl);
                WriteFile(Path.Combine(rootModFolder, "System.Security.Principal.Windows.dll"), Properties.Resources.System_Security_Principal_Windows);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Core.dll"), Properties.Resources.TwitchLib_Api_Core);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Core.Enums.dll"), Properties.Resources.TwitchLib_Api_Core_Enums);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Core.Interfaces.dll"), Properties.Resources.TwitchLib_Api_Core_Interfaces);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Core.Models.dll"), Properties.Resources.TwitchLib_Api_Core_Models);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.dll"), Properties.Resources.TwitchLib_Api);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Helix.dll"), Properties.Resources.TwitchLib_Api_Helix);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.Helix.Models.dll"), Properties.Resources.TwitchLib_Api_Helix_Models);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.V5.dll"), Properties.Resources.TwitchLib_Api_V5);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Api.V5.Models.dll"), Properties.Resources.TwitchLib_Api_V5_Models);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Client.dll"), Properties.Resources.TwitchLib_Client);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Client.Enums.dll"), Properties.Resources.TwitchLib_Client_Enums);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Client.Models.dll"), Properties.Resources.TwitchLib_Client_Models);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.Communication.dll"), Properties.Resources.TwitchLib_Communication);
                WriteFile(Path.Combine(rootModFolder, "TwitchLib.PubSub.dll"), Properties.Resources.TwitchLib_PubSub);
                WriteFile(Path.Combine(rootModFolder, "Wwise2010.tar.bz2"), Properties.Resources.Wwise2010_tar);
                WriteFile(Path.Combine(rootModFolder, "Wwise2013.tar.bz2"), Properties.Resources.Wwise2013_tar);
                WriteFile(Path.Combine(rootModFolder, "Wwise2014.tar.bz2"), Properties.Resources.Wwise2014_tar);
                WriteFile(Path.Combine(rootModFolder, "Wwise2015.tar.bz2"), Properties.Resources.Wwise2015_tar);
                WriteFile(Path.Combine(rootModFolder, "Wwise2016.tar.bz2"), Properties.Resources.Wwise2016_tar);
                WriteFile(Path.Combine(rootModFolder, "Wwise2017.tar.bz2"), Properties.Resources.Wwise2017_tar);
                WriteFile(Path.Combine(rootModFolder, "X360.dll"), Properties.Resources.X360);
                WriteFile(Path.Combine(rootModFolder, "zlib.net.dll"), Properties.Resources.zlib_net);
                // ddc Folder
                WriteFile(Path.Combine(ddcFolder, "ddc.exe"), Properties.Resources.ddc);
                WriteTextFile(Path.Combine(ddcFolder, "ddc_chords_protector.xml"), Properties.Resources.ddc_chords_protector);
                WriteTextFile(Path.Combine(ddcFolder, "ddc_chords_remover.xml"), Properties.Resources.ddc_chords_remover);
                WriteTextFile(Path.Combine(ddcFolder, "ddc_dd_remover.xml"), Properties.Resources.ddc_dd_remover);
                WriteFile(Path.Combine(ddcFolder, "ddc_default.cfg"), Properties.Resources.ddc_default);
                WriteTextFile(Path.Combine(ddcFolder, "ddc_default.xml"), Properties.Resources.ddc_default1);
                WriteFile(Path.Combine(ddcFolder, "ddc_keep_all_levels.cfg"), Properties.Resources.ddc_keep_all_levels);
                WriteFile(Path.Combine(ddcFolder, "ddc_merge_all_levels.cfg"), Properties.Resources.ddc_merge_all_levels);
                WriteTextFile(Path.Combine(ddcFolder, "license.txt"), Properties.Resources.license);
                WriteTextFile(Path.Combine(ddcFolder, "readme.txt"), Properties.Resources.readme1);
                // tools Folder 
                WriteFile(Path.Combine(toolsFolder, "7za.exe"), Properties.Resources._7za);
                WriteFile(Path.Combine(toolsFolder, "core.jar"), Properties.Resources.core);
                WriteFile(Path.Combine(toolsFolder, "CreateToolkitShortcut.exe"), Properties.Resources.CreateToolkitShortcut);
                WriteFile(Path.Combine(toolsFolder, "nvdxt.exe"), Properties.Resources.nvdxt);
                WriteFile(Path.Combine(toolsFolder, "oggCut.exe"), Properties.Resources.oggCut);
                WriteFile(Path.Combine(toolsFolder, "oggdec.exe"), Properties.Resources.oggdec);
                WriteFile(Path.Combine(toolsFolder, "oggenc.exe"), Properties.Resources.oggenc);
                WriteTextFile(Path.Combine(toolsFolder, "OpenCmd.bat"), Properties.Resources.OpenCmd);
                WriteFile(Path.Combine(toolsFolder, "packed_codebooks.bin"), Properties.Resources.packed_codebooks);
                WriteFile(Path.Combine(toolsFolder, "packed_codebooks_aoTuV_603.bin"), Properties.Resources.packed_codebooks_aoTuV_603);
                WriteTextFile(Path.Combine(toolsFolder, "readme.txt"), Properties.Resources.readme);
                WriteFile(Path.Combine(toolsFolder, "revorb.exe"), Properties.Resources.revorb);
                WriteFile(Path.Combine(toolsFolder, "topng.exe"), Properties.Resources.topng);
                WriteFile(Path.Combine(toolsFolder, "ww2ogg.exe"), Properties.Resources.ww2ogg);
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
