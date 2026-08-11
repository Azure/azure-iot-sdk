namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class ConnectionContext //TODO split it up so that user who passes in connection context doesn't try to assign twin push and issued certs?
    {
        public required string DeviceId { get; init; }

        public required string IotHubHostName { get; init; }

        public IReadOnlyList<string>? IssuedClientCertificates { get; init; }

        public required X509AuthenticationProvider AuthenticationProvider { get; init; }
    }
}
