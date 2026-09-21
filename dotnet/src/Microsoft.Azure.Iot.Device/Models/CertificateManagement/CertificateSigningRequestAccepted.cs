// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Models.CertificateManagement
{
    /// <summary>
    /// Represents the 202 Accepted response from IoT Hub.
    /// Contains the correlation ID and operation expiration time.
    /// </summary>
    public class CertificateSigningRequestAccepted
    {
        /// <summary>
        /// Correlation ID for diagnostic and support purposes.
        /// </summary>
        [JsonPropertyName("correlationId")]
        public required string CorrelationId { get; set; }

        /// <summary>
        /// Time when the operation expires and will be discarded if not completed.
        /// Default is approximately 12 hours from acceptance.
        /// </summary>
        [JsonPropertyName("operationExpires")]
        public required DateTimeOffset OperationExpires { get; set; }
    }
}
