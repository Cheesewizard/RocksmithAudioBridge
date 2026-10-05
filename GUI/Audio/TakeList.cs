using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class TakeList : ListView
	{
		private static readonly string[] Extensions = { ".wav", ".mp4", ".mkv" };
		private string signature = "";

		public string Summary { get; private set; } = "No takes yet.";

		public TakeList()
		{
			View = View.Details;
			OwnerDraw = true;
			DoubleBuffered = true;
			FullRowSelect = true;
			MultiSelect = false;
			HideSelection = false;
			HeaderStyle = ColumnHeaderStyle.Nonclickable;
			BorderStyle = BorderStyle.None;
			BackColor = StudioTheme.Field;
			ForeColor = StudioTheme.Ink;
			Font = StudioTheme.Body;
			Columns.Add("Take", 240);
			Columns.Add("Length", 78, HorizontalAlignment.Right);
			Columns.Add("Size", 74, HorizontalAlignment.Right);
			Columns.Add("Recorded", 116, HorizontalAlignment.Right);
			DrawColumnHeader += PaintHeader;
			DrawItem += PaintRow;
			DrawSubItem += PaintCell;
		}

		public string SelectedPath
		{
			get { return SelectedItems.Count == 1 ? (string)SelectedItems[0].Tag : null; }
		}

		public void Load(string directory, bool force = false)
		{
			string current = Signature(directory);
			if (!force && current == signature)
				return;
			signature = current;
			string selected = SelectedPath;
			BeginUpdate();
			long total = 0;
			try
			{
				Items.Clear();
				foreach (FileInfo take in Recent(directory))
				{
					TimeSpan? length = take.Extension.Equals(".wav", StringComparison.OrdinalIgnoreCase) ? StudioFormat.WaveLength(take.FullName) : null;
					var row = new ListViewItem(new[]
					{
						take.Name,
						length.HasValue ? StudioFormat.Length(length.Value) : "—",
						StudioFormat.Bytes(take.Length),
						take.LastWriteTime.ToString("d MMM  HH:mm")
					})
					{ Tag = take.FullName };
					Items.Add(row);
					total += take.Length;
					if (take.FullName == selected)
						row.Selected = true;
				}
				Summary = Items.Count == 0 ? "No takes in this folder yet." : Items.Count + (Items.Count == 1 ? " take · " : " takes · ") + StudioFormat.Bytes(total);
			}
			finally { EndUpdate(); }
		}

		public void Select(string path)
		{
			foreach (ListViewItem row in Items)
			{
				if (string.Equals((string)row.Tag, path, StringComparison.OrdinalIgnoreCase))
				{
					row.Selected = true;
					row.EnsureVisible();
					return;
				}
			}
		}

		private static IEnumerable<FileInfo> Recent(string directory)
		{
			var takes = new List<FileInfo>();
			try
			{
				if (!Directory.Exists(directory))
					return takes;
				foreach (FileInfo file in new DirectoryInfo(directory).GetFiles())
				{
					if (Array.IndexOf(Extensions, file.Extension.ToLowerInvariant()) >= 0)
						takes.Add(file);
				}
			}
			catch (Exception error) when (error is IOException || error is UnauthorizedAccessException)
			{
				return takes;
			}
			takes.Sort((left, right) => right.LastWriteTimeUtc.CompareTo(left.LastWriteTimeUtc));
			return takes.Count > 60 ? takes.GetRange(0, 60) : takes;
		}

		private static string Signature(string directory)
		{
			try
			{
				if (!Directory.Exists(directory))
					return directory + "|missing";
				var info = new DirectoryInfo(directory);
				return directory + "|" + info.LastWriteTimeUtc.Ticks + "|" + info.GetFiles().Length;
			}
			catch (Exception error) when (error is IOException || error is UnauthorizedAccessException)
			{
				return directory + "|unreadable";
			}
		}

		protected override void OnResize(EventArgs e)
		{
			base.OnResize(e);
			ResizeColumns();
		}

		protected override void ScaleControl(SizeF factor, BoundsSpecified specified)
		{
			base.ScaleControl(factor, specified);
			ResizeColumns();
		}

		private void ResizeColumns()
		{
			if (Columns.Count == 4)
				Columns[0].Width = Math.Max(140, ClientSize.Width - Columns[1].Width - Columns[2].Width - Columns[3].Width - 4);
		}

		private void PaintHeader(object sender, DrawListViewColumnHeaderEventArgs args)
		{
			using (var brush = new SolidBrush(StudioTheme.Surface))
				args.Graphics.FillRectangle(brush, args.Bounds);
			using (var pen = new Pen(StudioTheme.Line))
				args.Graphics.DrawLine(pen, args.Bounds.Left, args.Bounds.Bottom - 1, args.Bounds.Right, args.Bounds.Bottom - 1);
			var text = new Rectangle(args.Bounds.X + 8, args.Bounds.Y, args.Bounds.Width - 16, args.Bounds.Height);
			TextRenderer.DrawText(args.Graphics, args.Header.Text, StudioTheme.SectionFont, text, StudioTheme.Faint,
				Justify(args.Header.TextAlign) | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		private void PaintRow(object sender, DrawListViewItemEventArgs args)
		{
			// Details cells paint their own backgrounds so a row repaint cannot erase other columns.
		}

		private void PaintCell(object sender, DrawListViewSubItemEventArgs args)
		{
			Color background = args.Item.Selected
				? StudioTheme.Blend(StudioTheme.Field, StudioTheme.Accent, 0.38)
				: args.ItemIndex % 2 == 0 ? StudioTheme.Field : StudioTheme.Shift(StudioTheme.Field, 5);
			using (var brush = new SolidBrush(background))
				args.Graphics.FillRectangle(brush, args.Bounds);
			var text = new Rectangle(args.Bounds.X + 8, args.Bounds.Y, args.Bounds.Width - 16, args.Bounds.Height);
			Color ink = args.ColumnIndex == 0 ? StudioTheme.Ink : StudioTheme.Muted;
			TextRenderer.DrawText(args.Graphics, args.SubItem.Text, Font, text, args.Item.Selected ? Color.White : ink,
				Justify(Columns[args.ColumnIndex].TextAlign) | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		private static TextFormatFlags Justify(HorizontalAlignment alignment)
		{
			return alignment == HorizontalAlignment.Right ? TextFormatFlags.Right : TextFormatFlags.Left;
		}
	}
}
