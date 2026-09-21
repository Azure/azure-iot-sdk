using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.Models
{
    public class ConnectionContext
    {
        // Users should not be constructing this object. It should only be returned to the user.
        internal ConnectionContext()
        { 
        
        }

        public required string DeviceId { get; init; }

        public required string IotHubHostName { get; init; }

        public required ConnectionProfile ConnectionProfile { get; init; }

        public IReadOnlyList<string>? IssuedClientCertificates { get; init; }

        public required X509AuthenticationProvider AuthenticationProvider { get; set; }
    }
}
