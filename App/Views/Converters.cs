using System.Globalization;
using Avalonia;
using Avalonia.Data.Converters;
using Avalonia.Media;
using RocksmithAudioBridge.App.ViewModels;

namespace RocksmithAudioBridge.App.Views;

/// <summary>StatusTone to the overlay's status colours (Good / Warn / Bad), Muted for neutral.</summary>
public sealed class ToneBrushConverter : IValueConverter
{
	public static readonly ToneBrushConverter Instance = new();

	public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
	{
		string key = value switch
		{
			StatusTone.Good => "GoodBrush",
			StatusTone.Warn => "WarnBrush",
			StatusTone.Bad => "BadBrush",
			_ => parameter as string ?? "MutedBrush",
		};
		return Application.Current != null && Application.Current.TryGetResource(key, Application.Current.ActualThemeVariant, out object? brush) && brush is IBrush found ? found : Brushes.Gray;
	}

	public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) => throw new NotSupportedException();
}
