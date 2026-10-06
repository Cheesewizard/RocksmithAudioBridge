namespace RS2014_Mod_Installer
{
    partial class GUI
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(GUI));
            this.UseModsButton = new System.Windows.Forms.Button();
            this.UninstallButton = new System.Windows.Forms.Button();
            this.label1 = new System.Windows.Forms.Label();
            this.SuspendLayout();
            // 
            // UseModsButton
            // 
            this.UseModsButton.Font = new System.Drawing.Font("Microsoft Sans Serif", 12F, System.Drawing.FontStyle.Bold);
            this.UseModsButton.Location = new System.Drawing.Point(12, 71);
            this.UseModsButton.Name = "UseModsButton";
            this.UseModsButton.Size = new System.Drawing.Size(518, 80);
            this.UseModsButton.TabIndex = 1;
            this.UseModsButton.Text = "Install";
            this.UseModsButton.UseVisualStyleBackColor = true;
            this.UseModsButton.Click += new System.EventHandler(this.UseModsButton_Click);
            //
            // UninstallButton
            //
            this.UninstallButton.Font = new System.Drawing.Font("Microsoft Sans Serif", 9.75F);
            this.UninstallButton.Location = new System.Drawing.Point(12, 157);
            this.UninstallButton.Name = "UninstallButton";
            this.UninstallButton.Size = new System.Drawing.Size(518, 31);
            this.UninstallButton.TabIndex = 3;
            this.UninstallButton.Text = "Uninstall";
            this.UninstallButton.UseVisualStyleBackColor = true;
            this.UninstallButton.Visible = false;
            this.UninstallButton.Click += new System.EventHandler(this.UninstallButton_Click);
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Font = new System.Drawing.Font("Microsoft Sans Serif", 12F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(0)));
            this.label1.Location = new System.Drawing.Point(12, 9);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(518, 40);
            this.label1.TabIndex = 2;
            this.label1.Text = "Install, repair or uninstall Rocksmith Audio Bridge.\r\nYour recordings are never touched.";
            // 
            // GUI
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(548, 200);
            this.Controls.Add(this.label1);
            this.Controls.Add(this.UseModsButton);
            this.Controls.Add(this.UninstallButton);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.Name = "GUI";
            this.Text = "Rocksmith Audio Bridge Installer";
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion
        private System.Windows.Forms.Button UseModsButton;
        private System.Windows.Forms.Button UninstallButton;
        private System.Windows.Forms.Label label1;
    }
}

