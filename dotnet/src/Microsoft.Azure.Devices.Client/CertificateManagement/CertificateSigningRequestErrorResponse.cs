// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.CertificateManagement
{
    /// <summary>
    /// Represents an error response from IoT hub about a certificate signing request.
    /// </summary>
    public class CertificateSigningRequestErrorResponse
    {
        [JsonPropertyName("errorCode")]
        public int ErrorCode { get; set; }

        [JsonPropertyName("message")]
        public string Message { get; set; }

        [JsonPropertyName("trackingId")]
        public string TrackingId { get; set; }

        [JsonPropertyName("timestampUtc")]
        public DateTimeOffset TimestampUtc { get; set; }

        [JsonPropertyName("info")]
        public CertificateSigningRequestErrorInfo Info { get; set; }

        [JsonPropertyName("retryAfter")]
        public int? RetryAfterSeconds { get; set; }

        public class CertificateSigningRequestErrorInfo
        {
            [JsonPropertyName("correlationId")]
            public string CorrelationId { get; set; }

            [JsonPropertyName("credentialError")]
            public string CertificateSigningRequestError { get; set; }

            [JsonPropertyName("credentialMessage")]
            public string CertificateSigningRequestMessage { get; set; }

            [JsonPropertyName("requestId")]
            public string RequestId { get; set; }

            [JsonPropertyName("operationExpires")]
            public DateTimeOffset? OperationExpires { get; set; }
        }
    }
}
