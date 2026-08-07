using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.FileUpload.Models
{
    // For example:
    /*
    {
      "errorCode": 400020,
      "message": "Storage endpoint or blob name is invalid",
      "trackingId": "00C9CA3F088947F386D85FAB85D58690-G2:-TimeStamp:2026-07-06T19:04:28.725423523Z",
      "timestampUtc": "2026-07-06T19:04:28.725423523Z",
      "info": null
    }
     */
    public class IotHubNestedServiceException
    {
        [JsonPropertyName("errorCode")]
        public required int ErrorCode { get; set; }

        [JsonPropertyName("message")]
        public required string Message { get; set; }

        [JsonPropertyName("trackingId")]
        public required string TrackingId { get; set; }

        [JsonPropertyName("timestampUtc")]
        public required string TimestampUtc { get; set; }

        [JsonPropertyName("info")]
        public string? Info { get; set; } //TODO what is this?
    }
}
