using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class ConnectionClient : IConnectionClient
    {
        private IMqttClient _mqttClient;

        private ConnectionContext? CurrentConnectionContext { get; set; }

        //TODO include twin push details upon each connect event? Or just leave that to the twin client to handle?
        public IMqttClient MqttClient => _mqttClient;

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public ConnectionClient(ConnectionClientOptions options)
        {
            options ??= new ConnectionClientOptions();

            MqttSessionClientOptions sessionClientOptions = new()
            {
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                EnableMqttLogging = options.EnableMqttLogging,
            };

            _mqttClient = options.MqttClient ?? new MqttSessionClient(sessionClientOptions);
        }

        // This ctor allows the unified clients to create Gen2-specific connection clients to feed into the Gen2-specific feature clients
        internal ConnectionClient(Unified.Connection.IConnectionClient unifiedConnection)
        {
            //TODO does this pass by value cause issues later? How does it work when current connection status gets updated? Reprovisioning in particular?
            _mqttClient = unifiedConnection.MqttClient;
            var unifiedConnectionContext = unifiedConnection.GetCurrentConnectionContext();
            if (unifiedConnectionContext != null)
            {
                CurrentConnectionContext = new()
                {
                    DeviceId = unifiedConnectionContext.DeviceId,
                    IsAzureEventGrid = unifiedConnectionContext.IsAzureEventGrid,
                    InitialTwinPush = null,
                    IotHubHostName = unifiedConnectionContext.IotHubHostName,
                    IssuedClientCertificates = unifiedConnectionContext.IssuedClientCertificates,
                };
            }
        }

        /// <summary>
        /// Empty constructor mostly for mocking purposes
        /// </summary>
        public ConnectionClient()
        {
            _mqttClient = new MqttSessionClient(new());
        }

        /// <summary>
        /// Provision this device with the provided credentials using Device Provisioning Service, then connect this device to the IoT hub it was provisioned to.
        /// </summary>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="twinOptions">The optional flags to control twin updates to this device from IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, TwinPushOptions? twinOptions = default, CancellationToken cancellationToken = default)
        {
            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            //TODO several mqtt client options should not be provided by the user (ie, host name). Add checks here that validate all of them

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId,
                IotHubHostName = provisioningResult.AssignedHub,
                IsAzureEventGrid = provisioningResult.IsAzureEventGridHub,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,                
            };

            CurrentConnectionContext.InitialTwinPush = await ConnectAsync(CurrentConnectionContext, authentication, twinOptions, cancellationToken);

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
            CurrentConnectionContext = null;
        }

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(CertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            throw new NotImplementedException("Not a supported feature on Gen 2 Hubs yet");
        }

        /// <summary>
        /// Connect directly to IoT Hub
        /// </summary>
        /// <param name="connectionContext">The details about which IoT hub host to connect to, and which device Id to connect as.</param>
        /// <param name="authentication">The authentication to use when connecting.</param>
        /// <param name="twinPushOptions">The options around receiving a twin push upon connecting.</param>
        /// <param name="cancellationToken">Cancellation token.</param>
        /// <returns>The initial twin of the device if a twin push was configured via <see cref="TwinPushOptions"/></returns>
        public async Task<Models.Twin> ConnectAsync(ConnectionContext connectionContext, X509AuthenticationProvider authentication, TwinPushOptions? twinPushOptions = default, CancellationToken cancellationToken = default)
        {
            IotHubConnection iotHubConnection = new();

            CurrentConnectionContext = connectionContext;

            return await IotHubConnection.ConnectToAzureEventGridIotHubAsync(_mqttClient, connectionContext.IotHubHostName, connectionContext.DeviceId, authentication, twinPushOptions, cancellationToken);
        }

        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ProvisioningConnection provisioningConnection = new();
            return await provisioningConnection.RegisterAsync(_mqttClient, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO do we care about initial twin as returned by DPS?
        }

        public void Dispose()
        {
            _mqttClient.Dispose();
        }
    }
}
