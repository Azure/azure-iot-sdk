using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Models
{
    // For example:
    //{
    //  "Message":"{\"errorCode\":400020,\"message\":\"Storage endpoint or blob name is invalid\",\"trackingId\":\"00C9CA3F088947F386D85FAB85D58690-G2:-TimeStamp:2026-07-06T19:04:28.725423523Z\",\"timestampUtc\":\"2026-07-06T19:04:28.725423523Z\",\"info\":null}",
    //  "ExceptionMessage":""
    //}

    public class IotHubServiceError
    {
        [JsonPropertyName("Message")]
        public required string ErrorDetails { get; set; }

        [JsonPropertyName("ExceptionMessage")]
        public string? ExceptionMessage { get; set; }
    }
}
