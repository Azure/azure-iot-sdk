// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Models.CertificateManagement
{
    /// <summary>
    /// Represents an error response from IoT hub about a certificate signing request.
    /// </summary>
    /// <remarks>
    /// IoT hub does not populate every field on every error, so every field here is optional. A field that IoT hub
    /// omitted, or sent in a shape this client did not recognize, is left at its default value.
    /// </remarks>
    public class CertificateSigningRequestErrorResponse
    {
        /// <summary>
        /// The IoT hub error code (for example, 400040 for a certificate signing request that could not be decoded, or
        /// 409005 for a conflicting active request). Zero if IoT hub did not send one.
        /// </summary>
        [JsonPropertyName("errorCode")]
        public int ErrorCode { get; set; }

        /// <summary>
        /// A human readable description of the error.
        /// </summary>
        [JsonPropertyName("message")]
        public string? Message { get; set; }

        /// <summary>
        /// The IoT hub tracking ID for this error, for support purposes.
        /// </summary>
        [JsonPropertyName("trackingId")]
        public string? TrackingId { get; set; }

        /// <summary>
        /// When IoT hub generated this error.
        /// </summary>
        [JsonPropertyName("timestampUtc")]
        public DateTimeOffset? TimestampUtc { get; set; }

        /// <summary>
        /// Certificate management specific details about the error.
        /// </summary>
        [JsonPropertyName("info")]
        public CertificateSigningRequestErrorInfo Info { get; set; } = new();

        /// <summary>
        /// How long IoT hub asked this device to wait before trying again, if it asked at all.
        /// </summary>
        [JsonPropertyName("retryAfter")]
        public int? RetryAfterSeconds { get; set; }

        /// <summary>
        /// Certificate management specific details about a certificate signing request error.
        /// </summary>
        public class CertificateSigningRequestErrorInfo
        {
            /// <summary>
            /// Correlation ID for diagnostic and support purposes.
            /// </summary>
            [JsonPropertyName("correlationId")]
            public string? CorrelationId { get; set; }

            /// <summary>
            /// The certificate management specific error (for example, "CertificateSigningRequestInvalid").
            /// </summary>
            [JsonPropertyName("credentialError")]
            public string? CredentialError { get; set; }

            /// <summary>
            /// A human readable description of <see cref="CredentialError"/>.
            /// </summary>
            [JsonPropertyName("credentialMessage")]
            public string? CredentialMessage { get; set; }

            /// <summary>
            /// When the operation this error refers to expires, if IoT hub reported it.
            /// </summary>
            [JsonPropertyName("operationExpires")]
            public DateTimeOffset? OperationExpires { get; set; }
        }
    }
}
