namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class ConnectionContext //TODO split it up so that user who passes in connection context doesn't try to assign twin push and issued certs?
    {
        public Models.Twin.DeviceTwin? InitialTwinPush { get; internal set; } //TODO is it more appropriate to just let the twinclient catch this push?

        public required string DeviceId { get; init; }

        public required string IotHubHostName { get; init; }

        //TODO naming since AEG is implementation detail on service side
        public required bool IsAzureEventGrid { get; init; }

        public IReadOnlyList<string>? IssuedClientCertificates { get; init; }

        public required X509AuthenticationProvider AuthenticationProvider { get; init; }
    }
}
