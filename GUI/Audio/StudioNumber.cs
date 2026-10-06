using System;
using System.Globalization;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Typeable value box for the studio: a themed TextBox that owns its own number handling (clamp, round,
	/// step with the arrow keys or the wheel, commit on Enter or focus loss). Used instead of NumericUpDown,
	/// whose hidden spin buttons show as an unpainted strip at 150% scaling; a plain TextBox paints its
	/// whole face. Same members as NumericUpDown: Minimum, Maximum, Increment, DecimalPlaces,
	/// Value, ValueChanged (fires on programmatic changes too, like NumericUpDown, so the sync flags hold).
	/// </summary>
	internal sealed class StudioNumber : TextBox
	{
		private decimal value;
		private decimal minimum;
		private decimal maximum = 100;
		private decimal increment = 1;
		private int decimalPlaces;
		private bool rendering;

		public event EventHandler ValueChanged;

		public StudioNumber()
		{
			BackColor = StudioTheme.Field;
			ForeColor = StudioTheme.Ink;
			Font = StudioTheme.Strong;
			TextAlign = HorizontalAlignment.Center;
			BorderStyle = BorderStyle.FixedSingle;
			Width = 84;
			Height = 36;
			Margin = new Padding(10, 2, 0, 2);
			Render();
		}

		public decimal Minimum
		{
			get => minimum;
			set { minimum = value; if (maximum < minimum) maximum = minimum; Value = this.value; Render(); }
		}

		public decimal Maximum
		{
			get => maximum;
			set { maximum = value; if (minimum > maximum) minimum = maximum; Value = this.value; Render(); }
		}

		public decimal Increment
		{
			get => increment;
			set => increment = value <= 0 ? 1 : value;
		}

		public int DecimalPlaces
		{
			get => decimalPlaces;
			set { decimalPlaces = Math.Max(0, Math.Min(6, value)); Render(); }
		}

		public decimal Value
		{
			get => value;
			set
			{
				decimal clamped = Math.Round(Math.Max(minimum, Math.Min(maximum, value)), decimalPlaces);
				bool changed = clamped != this.value;
				this.value = clamped;
				Render();
				if (changed) ValueChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		private void Render()
		{
			rendering = true;
			Text = value.ToString("F" + decimalPlaces, CultureInfo.CurrentCulture);
			rendering = false;
		}

		/// <summary>Parse what was typed; an unreadable entry reverts to the current value.</summary>
		private void Commit()
		{
			if (rendering) return;
			decimal typed;
			if (decimal.TryParse(Text, NumberStyles.Number, CultureInfo.CurrentCulture, out typed)
				|| decimal.TryParse(Text, NumberStyles.Number, CultureInfo.InvariantCulture, out typed))
				Value = typed;
			else
				Render();
		}

		protected override void OnLeave(EventArgs e)
		{
			Commit();
			base.OnLeave(e);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			switch (e.KeyCode)
			{
				case Keys.Enter:
					Commit();
					SelectAll();
					e.Handled = e.SuppressKeyPress = true;
					return;
				case Keys.Up:
					Commit();
					Value = value + increment;
					e.Handled = e.SuppressKeyPress = true;
					return;
				case Keys.Down:
					Commit();
					Value = value - increment;
					e.Handled = e.SuppressKeyPress = true;
					return;
				case Keys.Escape:
					Render();
					e.Handled = e.SuppressKeyPress = true;
					return;
			}
			base.OnKeyDown(e);
		}

		protected override void OnMouseWheel(MouseEventArgs e)
		{
			if (Enabled)
			{
				Commit();
				Value = value + Math.Sign(e.Delta) * increment;
			}
			base.OnMouseWheel(e);
		}

		protected override void OnEnter(EventArgs e)
		{
			base.OnEnter(e);
			SelectAll();
		}

		protected override void OnEnabledChanged(EventArgs e)
		{
			base.OnEnabledChanged(e);
			ForeColor = Enabled ? StudioTheme.Ink : StudioTheme.Faint;
			BackColor = Enabled ? StudioTheme.Field : StudioTheme.Blend(StudioTheme.Field, StudioTheme.Surface, 0.5);
		}
	}
}
