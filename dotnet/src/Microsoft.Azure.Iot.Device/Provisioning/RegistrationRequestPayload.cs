// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// Optional data to be included in the registration request.
    /// </summary>
    public class RegistrationRequestPayload
    {
        /// <summary>
        /// Additional (optional) JSON data to be sent to the service.
        /// </summary>
        /// <remarks>
        /// The service supports passing a DTDL model Id, so one supported payload is <see cref="ModelIdPayload"/>.
        /// </remarks>
        [JsonPropertyName("payload")]
        public JsonNode? Payload { get; set; }

        [JsonPropertyName("csr")]
        public string? ClientCertificateSigningRequest { get; set; }
    }
}
