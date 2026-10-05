using System;
using System.Windows.Forms;
using System.IO;
using System.Diagnostics;
using System.Security.Cryptography.X509Certificates;

namespace RS2014_Mod_Installer
{
    public partial class GUI : Form
    {
        public GUI()
        {
            InitializeComponent();
            RefreshMode();
        }

        // Install, or Reinstall / Repair plus Uninstall when Rocksmith Audio Bridge is already in the game folder.
        private void RefreshMode()
        {
            string rsPath = RSMods.Util.GenUtil.GetRSDirectory();
            bool installed = Uninstaller.IsInstalled(rsPath);
            UseModsButton.Text = installed ? "Reinstall / Repair" : "Install";
            UninstallButton.Visible = installed;
        }

        private static bool IsGameRunning() => Process.GetProcessesByName("Rocksmith2014").Length != 0;

        private string rsLocation = string.Empty;

        private string WhereIsRocksmith()
        {
            if (rsLocation?.Length == 0)
                rsLocation = RSMods.Util.GenUtil.GetRSDirectory();

            return rsLocation;
        }

        private void UseModsButton_Click(object sender, EventArgs e)
        {
            string originalButtonText = UseModsButton.Text;
            UseModsButton.Text += "\n(Please wait as we get the mods setup. This should take but a moment).";

            string rsPath = WhereIsRocksmith();
            if (string.IsNullOrEmpty(rsPath))
            {
                MessageBox.Show("It looks like your current Rocksmith2014 install folder cannot be found. Please tell us where it is located!", "Error: RSLocation Not Found", MessageBoxButtons.OK, MessageBoxIcon.Error);
               
                rsPath = RSMods.Util.GenUtil.AskUserForRSFolder();

                if (string.IsNullOrEmpty(rsPath))
                {
                    MessageBox.Show("We cannot detect where you have Rocksmith located. Please try reinstalling your game on Steam.", "Error: RSLocation Not Found", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    Environment.Exit(1);
                    return;
                }

                rsLocation = rsPath;
            }

            IsVoid(rsPath);

            if (IsGameRunning())
            {
                MessageBox.Show("Please close Rocksmith first.", "Rocksmith is open", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                UseModsButton.Text = originalButtonText;
                return;
            }
            // A running RSMods.exe (the settings window) locks its files: the game DLL and driver would be updated but
            // the settings app and its runtime would not, leaving a mixed install. Ask first instead.
            if (Uninstaller.IsOurToolRunning(rsPath))
            {
                MessageBox.Show("Please close RSMods (the settings window) first.", "RSMods is open", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                UseModsButton.Text = originalButtonText;
                return;
            }

            if (DLLStuff.InjectDLL(rsPath) && DLLStuff.InjectGUI(rsPath))
            {
                string rsModsPath = Path.Combine(rsPath, "RSMods.exe");
                try { DLLStuff.SaveInstallManifest(rsPath); } catch { /* uninstall falls back to the core file list */ }

                // Note by Note is switched on from its Riff Repeater menu row, which lives in cache.psarc. Add it (or put
                // it back after a Steam file check or another tool restored the cache). Only our entry is touched.
                string menuError = Uninstaller.RunMenuTool(rsPath, add: true);
                if (menuError != null)
                    MessageBox.Show("The mod is installed, but the Note by Note entry could not be added to Riff Repeater (" + menuError + ").\n\nRun this installer again to retry.", "Note by Note menu", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                MessageBox.Show("This version of the installer allows you to take advantage of the new mod settings available by opening: " + rsModsPath, "New Mod Settings Available!", MessageBoxButtons.OK, MessageBoxIcon.Information);

                Process.Start(rsModsPath);
                CreateDesktopShortcut(rsModsPath);

                Close();
            }

            UseModsButton.Text = originalButtonText;
        }

        private void UninstallButton_Click(object sender, EventArgs e)
        {
            string rsPath = WhereIsRocksmith();
            if (string.IsNullOrEmpty(rsPath) || !Uninstaller.IsInstalled(rsPath))
            {
                MessageBox.Show("Rocksmith Audio Bridge was not found in the Rocksmith folder.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (IsGameRunning())
            {
                MessageBox.Show("Please close Rocksmith first.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (Uninstaller.IsOurToolRunning(rsPath))
            {
                MessageBox.Show("Please close RSMods (the settings window) first.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (MessageBox.Show("Uninstall Rocksmith Audio Bridge from\n" + rsPath + "?\n\nYour recordings are kept.", "Uninstall",
                MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            bool removeSettings = MessageBox.Show("Also remove your mod settings (RSMods.ini, AudioRouting.ini)?\n\nChoose No to keep them for a later reinstall.",
                "Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) == DialogResult.Yes;

            UseWaitCursor = true;
            try
            {
                // 1. The ASIO bridge driver, while its file is still there. Cancelling the admin prompt stops here.
                if (Uninstaller.IsDriverRegistered())
                {
                    try { Uninstaller.RemoveDriverRegistration(); }
                    catch (OperationCanceledException)
                    {
                        MessageBox.Show("Uninstall cancelled: removing the audio bridge driver needs admin approval. Nothing was removed.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        return;
                    }
                }

                // 2. Our Riff Repeater entry, surgically, from the current cache.psarc.
                string menuError = Uninstaller.RunMenuTool(rsPath, add: false);
                if (menuError != null
                    && MessageBox.Show("The Note by Note entry could not be removed from Riff Repeater (" + menuError + "). Without the mod it only shows an inactive row.\n\nContinue the uninstall?",
                        "Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;

                // 3. Our files.
                var failed = Uninstaller.RemoveFiles(rsPath, removeSettings, menuEntryRemoved: menuError == null);
                if (failed.Count == 0)
                    MessageBox.Show("Rocksmith Audio Bridge was uninstalled. Your recordings were kept.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
                else
                    MessageBox.Show("Uninstalled, but these files could not be removed (close any program using them and run Uninstall again):\n" + string.Join("\n", failed),
                        "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Uninstall failed: " + ex.Message, "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                UseWaitCursor = false;
                RefreshMode();
            }
        }

        private void CreateDesktopShortcut(string rsModsPath)
        {
            DialogResult dialogResult = MessageBox.Show("Would you like to create RSMods shortcut on your desktop?", "Create shortcut", MessageBoxButtons.YesNo);
            if (dialogResult != DialogResult.Yes)
            {
                return;
            }

            try
            {
                string deskDir = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);

                using (StreamWriter writer = new StreamWriter(deskDir + @"\\RSMods.url", true))
                {
                    writer.WriteLine("[InternetShortcut]");
                    writer.WriteLine("URL=file:///" + rsModsPath);
                    writer.WriteLine("IconIndex=0");
                    string icon = rsModsPath.Replace('\\', '/');
                    writer.WriteLine("IconFile=" + icon);
                }
            }
            catch (IOException ex)
            {
                MessageBox.Show(ex.Message, "Error creating the shortcut");
            }
        }

        public static void IsVoid(string installLocation) // Anti-Piracy Check (False = Real, True = Pirated) || Modified from Beat Saber Mod Assistant
        {
            string reason = string.Empty;
            bool fakeSteamApi = true;
            try
            {
                X509Certificate2 cert = new X509Certificate2(X509Certificate.CreateFromSignedFile(Path.Combine(installLocation, "steam_api.dll")));

                if (cert.GetNameInfo(X509NameType.SimpleName, false) == "Valve" || cert.Verify())
                {
                    fakeSteamApi = false;
                }
                else
                {
                    reason += "Invalid steam_api.dll certificate.";
                }
            }
            catch { } // Fall-through = bad cert.

            bool areCrackIndicationsPresent = File.Exists(Path.Combine(installLocation, "IGG-GAMES.COM.url")) || File.Exists(Path.Combine(installLocation, "SmartSteamEmu.ini")) || File.Exists(Path.Combine(installLocation, "GAMESTORRENT.CO.url")) || File.Exists(Path.Combine(installLocation, "Codex.ini")) || File.Exists(Path.Combine(installLocation, "Skidrow.ini")) || File.Exists(Path.Combine(installLocation, "steamclient.dll"));

            if (areCrackIndicationsPresent)
            {
                reason += "\nParts of game crack are present in the folder.";
            }

            bool isExeInvalid = !ExeUtil.CheckExecutable(installLocation);

            if (isExeInvalid)
            {
                reason += "\nGame executable version doesn't appear to be correct.";
            }

            if (areCrackIndicationsPresent || fakeSteamApi || isExeInvalid)
            {
                MessageBox.Show($"Incompatible Rocksmith version detected! Only the Steam versions of RS are supported - make sure you are not using a pirated / stolen copy of Rocksmith 2014! {Environment.NewLine}Reason: {reason}", "Incompatible Rocksmith version", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Process.Start("https://store.steampowered.com/app/221680/");
                Environment.Exit(1);
                return;
            }
        }
    }
}
