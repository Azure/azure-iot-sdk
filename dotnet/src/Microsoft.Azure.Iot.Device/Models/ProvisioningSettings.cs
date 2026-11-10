// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Models
{
    public class ProvisioningSettings
    {
        public ProvisioningSettings(string idScope)
        {
            IdScope = idScope;
        }

        public string? RegistrationId { get; set; }

        public string IdScope { get; set; }

        public string GlobalEndpointAddress { get; set; } = "global.azure-devices-provisioning.net";

        public JsonNode? ProvisioningPayload { get; set; }

        public ProvisioningCertificateSigningRequest? CertificateSigningRequest { get; set; }

        /// <summary>
        /// The protocol used to register with Device Provisioning Service. Defaults to <see cref="ProvisioningTransport.Mqtt"/>.
        /// </summary>
        /// <remarks>
        /// Use <see cref="ProvisioningTransport.Http"/> where MQTT traffic to Device Provisioning Service is blocked. The
        /// device still connects to its assigned IoT hub over MQTT.
        /// </remarks>
        public ProvisioningTransport Transport { get; set; } = ProvisioningTransport.Mqtt;
    }
}
