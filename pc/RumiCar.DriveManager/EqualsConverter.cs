using System.Globalization;
using System.Windows.Data;

namespace RumiCar.DriveManager;

/// <summary>True when all bound values are equal as strings (used to highlight the active mode button).</summary>
public sealed class EqualsConverter : IMultiValueConverter
{
    public static readonly EqualsConverter Instance = new();

    public object Convert(object[] values, Type targetType, object? parameter, CultureInfo culture)
    {
        if (values.Length < 2) return false;
        var first = values[0]?.ToString();
        if (first is null) return false;
        for (int i = 1; i < values.Length; i++)
        {
            if (!string.Equals(first, values[i]?.ToString(), StringComparison.Ordinal)) return false;
        }
        return true;
    }

    public object[] ConvertBack(object? value, Type[] targetTypes, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
