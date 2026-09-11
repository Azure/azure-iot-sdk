using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client
{
    //TODO Does my setup already allow for user to publish via feature clients even when this client is connected to DPS during identity terminal exception handling? It does, right?

    public abstract class AbstractConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync;
        
        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;
        
        /// <summary>
        /// Raised once the provisioning flow that runs upon connecting to Device Provisioning Service has either
        /// produced a registration result or failed. This is the provisioning counterpart of
        /// <see cref="DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        private event Func<ProvisioningFlowCompletedArgs, Task>? ProvisioningFlowCompletedAsync;

        private const string ProvisioningUsernameFormat = "{0}/registrations/{1}/api-version={2}&ClientVersion={3}";
        private const string ProvisioningApiVersion = "2019-03-31";
        private const string ProvisioningSubscribeFilter = "$dps/registrations/res/#";
        private const string ProvisioningRegisterTopic = "$dps/registrations/PUT/iotdps-register/?$rid={0}";
        private const string ProvisioningGetOperationsTopic = "$dps/registrations/GET/iotdps-get-operationstatus/?$rid={0}&operationId={1}";
        private const string RetryAfterHeader = "Retry-After";

        private static readonly TimeSpan s_defaultOperationPollingInterval = TimeSpan.FromSeconds(2);

        // The abstract methods cover all the differences between a gen2 client and a unified client.
        public abstract MqttConnect MqttConnectOverride(MqttConnect connect);

        // In gen2 case, SUB to devicebound, send birth message, wait for birth ack. In gen1 case, send all DM/Twin/Telem SUBs.
        // In both cases, this method should trigger the "OnDevicePresenceFlowCompleted" callback
        public abstract Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args);

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

        internal ConnectionContext? CurrentConnectionContext { get; set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// The endpoint that this client is currently connecting to, or connected to.
        /// </summary>
        /// <remarks>
        /// This decides which flow runs when a connection is established: connecting to Device Provisioning Service
        /// starts the provisioning flow, while connecting to an IoT hub starts the device presence flow. The connection
        /// layer owns reconnection for both endpoints, so this also decides which flow a reconnection restarts.
        /// </remarks>
        public ConnectionEndpoint CurrentEndpoint { get; private set; } = ConnectionEndpoint.None;

        // The registration request to send on every connection to Device Provisioning Service. Only set while provisioning.
        private RegistrationRequestPayload? _provisioningRequestPayload;

        // The inputs of the most recent provisioning run, kept for the lifetime of this client so that a connection
        // that faults on this device's identity can be recovered from by provisioning again with the same credentials.
        private ProvisioningSettings? _lastProvisioningSettings;
        private X509AuthenticationProvider? _lastProvisioningAuthentication;

        // Set while a re-provisioning attempt started by an identity fault is in flight. Only one such attempt may run
        // at a time because each one takes over this client's single connection.
        private int _isReprovisioning;

        /// <summary>
        /// Cancels the re-provisioning attempt that an identity fault started, if one is in flight. Because that attempt
        /// runs on its own, it is only abandoned when this client is deliberately disconnected or disposed.
        /// </summary>
        private CancellationTokenSource? _currentReprovisioningCancellation;

        /// <summary>
        /// The fault that ended connection maintenance without anything left to bring the connection back, if this
        /// client has hit one. Cleared as soon as this client starts establishing a connection again.
        /// </summary>
        private DeviceException? _unrecoverableFault;

        /// <summary>
        /// Raised when <see cref="_unrecoverableFault"/> is set, so that operations waiting for the connection to come
        /// back stop waiting for something that will never happen.
        /// </summary>
        private event Action? UnrecoverablyFaulted;

        // The DPS responses that the in-progress provisioning flow is waiting on. Both are reset for each new connection
        // to DPS since a registration only lives as long as the connection it was started on.
        private TaskCompletionSource<RegistrationOperationStatus>? _startProvisioningRequestStatusSource;
        private TaskCompletionSource<RegistrationOperationStatus>? _checkRegistrationOperationStatusSource;
        private int _provisioningRequestId;

        /// <summary>
        /// Cancels everything the in-progress provisioning flow is waiting on. Because DPS cannot persist sessions, a
        /// flow is only valid for the connection it was started on, so this is cancelled whenever that connection ends.
        /// </summary>
        private CancellationTokenSource? _currentProvisioningFlowCancellation;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public AbstractConnectionClient(ConnectionClientOptions? options = null)
        {
            options ??= new ConnectionClientOptions();

            // This is the basic MQTT client that has no reconnection/retry logic
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(enableMqttLogs: options.EnableMqttLogging);

            // This is the wrapper that manages reconnection
            ManagedMqttConnection = new(unmanagedMqttClient, options.ConnectionAttemptTimeout, options.ConnectionRetryPolicy);
            ManagedMqttConnection.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client

            // These handlers stay attached for the lifetime of this client and dispatch on the endpoint that the current
            // connection targets, so that a connection to DPS runs the provisioning flow and a connection to IoT hub runs
            // the device presence flow.
            ManagedMqttConnection.ConnectingAsync += PatchConnectPacketAsync;
            ManagedMqttConnection.ConnectedAsync += HandleConnectedAsync;
            ManagedMqttConnection.DisconnectedAsync += HandleDisconnectedAsync;
            ManagedMqttConnection.ConnectionFaultedAsync += HandleConnectionFaultedAsync;
            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedProvisioningPublishAsync;
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
        /// <remarks>
        /// If the connection to the assigned IoT hub later faults because of this device's identity, this client
        /// provisions again with these same credentials and reconnects to whichever hub it is assigned, without the
        /// application having to do anything.
        /// </remarks>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="twinOptions">The optional flags to control twin updates to this device from IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
                IsGen2Hub = provisioningResult.IsAzureEventGridHub,
            };

            await ConnectAsync(CurrentConnectionContext, cancellationToken);

            return CurrentConnectionContext;
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

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // The application is closing this connection deliberately, so any recovery that an earlier identity fault
            // started is no longer wanted.
            CancelCurrentReprovisioning();

            await ManagedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
            CurrentConnectionContext = null;

            Trace.TraceInformation("ConnectionClient's current endpoint is now neither IoT Hub or DPS");
            CurrentEndpoint = ConnectionEndpoint.None;
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public virtual void Dispose(bool disposing)
        {
            DetachConnectionCallbacks();

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
            DetachConnectionCallbacks();

            ManagedMqttConnection.Dispose();

            _isDisposed = true;
        }

        internal async Task ConnectAsync(ConnectionContext connectionContext, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // From here on, every connection this client establishes targets IoT hub, so every connection (including the
            // ones the connection layer re-establishes on its own) runs the device presence flow.
            Trace.TraceInformation("ConnectionClient's current endpoint is now IoT Hub");
            CurrentEndpoint = ConnectionEndpoint.IotHub;

            // This client is establishing a connection again, so any earlier fault no longer describes its state.
            ClearUnrecoverableFault();

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
                // These are the connect packet fields that are invariable between connect attempts to any kind of IoT Hub. There is an MQTT connect packet override
                // step triggered by the "ConnectingAsync" callback each time a connect is attempted. This allows for the gen 2 client to insert a new connect nonce
                // per connect attempt. It also allows the inheriting ConnectionClient to insert the appropriate username/password/websocket port for that hub type
                MqttConnect connectPacket = new MqttConnect()
                {
                    HostName = hostname,
                    TcpPort = 8883,
                    WebsocketPort = 443,
                    ClientCertificate = CurrentConnectionContext.AuthenticationProvider.ClientCertificate,
                    CleanSession = true, // TODO user configurable? Less applicable in gen 2 hub connection
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

        /// <summary>
        /// Patch the CONNECT packet that is about to be sent, including on every reconnect attempt.
        /// </summary>
        private Task<MqttConnect> PatchConnectPacketAsync(MqttConnect connectPacketToEdit)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // The CONNECT sent to DPS is fully formed before the first connect attempt and nothing about it changes
                // between attempts or between the generation of Hub that the inheriting class uses, so there is nothing to inject here.
                return Task.FromResult(connectPacketToEdit);
            }

            // Gen 1 hub clients and gen 2 hub clients can insert their specific username/password/protocol version/etc before each connect attempt
            return Task.FromResult(MqttConnectOverride(connectPacketToEdit));
        }

        /// <summary>
        /// Run the flow that the endpoint of the newly established connection expects.
        /// </summary>
        private async Task HandleConnectedAsync(MqttClientConnectedEventArgs args)
        {
            switch (CurrentEndpoint)
            {
                case ConnectionEndpoint.DeviceProvisioningService:
                    await HandleConnectedToDpsAsync(args);
                    break;
                case ConnectionEndpoint.IotHub:
                    await HandleConnectedToHubAsync(args);
                    break;
                default:
                    Trace.TraceWarning("Connection was established while this client was not targeting any endpoint. Ignoring it.");
                    break;
            }
        }

        private Task HandleDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // Because DPS cannot persist sessions, any connection loss should be treated as a session loss. Abandon
                // the in-progress flow so that it can be restarted from the beginning once the connection is re-established.
                CancelCurrentProvisioningFlow();
            }

            return Task.CompletedTask;
        }

        private async Task HandleConnectionFaultedAsync(MqttConnectionFaultedEventArgs args)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                Trace.TraceError("ConnectionClient encountered an unrecoverable error during provisioning.", args.Exception);
                // The connection layer has stopped maintaining the connection to DPS, so no further connection will
                // arrive to start the provisioning flow again.
                CancelCurrentProvisioningFlow();
                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(args.Exception));
                }

                return;
            }

            if (args.IsIdentityFault && TryStartReprovisioning(args))
            {
                // This client is recovering from the fault on its own, so anything waiting for the connection should
                // keep waiting for that recovery to re-establish it.
                return;
            }

            // Nothing is going to bring this connection back, so stop anything that is waiting for it.
            MarkUnrecoverablyFaulted(args.Exception);
        }

        /// <summary>
        /// Start provisioning this device again in response to a connection that faulted on this device's identity, and
        /// connect to the hub it gets assigned.
        /// </summary>
        /// <remarks>
        /// This is started rather than awaited because the fault is reported from within the connection layer's own
        /// maintenance, which must not be blocked while this client establishes a new connection to DPS.
        /// </remarks>
        /// <param name="args">The fault that this recovery is responding to.</param>
        /// <returns>True if this call started a recovery attempt, false if this client will not recover on its own.</returns>
        private bool TryStartReprovisioning(MqttConnectionFaultedEventArgs args)
        {
            if (args.LastDisconnect == null)
            {
                // The fault ended a connect attempt that a caller is waiting on, so that caller is told about it and
                // decides what to do. Recovering here as well would have both this client and that caller trying to
                // establish the same connection.
                Trace.TraceWarning("Not re-provisioning after an identity fault because the fault is reported to the caller that requested the connection.");
                return false;
            }

            ProvisioningSettings? provisioningSettings = _lastProvisioningSettings;
            X509AuthenticationProvider? provisioningAuthentication = _lastProvisioningAuthentication;

            if (provisioningSettings == null || provisioningAuthentication == null)
            {
                // This device was connected with credentials that the application supplied directly, so there is no
                // registration for this client to renew. Only the application can recover from here.
                Trace.TraceError("The connection faulted on this device's identity, but this device was not provisioned through Device Provisioning Service so it cannot re-provision. {0}", args.Exception);
                return false;
            }

            if (Interlocked.CompareExchange(ref _isReprovisioning, 1, 0) != 0)
            {
                // An earlier fault already started this recovery, and a second one would fight it over this client's connection.
                Trace.TraceInformation("Ignoring an identity fault because this device is already re-provisioning.");
                return true;
            }

            var reprovisioningCancellation = new CancellationTokenSource();
            _currentReprovisioningCancellation = reprovisioningCancellation;

            _ = Task.Run(async () =>
            {
                try
                {
                    Trace.TraceInformation("Re-provisioning this device because the connection faulted on its identity. {0}", args.Exception);

                    await ProvisionAndConnectAsync(provisioningSettings, provisioningAuthentication, reprovisioningCancellation.Token);

                    Trace.TraceInformation("Finished re-provisioning this device and connected it to the IoT hub it was assigned.");
                }
                catch (OperationCanceledException)
                {
                    Trace.TraceWarning("Re-provisioning was abandoned because this client was disconnected or disposed.");
                }
                catch (Exception e)
                {
                    // This task is unmonitored, so nothing may escape it.
                    Trace.TraceError("Failed to re-provision this device after the connection faulted on its identity. {0}", e);

                    // This recovery was the only thing left that could have re-established the connection, so anything
                    // waiting for it is waiting for something that will never happen.
                    MarkUnrecoverablyFaulted(AsUnrecoverableFault(e));
                }
                finally
                {
                    // Clear the field only if this attempt is still the current one, so that a newer attempt's cancellation source is left intact.
                    Interlocked.CompareExchange(ref _currentReprovisioningCancellation, null, reprovisioningCancellation);
                    reprovisioningCancellation.Dispose();
                    Volatile.Write(ref _isReprovisioning, 0);
                }
            });

            return true;
        }

        /// <summary>
        /// Record that this client has stopped maintaining its connection for a reason that neither the connection
        /// layer nor this client will recover from, and release everything that is waiting for the connection.
        /// </summary>
        private void MarkUnrecoverablyFaulted(DeviceException fault)
        {
            Trace.TraceError("ConnectionClient encountered an unrecoverable exception", fault);
            _unrecoverableFault = fault;

            UnrecoverablyFaulted?.Invoke();
        }

        /// <summary>
        /// Forget any earlier fault because this client is establishing a connection again.
        /// </summary>
        private void ClearUnrecoverableFault()
        {
            _unrecoverableFault = null;
        }

        /// <summary>
        /// Throw if this client has hit a fault that it will not recover from, so that an operation waiting for the
        /// connection to come back does not wait forever.
        /// </summary>
        private void ThrowIfUnrecoverablyFaulted()
        {
            DeviceException? fault = _unrecoverableFault;

            if (fault != null)
            {
                throw new OperationCanceledException("Operation canceled because this client hit a fault that it cannot recover from. See the inner exception for that fault.", fault);
            }
        }

        /// <summary>
        /// Present a failed recovery attempt as the classified fault that ended it, keeping the classification when the
        /// attempt already failed with one.
        /// </summary>
        private static DeviceException AsUnrecoverableFault(Exception exception)
        {
            if (exception is DeviceException deviceException)
            {
                return deviceException;
            }

            return new DeviceException("Failed to re-provision this device after the connection faulted on its identity.", exception)
            {
                Retryability = ErrorRetryability.Terminal,
                IsContained = false,
            };
        }

        /// <summary>
        /// Abandon the re-provisioning attempt that an identity fault started, if one is in flight.
        /// </summary>
        private void CancelCurrentReprovisioning()
        {
            CancellationTokenSource? reprovisioningCancellation = Interlocked.Exchange(ref _currentReprovisioningCancellation, null);

            try
            {
                reprovisioningCancellation?.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // The attempt already ended and disposed its own cancellation source, so there is nothing left to cancel.
            }
        }

        /// <summary>
        /// Connect to Device Provisioning Service and register this device.
        /// </summary>
        /// <remarks>
        /// The connection layer owns reconnection here just as it does for IoT hub connections. Because DPS cannot
        /// persist sessions, every connection it establishes starts a brand new registration in
        /// <see cref="HandleConnectedToDpsAsync"/>, which is the provisioning counterpart of the device presence flow.
        /// </remarks>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields.</param>
        /// <param name="authentication">The x509 authentication to use when connecting to Device Provisioning Service.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The terminal registration result reported by Device Provisioning Service.</returns>
        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            cancellationToken.ThrowIfCancellationRequested();

            _provisioningRequestPayload = new RegistrationRequestPayload()
            {
                ClientCertificateSigningRequest = null,
                Payload = provisioningSettings.ProvisioningPayload,
            };

            // Remember what this device provisioned with so that a connection that later faults on this device's
            // identity can be recovered from by provisioning again.
            _lastProvisioningSettings = provisioningSettings;
            _lastProvisioningAuthentication = authentication;

            // From here on, every connection this client establishes targets DPS, so every connection (including the ones
            // the connection layer re-establishes on its own) runs the provisioning flow.
            Trace.TraceInformation("ConnectionClient's current endpoint is now DPS");
            CurrentEndpoint = ConnectionEndpoint.DeviceProvisioningService;

            // This client is establishing a connection again, so any earlier fault no longer describes its state.
            ClearUnrecoverableFault();

            MqttConnect connect = CreateProvisioningConnectPacket(authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress);

            TaskCompletionSource<ProvisioningFlowCompletedArgs> provisioningFlowResult = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Task HandleProvisioningFlowCompletedAsync(ProvisioningFlowCompletedArgs args)
            {
                provisioningFlowResult.TrySetResult(args);
                return Task.CompletedTask;
            }

            // Setup the callback BEFORE sending CONNECT so that the CONNACK can be handled regardless of how quickly it arrives
            ProvisioningFlowCompletedAsync += HandleProvisioningFlowCompletedAsync;

            try
            {
                // MQTT connection manager already checks connack for non-success cases, so no need to check it here as well
                MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connect, cancellationToken);

                ProvisioningFlowCompletedArgs provisioningFlowCompletedArgs = await provisioningFlowResult.Task.WaitAsync(cancellationToken);

                if (provisioningFlowCompletedArgs.Exception != null)
                {
                    throw provisioningFlowCompletedArgs.Exception;
                }

                Debug.Assert(provisioningFlowCompletedArgs.RegistrationResult != null);

                //TODO do we care about initial twin as returned by DPS?
                return provisioningFlowCompletedArgs.RegistrationResult;
            }
            finally
            {
                ProvisioningFlowCompletedAsync -= HandleProvisioningFlowCompletedAsync;

                // Stop any provisioning flow that is still waiting on a DPS response now that no one is listening for its result.
                CancelCurrentProvisioningFlow();
                _provisioningRequestPayload = null;
                Trace.TraceInformation("ConnectionClient's current endpoint is now neither Hub or DPS");
                CurrentEndpoint = ConnectionEndpoint.None;

                // Always close the MQTT connection once provisioning has finished so that the connection can be
                // re-established against the assigned IoT hub.
                var disconnect = new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection };

                try
                {
                    await ManagedMqttConnection.DisconnectAsync(false, disconnect, CancellationToken.None);
                }
                catch (Exception)
                {
                    // Deliberately not rethrowing the exception because this is a "best effort" close.
                    // The service may not have acknowledged that the client closed the connection, but
                    // all local resources have been closed. The service will eventually realize the
                    // connection is closed in cases like these.
                }
            }
        }

        /// <summary>
        /// Run the provisioning flow upon connecting to Device Provisioning Service, in the same way that
        /// <see cref="HandleConnectedToHubAsync(MqttClientConnectedEventArgs)"/> runs the device presence flow upon
        /// connecting to IoT hub.
        /// </summary>
        /// <remarks>
        /// DPS cannot persist sessions, so the CONNACK's session present flag is deliberately ignored: every connection
        /// starts a brand new registration.
        /// </remarks>
        private async Task HandleConnectedToDpsAsync(MqttClientConnectedEventArgs args)
        {
            // Any flow left over from a previous connection belongs to a session that no longer exists, so abandon it.
            CancelCurrentProvisioningFlow();

            var provisioningFlowCancellation = new CancellationTokenSource();
            _currentProvisioningFlowCancellation = provisioningFlowCancellation;

            // Responses to the previous connection's requests are no longer expected.
            _startProvisioningRequestStatusSource = null;
            _checkRegistrationOperationStatusSource = null;

            try
            {
                await SubscribeToRegistrationResponseMessagesAsync(provisioningFlowCancellation.Token);

                RegistrationOperationStatus registrationStatus = await PublishRegistrationRequestAsync(provisioningFlowCancellation.Token);

                DeviceRegistrationResult registrationResult = await PollUntilProvisioningFinishesAsync(
                    registrationStatus.OperationId,
                    provisioningFlowCancellation.Token);

                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(registrationResult));
                }
            }
            catch (OperationCanceledException)
            {
                // The connection this flow was running on ended, or provisioning was abandoned altogether. If the
                // connection layer re-establishes the connection, this callback starts the flow over from the beginning.
                Trace.TraceWarning("Provisioning flow was abandoned because the connection to DPS ended.");
            }
            catch (Exception e)
            {
                Trace.TraceError("Exception thrown while running the provisioning flow. {0}", e);
                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(e));
                }
            }
            finally
            {
                // Clear the field only if this flow is still the current one, so that a newer flow's cancellation source is left intact.
                Interlocked.CompareExchange(ref _currentProvisioningFlowCancellation, null, provisioningFlowCancellation);
                provisioningFlowCancellation.Dispose();
            }
        }

        private void CancelCurrentProvisioningFlow()
        {
            CancellationTokenSource? provisioningFlowCancellation = Interlocked.Exchange(ref _currentProvisioningFlowCancellation, null);

            try
            {
                provisioningFlowCancellation?.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // The flow already ended and disposed its own cancellation source, so there is nothing left to cancel.
            }
        }

        private async Task SubscribeToRegistrationResponseMessagesAsync(CancellationToken cancellationToken)
        {
            Trace.TraceInformation("Subscribing to DPS response topic {0}", ProvisioningSubscribeFilter);
            MqttSubscribeAck subscribeResults = await ManagedMqttConnection.SubscribeAsync(new(ProvisioningSubscribeFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);

            if (subscribeResults.Items.FirstOrDefault()!.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
            {
                throw new Exception("todo");
            }
        }

        private async Task<RegistrationOperationStatus> PublishRegistrationRequestAsync(CancellationToken cancellationToken)
        {
            byte[] serializedPayload = Array.Empty<byte>();
            if (_provisioningRequestPayload != null)
            {
                string requestString = JsonSerializer.Serialize(_provisioningRequestPayload, JsonSerializationSettings.Options);
                serializedPayload = Encoding.UTF8.GetBytes(requestString);
            }

            string registrationTopic = string.Format(CultureInfo.InvariantCulture, ProvisioningRegisterTopic, ++_provisioningRequestId);
            MqttPublish publish = new MqttPublish()
            {
                Payload = serializedPayload,
                Topic = registrationTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
            };

            _startProvisioningRequestStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

            Trace.TraceInformation("Publishing to DPS on topic {0}", registrationTopic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(publish, cancellationToken);

            Trace.TraceInformation("Successfully published registration request to DPS with request Id {0}", _provisioningRequestId);

            try
            {
                RegistrationOperationStatus registrationStatus = await _startProvisioningRequestStatusSource.Task.WaitAsync(cancellationToken);

                return registrationStatus.Status != ProvisioningRegistrationStatus.Assigning
                    ? throw new Exception("TODO")
                    : registrationStatus;
            }
            catch (OperationCanceledException e)
            {
                throw new OperationCanceledException("Timed out waiting for DPS to send the initial provisioning response", e);
            }
        }

        private async Task<DeviceRegistrationResult> PollUntilProvisioningFinishesAsync(string operationId, CancellationToken cancellationToken)
        {
            while (true)
            {
                string topic = string.Format(CultureInfo.InvariantCulture, ProvisioningGetOperationsTopic, ++_provisioningRequestId, operationId);
                MqttPublish message = new MqttPublish()
                {
                    Topic = topic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
                };

                _checkRegistrationOperationStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

                Trace.TraceInformation("Publishing to DPS on topic {0}", topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(message, cancellationToken);

                RegistrationOperationStatus currentStatus;
                try
                {
                    currentStatus = await _checkRegistrationOperationStatusSource.Task.WaitAsync(cancellationToken);
                }
                catch (OperationCanceledException e)
                {
                    throw new OperationCanceledException("Timed out waiting for DPS to send a response to the polling request", e);
                }

                Debug.Assert(currentStatus.RegistrationState != null);

                if (currentStatus.RegistrationState.Status != ProvisioningRegistrationStatus.Assigning)
                {
                    // return once a terminal state has been reached
                    return currentStatus.RegistrationState;
                }

                // The service is expected to return a value signalling how long to wait before polling again, but
                // the SDK has a default value for when the service does not send that value. Included in this default value
                // is some jitter to help stagger the requests if multiple provisioning device clients are checking their provisioning
                // state at the same time.
                TimeSpan pollingDelay = currentStatus.RetryAfter ?? RetryJitter.GenerateDelayWithJitterForRetry(s_defaultOperationPollingInterval);

                await Task.Delay(pollingDelay, cancellationToken);
            }
        }

        private MqttConnect CreateProvisioningConnectPacket(X509AuthenticationProvider authentication, string idScope, string globalDeviceEndpoint)
        {
            string hostName = globalDeviceEndpoint;

            string username = string.Format(
                CultureInfo.InvariantCulture,
                ProvisioningUsernameFormat,
                idScope,
                authentication.GetRegistrationId(),
                ProvisioningApiVersion,
                Uri.EscapeDataString(GetProvisioningUserAgentString()));

            return new MqttConnect()
            {
                HostName = hostName,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostName}",
                ClientCertificate = authentication.ClientCertificate,
                CleanSession = true, // The DPS MQTT broker does not support session persistence, so setting these clean start/clean session flags does nothing
                CleanStart = true,
                SessionExpiryInterval = 0,
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = authentication.GetRegistrationId(),
                ProtocolVersion = MqttProtocolVersion.V311
            };
        }

        private Task HandleReceivedProvisioningPublishAsync(MqttPublishReceivedEventArgs receivedEventArgs)
        {
            if (CurrentEndpoint != ConnectionEndpoint.DeviceProvisioningService)
            {
                // This client is not provisioning, so this publish came from IoT hub and is for the feature clients to handle.
                return Task.CompletedTask;
            }

            string topic = receivedEventArgs.Publish.Topic;

            TaskCompletionSource<RegistrationOperationStatus>? startProvisioningRequestStatusSource = _startProvisioningRequestStatusSource;

            if (startProvisioningRequestStatusSource == null)
            {
                // No registration request is outstanding on the current connection, so this publish belongs to a
                // provisioning flow that was abandoned when its connection ended.
                return Task.CompletedTask;
            }

            Trace.TraceInformation("Received MQTT publish from DPS on topic {0}", topic);

            if (!startProvisioningRequestStatusSource.Task.IsCompleted)
            {
                // The initial provisioning request's response topic is shaped like "$dps/registrations/res/202/?$rid=1&retry-after=3"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                startProvisioningRequestStatusSource.TrySetResult(operation);
            }
            else
            {
                TaskCompletionSource<RegistrationOperationStatus>? checkRegistrationOperationStatusSource = _checkRegistrationOperationStatusSource;

                if (checkRegistrationOperationStatusSource == null)
                {
                    // No polling request is outstanding, so there is nothing waiting on this response.
                    return Task.CompletedTask;
                }

                // All status polling requests' response topics are shaped like "$dps/registrations/res/200/?$rid=2"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                operation.RetryAfter = GetRetryAfterFromTopic(topic, s_defaultOperationPollingInterval);

                checkRegistrationOperationStatusSource.TrySetResult(operation);
            }

            return Task.CompletedTask;
        }

        private static TimeSpan? GetRetryAfterFromTopic(string topic, TimeSpan defaultPoolingInterval)
        {
            string[] topicAndQueryString = topic.Split('?');
            if (topicAndQueryString.Length > 1)
            {
                string[] queryPairs = topicAndQueryString[1].Split('&');
                for (int queryPairIndex = 0; queryPairIndex < queryPairs.Length; queryPairIndex++)
                {
                    string[] queryKeyAndValue = queryPairs[queryPairIndex].Split('=');
                    if (queryKeyAndValue.Length == 2 && queryKeyAndValue[0].Equals(RetryAfterHeader, StringComparison.OrdinalIgnoreCase))
                    {
                        if (int.TryParse(queryKeyAndValue[1], out int secondsToWait))
                        {
                            var serviceRecommendedDelay = TimeSpan.FromSeconds(secondsToWait);

                            return serviceRecommendedDelay.TotalSeconds < defaultPoolingInterval.TotalSeconds
                                ? defaultPoolingInterval
                                : serviceRecommendedDelay;
                        }
                    }
                }
            }

            return null;
        }

        private static string GetProvisioningUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Provisioning.Client";

            string version = typeof(AbstractConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{version} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        private void DetachConnectionCallbacks()
        {
            ManagedMqttConnection.ConnectingAsync -= PatchConnectPacketAsync;
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedAsync;
            ManagedMqttConnection.DisconnectedAsync -= HandleDisconnectedAsync;
            ManagedMqttConnection.ConnectionFaultedAsync -= HandleConnectionFaultedAsync;
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedProvisioningPublishAsync;
            ManagedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;

            CancelCurrentProvisioningFlow();
            CancelCurrentReprovisioning();
            Trace.TraceInformation("ConnectionClient's current endpoint is now neither Hub or DPS");
            CurrentEndpoint = ConnectionEndpoint.None;
        }

        private async Task<TResp> PerformWhileRespectingConnectionState<TResp>(Func<CancellationToken, Task<TResp>> funcToRetry, CancellationToken cancellationToken)
        {
            // Assume an open connection to start, but increment this by one if MqttClientNotConnectedException is thrown to counteract that assumption
            using ManualResetEventSlim latch = new();
            latch.Set();
            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                latch.Set();
                return Task.CompletedTask; // Not covered? It should be, right?
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            Action HandleUnrecoverableFault = () => latch.Set(); // Stop waiting for a connection that is never coming back
            UnrecoverablyFaulted += HandleUnrecoverableFault;
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

                        // A fault that this client will not recover from must not be waited out, whether it happened
                        // before this operation started or while this operation was waiting.
                        ThrowIfUnrecoverablyFaulted();

                        try
                        {
                            latch.Wait(cancellationToken); // Wait for device birth flow to finish before resuming this feature client-level traffic
                        }
                        catch (OperationCanceledException)
                        {
                            throw new OperationCanceledException("Operation canceled while waiting for reconnection to finish");
                        }

                        ThrowIfUnrecoverablyFaulted();
                    }
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
                UnrecoverablyFaulted -= HandleUnrecoverableFault;
            }
        }
    }
}
