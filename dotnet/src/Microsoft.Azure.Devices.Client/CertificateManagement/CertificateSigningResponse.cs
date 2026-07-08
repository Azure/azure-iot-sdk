// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.CertificateManagement
{
    /// <summary>
    /// Represents the response from IoT Hub containing issued certificates.
    /// </summary>
    public class CertificateSigningResponse
    {
        /// <summary>
        /// List of Base64-encoded certificates in the certificate chain.
        /// The first certificate is the issued device certificate, followed by intermediates.
        /// </summary>
        [JsonPropertyName("certificates")]
        public required IReadOnlyList<string> Certificates { get; set; }

        /// <summary>
        /// Correlation ID for diagnostic and support purposes.
        /// </summary>
        [JsonPropertyName("correlationId")]
        public required string CorrelationId { get; set; }
    }
}
