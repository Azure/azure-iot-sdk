using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Diagnostics;

namespace Microsoft.Azure.Devices.Client
{
    //TODO Does my setup already allow for user to publish via feature clients even when this client is connected to DPS during identity terminal exception handling? It does, right?

    public abstract class GenericConnectionClient
    {
        // The abstract methods cover all the differences between a gen2 client and a unified client.
        public abstract MqttConnect MqttConnectOverride(MqttConnect connect);

        // In gen2 case, SUB to devicebound, send birth message, wait for birth ack. In gen1 case, send all DM/Twin/Telem SUBs.
        // In both cases, this method should trigger the "OnDevicePresenceFlowCompleted" callback
        public abstract Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args);

        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;

        // Workaround so that inheriting classes can invoke the "DevicePresenceFlowCompletedAsync" event.
        protected Task RaiseDevicePresenceFlowCompletedAsync(DevicePresenceFlowCompletedArgs args)
        {
            if (DevicePresenceFlowCompletedAsync != null)
            {
                DevicePresenceFlowCompletedAsync.Invoke(args);
            }

            return Task.CompletedTask;
        }




        internal bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        internal MqttConnectionManager ManagedMqttConnection;

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync;

        internal ConnectionContext? CurrentConnectionContext { get; set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;







        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public GenericConnectionClient(ConnectionClientOptions? options = null)
        {
            options ??= new ConnectionClientOptions();

            // This is the basic MQTT client that has no reconnection/retry logic
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(enableMqttLogs: options.EnableMqttLogging);

            // This is the wrapper that manages reconnection
            ManagedMqttConnection = new(unmanagedMqttClient, options.ConnectionAttemptTimeout, options.ConnectionRetryPolicy);
            ManagedMqttConnection.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client
        }

        private async Task DelegatePublishAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        /// <summary>
        /// Provision this device with the provided credentials using Device Provisioning Service, then connect this device to the IoT hub it was provisioned to.
        /// </summary>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="twinOptions">The optional flags to control twin updates to this device from IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // Remove any IoT Hub-specific handling of connect attempts when provisioning.
            ClearHubCallbacks();

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
                IsGen2Hub = true,
            };

            await ConnectAsync(CurrentConnectionContext, null, cancellationToken);

            return CurrentConnectionContext;
        }

        internal void ClearHubCallbacks()
        {
            ManagedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
        }

        private Task<MqttConnect> ConstructConnectPatcketAsync(MqttConnect connectPacketToEdit)
        {
            return Task.FromResult(MqttConnectOverride(connectPacketToEdit));
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            await ManagedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
            CurrentConnectionContext = null;
        }



        /// <summary>
        /// Connect directly to IoT Hub
        /// </summary>
        /// <param name="connectionContext">The details about which IoT hub host to connect to, and which device Id to connect as.</param>
        /// <param name="authentication">The authentication to use when connecting.</param>
        /// <param name="twinPushOptions">The options around receiving a twin push upon connecting.</param>
        /// <param name="cancellationToken">Cancellation token.</param>
        /// <returns>The initial twin of the device if a twin push was configured via <see cref="TwinPushOptions"/></returns>
        internal async Task ConnectAsync(ConnectionContext connectionContext, TwinPushOptions? twinPushOptions = default, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // Setup mqtt client to handle connections now that they will connect to IoT Hub rather than DPS
            ManagedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync; //Covers against calling connect, losing connection, then calling connect again?
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            ManagedMqttConnection.ConnectingAsync += ConstructConnectPatcketAsync;
            ManagedMqttConnection.ConnectedAsync += HandleConnectedToHubAsync;

            CurrentConnectionContext = connectionContext;

            string deviceId = CurrentConnectionContext.DeviceId;
            string hostname = CurrentConnectionContext.IotHubHostName;

            cancellationToken.ThrowIfCancellationRequested();

            Trace.TraceInformation("Attempting to establish connection and presence for device {0} with IoT Hub {1}", deviceId, hostname);

            TaskCompletionSource<DevicePresenceFlowCompletedArgs> devicePresenceFlowResult = new();
            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                devicePresenceFlowResult.TrySetResult(args);
                return Task.CompletedTask;
            };

            // Setup callbacks BEFORE sending CONNECT so that CONNACK can be handled regardless of how quickly it arrives
            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;

            try
            {
                // This is the common shape of all connect packets for this device to this hub. There is an override per-connect attempt elsewhere that
                // sets the connect nonce on the username field here. This is done so that a new connect nonce can be injected even during reconnection attempts
                MqttConnect connectPacket = new MqttConnect()
                {
                    HostName = hostname,
                    TcpPort = 8883,
                    WebsocketPort = 443,
                    WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                    ClientCertificate = CurrentConnectionContext.AuthenticationProvider.ClientCertificate,
                    CleanSession = true, // TODO user configurable? Less applicable in gen 2 hub connection
                    Password = Array.Empty<byte>(),
                    ProtocolVersion = MqttProtocolVersion.V500,
                    ClientId = deviceId,
                };

                MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connectPacket, cancellationToken);

                var devicePresenceFlowCompletedArgs = await devicePresenceFlowResult.Task.WaitAsync(cancellationToken);

                //TODO retry? Feels a bit odd to retry a connect call, but Hub folks do have a prescribed pattern for connect attempts. Maybe offer one connect with retry, one connect w/o
                if (devicePresenceFlowCompletedArgs.Exception != null)
                {
                    throw devicePresenceFlowCompletedArgs.Exception;
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
            }
        }

        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ProvisioningConnection provisioningConnection = new();
            return await provisioningConnection.RegisterAsync(ManagedMqttConnection, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO do we care about initial twin as returned by DPS?
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public virtual void Dispose(bool disposing)
        {
            ManagedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
            ManagedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;

            if (disposing)
            {
                ManagedMqttConnection.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                ManagedMqttConnection.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public virtual void Dispose()
        {
            ManagedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
            ManagedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;

            ManagedMqttConnection.Dispose();

            _isDisposed = true;
        }

        private async Task<TResp> PerformWhileRespectingConnectionState<TResp>(Func<CancellationToken, Task<TResp>> funcToRetry, CancellationToken cancellationToken)
        {
            // Assume an open connection to start, but increment this by one if MqttClientNotConnectedException is thrown to counteract that assumption
            using ManualResetEventSlim latch = new();
            latch.Set();
            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                latch.Set();
                return Task.CompletedTask;
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            try
            {
                while (true) // Retry sending publish until user cancels as long as the failure is just that the underlying mqtt client was disconnected.
                {
                    try
                    {
                        return await funcToRetry.Invoke(cancellationToken);
                    }
                    catch (MqttClientNotConnectedException)
                    {
                        latch.Reset(); // No-op if the HandleDisconnection already reset this latch. Only here because there is a chance that this method was called while MQTT client was disconnected

                        try
                        {
                            latch.Wait(cancellationToken); // Wait for device birth flow to finish before resuming this feature client-level traffic
                        }
                        catch (OperationCanceledException)
                        {
                            throw new OperationCanceledException("Operation canceled while waiting for reconnection to finish");
                        }
                    }
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
            }
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttPublishAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.PublishAsync(publish, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttSubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.SubscribeAsync(subscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttUnsubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.UnsubscribeAsync(unsubscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }
    }
}
