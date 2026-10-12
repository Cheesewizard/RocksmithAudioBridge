using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
	// The RS_ASIO Settings tab in the overlay palette (dark page, rounded cards) instead of the designer's light
	// group boxes. Only the look changes: every group box is swapped for a card of the same position and size, and its
	// controls move across at the same positions, so the tab keeps upstream's layout (one screen, no scrolling) and
	// upstream's loading (Startup_LoadASIODevices, PriorSettings_LoadASIOSettings) and saving
	// (SaveSettings_ASIO_Middleware) work on the same controls as before. No layout panels: the first version flowed
	// the cards with auto-sizing panels, which opened slowly and needed scrolling.
	public partial class MainForm
	{
		private void InitializeRsAsioPage()
		{
			tab_RSASIO.SuspendLayout();
			tab_RSASIO.BackColor = OverlayLook.Window;
			tab_RSASIO.ForeColor = OverlayLook.Text;
			rsAsioInput0Card = ReplaceWithCard(groupBox_ASIO_Input0, "Input 1 · Player 1");
			rsAsioInput1Card = ReplaceWithCard(groupBox_ASIO_Input1, "Input 2 · Player 2");
			rsAsioOutputCard = ReplaceWithCard(groupBox_ASIO_Output, "Output");
			ReplaceWithCard(groupBox_ASIO_InputMic, "Microphone");
			ReplaceWithCard(groupBox_ASIO_BufferSize, "Buffer size");
			rsAsioConfigCard = ReplaceWithCard(groupBox_ASIO_Config, "Config");
			label_ASIO_Credits.Font = StudioTheme.Small;
			label_ASIO_Credits.ForeColor = OverlayLook.Faint;
			label_ASIO_Credits.BackColor = OverlayLook.Window;
			StyleRsAsioControls(tab_RSASIO);
			tab_RSASIO.ResumeLayout(true);
		}

		// Kept for the Audit card (under Config) and its Details panel (over the device cards): UI.AuditTab.cs.
		private RsAsioCard rsAsioInput0Card, rsAsioInput1Card, rsAsioOutputCard, rsAsioConfigCard;

		private RsAsioCard ReplaceWithCard(GroupBox group, string title)
		{
			var card = new RsAsioCard { Bounds = group.Bounds, Anchor = group.Anchor };
			var heading = new Label { Text = title.ToUpperInvariant(), Font = StudioTheme.SectionFont, ForeColor = OverlayLook.Muted, BackColor = OverlayLook.Card, AutoSize = true, Location = new Point(card.LogicalToDeviceUnits(10), card.LogicalToDeviceUnits(8)) };
			card.Controls.Add(heading);
			// The group box drew its caption in its border, so a control can sit higher than the card's own title
			// (Config's first checkbox did): move them all down by the overlap and grow the card to match.
			int top = int.MaxValue;
			foreach (Control child in group.Controls) top = System.Math.Min(top, child.Top);
			int below = heading.Bottom + card.LogicalToDeviceUnits(6);
			int shift = top < below ? below - top : 0;
			while (group.Controls.Count > 0)
			{
				Control child = group.Controls[0];
				Point at = child.Location;
				group.Controls.Remove(child);
				child.Location = new Point(at.X, at.Y + shift);
				card.Controls.Add(child);
			}
			card.Height += shift;
			int index = tab_RSASIO.Controls.GetChildIndex(group);
			tab_RSASIO.Controls.Remove(group);
			tab_RSASIO.Controls.Add(card);
			tab_RSASIO.Controls.SetChildIndex(card, index);
			return card;
		}

		private static void StyleRsAsioControls(Control parent)
		{
			foreach (Control child in parent.Controls)
			{
				switch (child)
				{
					case ListBox list:
						list.BackColor = StudioTheme.Field;
						list.ForeColor = StudioTheme.Ink;
						list.BorderStyle = BorderStyle.None;
						list.DrawMode = DrawMode.OwnerDrawFixed;
						list.ItemHeight = list.Font.Height + list.LogicalToDeviceUnits(4);
						list.DrawItem -= DrawRsAsioDevice;
						list.DrawItem += DrawRsAsioDevice;
						break;
					case NumericUpDown number:
						number.BackColor = StudioTheme.Field;
						number.ForeColor = StudioTheme.Ink;
						number.BorderStyle = BorderStyle.FixedSingle;
						break;
					case Button button:
						button.FlatStyle = FlatStyle.Flat;
						button.FlatAppearance.BorderColor = StudioTheme.Line;
						button.BackColor = StudioTheme.Field;
						button.ForeColor = StudioTheme.Ink;
						button.Cursor = Cursors.Hand;
						break;
					case ButtonBase toggle when toggle is CheckBox || toggle is RadioButton:
						// Flat draws a dark box in the card's colours; the standard style is the white system box.
						toggle.FlatStyle = FlatStyle.Flat;
						toggle.FlatAppearance.BorderColor = StudioTheme.Line;
						toggle.ForeColor = StudioTheme.Ink;
						toggle.BackColor = OverlayLook.Card;
						break;
					case Label label when label.Parent is RsAsioCard && label.Font != StudioTheme.SectionFont:   // not the card titles
						label.Text = label.Text.TrimEnd(':', ' ');
						label.ForeColor = OverlayLook.Text;
						label.BackColor = OverlayLook.Card;
						break;
				}
				StyleRsAsioControls(child);
			}
		}

		// A device row: the picked device on an accent tint, the rest plain.
		private static void DrawRsAsioDevice(object sender, DrawItemEventArgs args)
		{
			var list = (ListBox)sender;
			bool selected = (args.State & DrawItemState.Selected) != 0;
			using (var background = new SolidBrush(selected ? StudioTheme.Blend(StudioTheme.Field, StudioTheme.Accent, 0.45) : StudioTheme.Field))
				args.Graphics.FillRectangle(background, args.Bounds);
			if (args.Index < 0) return;
			var text = new Rectangle(args.Bounds.X + 4, args.Bounds.Y, args.Bounds.Width - 6, args.Bounds.Height);
			TextRenderer.DrawText(args.Graphics, list.Items[args.Index].ToString(), list.Font, text, StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		// The overlay's card: Card fill, CardEdge outline, rounded corners. A plain panel, so its children keep the
		// absolute positions the designer gave them.
		private sealed class RsAsioCard : Panel
		{
			public RsAsioCard()
			{
				BackColor = OverlayLook.Card;
				ForeColor = OverlayLook.Text;
				DoubleBuffered = true;
				ResizeRedraw = true;
			}

			protected override void OnPaintBackground(PaintEventArgs e)
			{
				e.Graphics.Clear(Parent?.BackColor ?? OverlayLook.Window);
				e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
				var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
				using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(6)))
				using (var fill = new SolidBrush(OverlayLook.Card))
				using (var edge = new Pen(OverlayLook.CardEdge))
				{
					e.Graphics.FillPath(fill, path);
					e.Graphics.DrawPath(edge, path);
				}
			}
		}
	}
}
