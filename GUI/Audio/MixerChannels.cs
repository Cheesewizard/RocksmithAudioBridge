using System;
using System.Drawing;
using System.Drawing.Drawing2D;

namespace RSMods.Audio
{
	/// <summary>Playback channels in the order the audio bridge reports and accepts them.</summary>
	internal enum MixerChannel
	{
		Song = 0,
		PlayerOne = 1,
		Master = 2,
		PlayerTwo = 3,
		Microphone = 4,
		VoiceOver = 5,
		SoundEffects = 6
	}

	internal enum ChannelGlyph
	{
		Speaker,
		Guitar,
		Note,
		Microphone,
		Voice,
		Spark
	}

	/// <summary>Name, colour and icon for each playback channel, kept in one place so the strips stay consistent.</summary>
	internal static class ChannelStyle
	{
		public static string Name(MixerChannel channel)
		{
			switch (channel)
			{
				case MixerChannel.Song: return "Song";
				case MixerChannel.PlayerOne: return "Player 1";
				case MixerChannel.Master: return "Master";
				case MixerChannel.PlayerTwo: return "Player 2";
				case MixerChannel.Microphone: return "Microphone";
				case MixerChannel.VoiceOver: return "Voice-over";
				default: return "Effects";
			}
		}

		public static string Description(MixerChannel channel)
		{
			switch (channel)
			{
				case MixerChannel.Song: return "Backing track playback level.";
				case MixerChannel.PlayerOne: return "Player 1 guitar playback level. Note detection is unaffected.";
				case MixerChannel.Master: return "Everything Rocksmith plays, after the channels below it.";
				case MixerChannel.PlayerTwo: return "Player 2 guitar playback level. Note detection is unaffected.";
				case MixerChannel.Microphone: return "Microphone monitoring level.";
				case MixerChannel.VoiceOver: return "Menu and lesson voice-over level.";
				default: return "Menu and gameplay sound effects level.";
			}
		}

		public static Color Tint(MixerChannel channel)
		{
			// One accent for every strip: the glyph and name identify the channel, the colour does not, so
			// the mixer shares the palette of the other tabs instead of carrying its own rainbow.
			return StudioTheme.Accent;
		}

		public static ChannelGlyph Glyph(MixerChannel channel)
		{
			switch (channel)
			{
				case MixerChannel.Master: return ChannelGlyph.Speaker;
				case MixerChannel.PlayerOne:
				case MixerChannel.PlayerTwo: return ChannelGlyph.Guitar;
				case MixerChannel.Song: return ChannelGlyph.Note;
				case MixerChannel.Microphone: return ChannelGlyph.Microphone;
				case MixerChannel.VoiceOver: return ChannelGlyph.Voice;
				default: return ChannelGlyph.Spark;
			}
		}

		/// <summary>Draws a glyph inside a 24 by 24 box placed at the top left of <paramref name="box"/>.</summary>
		public static void Draw(Graphics canvas, ChannelGlyph glyph, PointF origin, float scale, Color color)
		{
			if (canvas == null)
				throw new ArgumentNullException(nameof(canvas));
			var state = canvas.Save();
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			canvas.TranslateTransform(origin.X, origin.Y);
			canvas.ScaleTransform(scale, scale);
			using (var pen = new Pen(color, 1.8f) { LineJoin = LineJoin.Round, StartCap = LineCap.Round, EndCap = LineCap.Round })
			using (var brush = new SolidBrush(color))
			{
				switch (glyph)
				{
					case ChannelGlyph.Speaker: DrawSpeaker(canvas, pen, brush); break;
					case ChannelGlyph.Guitar: DrawGuitar(canvas, pen, brush); break;
					case ChannelGlyph.Note: DrawNote(canvas, pen, brush); break;
					case ChannelGlyph.Microphone: DrawMicrophone(canvas, pen, brush); break;
					case ChannelGlyph.Voice: DrawVoice(canvas, pen, brush); break;
					default: DrawSpark(canvas, brush); break;
				}
			}
			canvas.Restore(state);
		}

		private static void DrawSpeaker(Graphics canvas, Pen pen, Brush brush)
		{
			canvas.FillPolygon(brush, new[]
			{
				new PointF(3, 9), new PointF(7, 9), new PointF(12, 4),
				new PointF(12, 20), new PointF(7, 15), new PointF(3, 15)
			});
			canvas.DrawArc(pen, 12, 6, 8, 12, -60, 120);
			canvas.DrawArc(pen, 14, 2, 12, 20, -55, 110);
		}

		private static void DrawGuitar(Graphics canvas, Pen pen, Brush brush)
		{
			canvas.DrawEllipse(pen, 2, 11, 12, 10);
			canvas.DrawEllipse(pen, 8, 7, 9, 9);
			canvas.DrawLine(pen, 13, 11, 21, 3);
			canvas.DrawLine(pen, 16, 13, 23, 6);
			canvas.FillEllipse(brush, 8, 12, 4, 4);
		}

		private static void DrawNote(Graphics canvas, Pen pen, Brush brush)
		{
			canvas.DrawLines(pen, new[] { new PointF(8, 18), new PointF(8, 5), new PointF(20, 2), new PointF(20, 15) });
			canvas.FillEllipse(brush, 2, 15, 7, 6);
			canvas.FillEllipse(brush, 14, 12, 7, 6);
		}

		private static void DrawMicrophone(Graphics canvas, Pen pen, Brush brush)
		{
			using (var capsule = StudioTheme.RoundedRectangle(new Rectangle(8, 2, 8, 12), 4))
				canvas.FillPath(brush, capsule);
			canvas.DrawArc(pen, 4, 7, 16, 13, 20, 140);
			canvas.DrawLine(pen, 12, 17, 12, 21);
			canvas.DrawLine(pen, 8, 21, 16, 21);
		}

		private static void DrawVoice(Graphics canvas, Pen pen, Brush brush)
		{
			using (var bubble = StudioTheme.RoundedRectangle(new Rectangle(2, 4, 20, 13), 5))
				canvas.DrawPath(pen, bubble);
			canvas.FillPolygon(brush, new[] { new PointF(7, 16), new PointF(13, 16), new PointF(7, 21) });
			for (int index = 0; index < 3; index++)
				canvas.FillEllipse(brush, 6 + index * 5, 9, 3, 3);
		}

		private static void DrawSpark(Graphics canvas, Brush brush)
		{
			canvas.FillPolygon(brush, new[]
			{
				new PointF(13, 1), new PointF(15.5f, 8.5f), new PointF(23, 11), new PointF(15.5f, 13.5f),
				new PointF(13, 21), new PointF(10.5f, 13.5f), new PointF(3, 11), new PointF(10.5f, 8.5f)
			});
			canvas.FillPolygon(brush, new[]
			{
				new PointF(5, 15), new PointF(6.2f, 18), new PointF(9, 19), new PointF(6.2f, 20), new PointF(5, 23),
				new PointF(3.8f, 20), new PointF(1, 19), new PointF(3.8f, 18)
			});
		}
	}
}
