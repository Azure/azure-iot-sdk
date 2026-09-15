using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Models
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
    }
}
