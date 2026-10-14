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

            bool written = DLLStuff.InjectDLL(rsPath) && DLLStuff.InjectGUI(rsPath);
            // Saved even when a step failed, so Uninstall still knows every file that did get written.
            try { DLLStuff.SaveInstallManifest(rsPath); } catch { /* uninstall falls back to the core file list */ }
            if (!written)
                MessageBox.Show("The install did not finish, so the game folder may now hold files from two versions.\n\nFix the problem above and press " + originalButtonText + " again.", "Install incomplete", MessageBoxButtons.OK, MessageBoxIcon.Warning);

            if (written)
            {
                string rsModsPath = Path.Combine(rsPath, "RSMods", "RSMods.exe");

                // The audio bridge driver is part of the mod, set up through RSMods.exe's own Install (one Windows
                // admin prompt, skipped when it is already registered). Without it the install is rolled back, keeping the
                // log the message points at. UsbDk failing only costs the direct USB cable capture, so it is a warning.
                string driverError = Uninstaller.RunDriverInstall(rsPath, out string usbDkWarning, out bool restartNeeded);
                if (driverError != null)
                {
                    Uninstaller.RemoveFiles(rsPath, removeSettings: false, menuEntryRemoved: false, keepLogs: true);
                    MessageBox.Show("Rocksmith Audio Bridge was not installed: its audio driver is required and could not be set up (" + driverError + ").\n\nRun the installer again and approve the Windows admin prompt.",
                        "Install cancelled", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    UseModsButton.Text = originalButtonText;
                    RefreshMode();
                    return;
                }
                if (usbDkWarning != null)
                    MessageBox.Show("The mod is installed. " + usbDkWarning, "Real Tone Cable", MessageBoxButtons.OK, MessageBoxIcon.Warning);

                // Note by Note is switched on from its Riff Repeater menu row, which lives in cache.psarc. Add it (or put
                // it back after a Steam file check or another tool restored the cache). Only our entry is touched.
                string menuError = Uninstaller.RunMenuTool(rsPath, add: true);
                if (menuError != null)
                    MessageBox.Show("The mod is installed, but the Note by Note entry could not be added to Riff Repeater (" + menuError + ").\n\nRun this installer again to retry.", "Note by Note menu", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                MessageBox.Show("This version of the installer allows you to take advantage of the new mod settings available by opening: " + rsModsPath, "New Mod Settings Available!", MessageBoxButtons.OK, MessageBoxIcon.Information);

                CreateDesktopShortcut(rsModsPath);
                // Last, so it is the message players see: the low-latency USB driver for the Real Tone Cable (UsbDk) was
                // just installed and only starts after a restart. Without one the cable still plays, but slower, with no
                // sign of why.
                if (restartNeeded)
                {
                    DialogResult restart = MessageBox.Show(
                        "RESTART YOUR PC BEFORE YOU PLAY.\n\n" +
                        "The low-latency driver for the Real Tone Cable was just installed. It only starts after Windows restarts. " +
                        "Until then the cable still works, but with Windows' usual delay.\n\n" +
                        "Save anything open in other programs, then press Yes to restart now. Press No to restart later yourself.",
                        "Restart required", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button1);
                    if (restart == DialogResult.Yes)
                    {
                        try
                        {
                            Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "shutdown.exe"),
                                "/r /t 10 /c \"Restarting to finish installing Rocksmith Audio Bridge.\"") { UseShellExecute = false, CreateNoWindow = true });
                            Close();
                            return;
                        }
                        catch (Exception ex)
                        {
                            MessageBox.Show("Windows could not be restarted from here (" + ex.Message + "). Please restart your PC yourself before playing.", "Restart required", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        }
                    }
                }

                Process.Start(rsModsPath);

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
            bool removeSettings = MessageBox.Show("Also remove your mod settings (RSMods.ini)?\n\nChoose No to keep them for a later reinstall.",
                "Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) == DialogResult.Yes;
            // UsbDk (the Real Tone Cable's direct USB driver) is system-wide, so it is asked about rather than removed
            // silently: Yes by default when this installer put it there, No when it was already on the PC.
            bool removeUsbDk = false;
            if (Uninstaller.IsUsbDkInstalled())
            {
                bool ours = Uninstaller.UsbDkInstalledByUs();
                removeUsbDk = MessageBox.Show(
                    (ours ? "Also remove UsbDk, the low-latency Real Tone Cable driver this mod installed?\n\nChoose No only if another program you use needs it."
                          : "UsbDk, the low-latency Real Tone Cable driver, is installed on this PC. It may have come with this mod or with another program.\n\nRemove it too?"),
                    "Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question, ours ? MessageBoxDefaultButton.Button1 : MessageBoxDefaultButton.Button2) == DialogResult.Yes;
            }

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

                // 3. The player's own RS_ASIO.ini, if "Set up for me" replaced it. Before the files: it needs RSMods.exe
                // and the registry state RemoveFiles deletes.
                string restoreError = Uninstaller.RunRsAsioRestore(rsPath);

                // 4. Our files.
                var failed = Uninstaller.RemoveFiles(rsPath, removeSettings, menuEntryRemoved: menuError == null);

                // 5. UsbDk, when asked. Last, so declining its admin prompt only leaves UsbDk behind.
                bool usbDkRestart = false;
                if (removeUsbDk)
                {
                    string usbDkError;
                    try { usbDkError = Uninstaller.RemoveUsbDk(out usbDkRestart); }
                    catch (OperationCanceledException) { usbDkError = "the Windows admin prompt was declined"; }
                    if (usbDkError != null)
                        MessageBox.Show("UsbDk could not be removed (" + usbDkError + "). You can remove \"UsbDk Runtime Libraries\" from Windows Settings > Apps > Installed apps.",
                            "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                if (restoreError != null)
                    MessageBox.Show("Your original RS_ASIO.ini could not be put back (" + restoreError + "). A copy is in "
                        + System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "Backups") + ".",
                        "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                string restartNote = usbDkRestart ? "\n\nRestart Windows to finish removing UsbDk." : "";
                if (failed.Count == 0)
                    MessageBox.Show("Rocksmith Audio Bridge was uninstalled. Your recordings were kept." + restartNote, "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
                else
                    MessageBox.Show("Uninstalled, but these files could not be removed (close any program using them and run Uninstall again):\n" + string.Join("\n", failed) + restartNote,
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
