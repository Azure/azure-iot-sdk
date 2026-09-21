using System.Diagnostics.CodeAnalysis;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    internal static class MqttUserPropertyListExtensions
    {
        internal static bool TryGetProperty(this List<MqttUserProperty> userProperties, string name, out string? value)
        {
            value = default;
            if (userProperties == null)
            {
                return false;
            }

            var property = userProperties.FirstOrDefault(x => x.Name == name);
            if (property != null)
            {
                value = Encoding.UTF8.GetString(property.Value.ToArray());
                return true;
            }

            return false;
        }

        internal static bool TryGetType(this List<MqttUserProperty> userProperties, [NotNullWhen(returnValue: true)] out string? type, [NotNullWhen(returnValue: true)] out int? value)
        {
            type = default;
            value = default;
            if (userProperties == null)
            {
                return false;
            }

            var property = userProperties.FirstOrDefault(x => x.Name == "type");
            if (property != null)
            {
                var userPropValue = Encoding.UTF8.GetString(property.Value.ToArray());
                var split = userPropValue.Split(":");
                if (split.Length != 2)
                {
                    return false;
                }

                type = split[0];
                try
                {
                    value = int.Parse(split[1]);
                    return true;
                }
                catch
                {
                    return false;
                }
            }

            return false;
        }

        internal static bool TryGetPropertyBuffer(this List<MqttUserProperty> userProperties, string name, out ReadOnlyMemory<byte> value)
        {
            value = default;
            if (userProperties == null)
            {
                return false;
            }

            var property = userProperties.FirstOrDefault(x => x.Name == name);
            if (property != null)
            {
                value = property.Value;
                return true;
            }

            return false;
        }
    }

    /// <summary>
    /// Extension methods for reading MqttUserProperty values.
    /// </summary>
    public static class MqttUserPropertyExtensions
    {
        /// <summary>
        /// Reads the value of the user property as a UTF-8 string.
        /// </summary>
        /// <param name="userProperty">The user property to read.</param>
        /// <returns>The value as a string.</returns>
        public static string ReadValueAsString(this MqttUserProperty userProperty)
        {
            ArgumentNullException.ThrowIfNull(userProperty);

            var buffer = userProperty.Value;
            if (buffer.IsEmpty)
            {
                return string.Empty;
            }

            return Encoding.UTF8.GetString(buffer.Span);
        }
    }
}
