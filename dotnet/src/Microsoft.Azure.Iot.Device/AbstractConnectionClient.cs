// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Retry;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;

namespace Microsoft.Azure.Iot.Device
{

    public abstract class AbstractConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync;

        public Func<IReadOnlyList<string>, Task<X509AuthenticationProvider>>? HandleCertificateSigningCompleteAsync;

        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;

        /// <summary>
        /// Raised once the provisioning flow that runs upon connecting to Device Provisioning Service has either
        /// produced a registration result or failed. This is the provisioning counterpart of
        /// <see cref="DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        private event Func<ProvisioningFlowCompletedArgs, Task>? ProvisioningFlowCompletedAsync;

        private const string ProvisioningUsernameFormat = "{0}/registrations/{1}/api-version={2}&ClientVersion={3}";

        // Used for every DPS session. Required for the "connectionProfile" registration result field and supports
        // certificate signing requests.
        private const string ProvisioningApiVersion = "2026-11-02-preview";
        private const string ProvisioningSubscribeFilter = "$dps/registrations/res/#";
        private const string ProvisioningRegisterTopic = "$dps/registrations/PUT/iotdps-register/?$rid={0}";
        private const string ProvisioningGetOperationsTopic = "$dps/registrations/GET/iotdps-get-operationstatus/?$rid={0}&operationId={1}";
        private const string RetryAfterHeader = "Retry-After";

        private static readonly TimeSpan s_defaultOperationPollingInterval = TimeSpan.FromSeconds(2);

        // The abstract methods cover all the differences between an MQTTv5 client and a unified client.
        public abstract MqttConnect MqttConnectOverride(MqttConnect connect);

        // In MQTTv5 case, SUB to devicebound, send birth message, wait for birth ack. In MQTTv3 case, send all DM/Twin/Telem SUBs.
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

        // Backing field for CurrentConnectionContext. Volatile because the connection layer's callback threads read it
        // (through GetCurrentConnectionContext and the connect/fault flows) while a caller thread publishes it, so the
        // reference must be seen fully constructed across threads.
        private volatile ConnectionContext? _currentConnectionContext;

        internal ConnectionContext? CurrentConnectionContext
        {
            get => _currentConnectionContext;
            set => _currentConnectionContext = value;
        }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        // Backing field for CurrentEndpoint. Volatile because it is written on a caller thread (as a connection is
        // established) and read on the connection layer's callback threads (which dispatch the provisioning vs. device
        // presence flow on it), so every thread must observe the latest value.
        private volatile ConnectionEndpoint _currentEndpoint = ConnectionEndpoint.None;

        /// <summary>
        /// The endpoint that this client is currently connecting to, or connected to.
        /// </summary>
        /// <remarks>
        /// This decides which flow runs when a connection is established: connecting to Device Provisioning Service
        /// starts the provisioning flow, while connecting to an IoT hub starts the device presence flow. The connection
        /// layer owns reconnection for both endpoints, so this also decides which flow a reconnection restarts.
        /// </remarks>
        internal ConnectionEndpoint CurrentEndpoint
        {
            get => _currentEndpoint;
            private set => _currentEndpoint = value;
        }

        // The registration request to send on every connection to Device Provisioning Service. Only set while provisioning.
        private RegistrationRequestPayload? _provisioningRequestPayload;

        // The inputs of the most recent provisioning run, kept for the lifetime of this client so that a connection
        // that faults on this device's identity can be recovered from by provisioning again with the same credentials.
        private ProvisioningSettings? _lastProvisioningSettings;
        private X509AuthenticationProvider? _lastProvisioningAuthentication;

        // Set while a re-provisioning attempt started by an identity fault is in flight. Only one such attempt may run
        // at a time because each one takes over this client's single connection.
        private int _isReprovisioning;

        // The retry policy that governs how many times, and how quickly, an automatic re-provisioning attempt is
        // repeated after it fails. Mirrors the C connection client, whose needs_reprovision intent keeps sending the
        // device back to DPS until a registration succeeds rather than giving up after a single failed attempt.
        private readonly IRetryPolicy _connectionRetryPolicy;

        // Standing intent to re-provision: the cached assignment is no good -- an identity rejection at CONNACK or the
        // hub-unreachable threshold -- so this client should ask DPS for a fresh assignment rather than reconnecting to
        // the same hub. Mirrors the C client's needs_reprovision: it is the single input to the Hub-vs-DPS recovery
        // decision in HandleConnectionFaultedAsync, and it is consumed only once a re-provisioning attempt actually
        // starts, so a trigger that cannot be acted on leaves the intent standing. Volatile because the connection
        // layer's callback threads set it while caller threads read it (and vice versa), so the decision must be made
        // on the latest value rather than a stale cache.
        private volatile bool _needsReprovision;

        // The Retry-After that Device Provisioning Service most recently asked for during a registration, in ticks (0
        // when it asked for none). It is captured from each provisioning response and read as a floor on the
        // re-provisioning backoff, so a registration that fails after the service asked to be left alone waits at least
        // that long before registering again. Mirrors the C client's dps_pending_retry_after_secs, whose retry-after
        // floors the reconnection policy's backoff (connection_client.c:2149-2166). Stored as a long so it can be
        // written from the DPS response handler and read from the re-provisioning loop without tearing.
        private long _lastProvisioningServiceRetryAfterTicks;

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
        private TaskCompletionSource<ProvisioningServiceResponse>? _startProvisioningRequestStatusSource;
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

            _connectionRetryPolicy = options.ConnectionRetryPolicy;

            // This is the basic MQTT client that has no reconnection/retry logic
            MqttNetClientOptions mqttNetClientOptions = new()
            {
                EnableMqttLogs = options.EnableMqttLogging,
            };
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(mqttNetClientOptions);

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

            if (provisioningSettings.CertificateSigningRequest != null && HandleCertificateSigningCompleteAsync == null)
            {
                throw new InvalidOperationException("Must set \"HandleCertificateSigningCompleteAsync\" callback before doing any certificate signing operations");
            }

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            if (provisioningResult.Status != ProvisioningRegistrationStatus.Assigned)
            {
                // Device Provisioning Service returned a terminal result other than "assigned" (for example "failed",
                // "disabled" or "unassigned"), so there is no hub assignment to connect to and AssignedHub/DeviceId are
                // null. Mirror the C connection client (dps_apply_deferred), which treats a registration that failed or
                // completed without an assignment as the most transient failure a device meets -- the enrollment may
                // not have been created yet, DPS may not have a linked IoT hub yet, or the service may simply have been
                // unavailable -- and retries it under the reconnection policy rather than giving up. Throwing a
                // retryable fault lets ReprovisionWithRetryAsync keep re-provisioning under the policy (indefinitely,
                // under the default policy), and surfaces a meaningful error to a caller that provisioned directly
                // instead of dereferencing a null assignment.
                string errorDetails = provisioningResult.ErrorCode != null || provisioningResult.ErrorMessage != null
                    ? $" (error code {provisioningResult.ErrorCode}, error message \"{provisioningResult.ErrorMessage}\")"
                    : string.Empty;

                Trace.TraceError(
                    "Device Provisioning Service returned registration status '{0}' (substatus '{1}') instead of 'assigned'.{2}",
                    provisioningResult.Status,
                    provisioningResult.Substatus,
                    errorDetails);

                throw new DeviceException(
                    $"Device Provisioning Service did not assign this device to an IoT hub: registration status was '{provisioningResult.Status}' (substatus '{provisioningResult.Substatus}'){errorDetails}.")
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = false,
                };
            }

            // An absent connection profile resolves to the documented default ("classic"); an unknown profile string
            // never reaches here because the registration response deserializer rejects values outside this enum.
            ConnectionProfile connectionProfile = provisioningResult.ConnectionProfile ?? ConnectionProfile.Classic;

            // Even a registration that reports "assigned" must actually carry the information this device needs to
            // connect: a hub hostname, a device id, and a connection profile this SDK can speak. Mirror the C
            // connection client's reject_assignment (connection_client.c:2045-2049, 2187-2246): if the assignment is
            // missing a hub hostname or device id, or names a connection profile this SDK does not understand, refuse
            // to adopt it and re-provision for a usable one instead of connecting with missing or wrong parameters (or
            // dereferencing a null assignment at connect time). The Enum.IsDefined check is defense-in-depth against a
            // reconfigured, lenient deserializer letting an unrecognized profile through. Like the non-"assigned" case
            // above, this is thrown as a retryable fault so ReprovisionWithRetryAsync keeps re-provisioning under the
            // policy (the enrollment's hub assignment may be corrected service-side) and a caller that provisioned
            // directly gets a meaningful error.
            if (string.IsNullOrEmpty(provisioningResult.AssignedHub)
                || string.IsNullOrEmpty(provisioningResult.DeviceId)
                || connectionProfile != ConnectionProfile.Classic)
            {
                Trace.TraceError(
                    "Device Provisioning Service reported an 'assigned' registration this device cannot use (assigned hub '{0}', device id '{1}', connection profile '{2}').",
                    provisioningResult.AssignedHub,
                    provisioningResult.DeviceId,
                    provisioningResult.ConnectionProfile);

                throw new DeviceException(
                    "Device Provisioning Service assigned this device to an IoT hub, but the assignment is missing a hub hostname or device id, or names a connection profile this SDK does not support, so it cannot be used.")
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = false,
                };
            }

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
                ConnectionProfile = connectionProfile,
            };

            // If CSR was a part of the provisioning request, then connect to IoT hub using the operational certificates (the ones signed by DPS) rather than the boot certificates (the ones used to authenticate with DPS).
            if (provisioningResult.IssuedClientCertificateChain != null && provisioningResult.IssuedClientCertificateChain.Count > 0)
            {
                // Service should only return signed certificates if the provisioning request included a certificate signing request, and this HandleCertificateSigningCompleteAsync callback
                // is null checked earlier if one was provided
                Debug.Assert(HandleCertificateSigningCompleteAsync != null);
                CurrentConnectionContext.AuthenticationProvider = await HandleCertificateSigningCompleteAsync(provisioningResult.IssuedClientCertificateChain);
            }
            else
            {
                // Otherwise use the same certs when connecting to IoT hub that were used to connect to DPS
                CurrentConnectionContext.AuthenticationProvider = authentication;
            }

            await ConnectToHubAsync(CurrentConnectionContext, cancellationToken);

            return CurrentConnectionContext;
        }

        public async Task ConnectAsync(ConnectionContext connectionContext, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // A single ConnectAsync keeps working until this device is connected: it tries the cached hub and, if the
            // hub rejects this device's identity or becomes unreachable past the re-provision threshold, falls back to
            // Device Provisioning Service for a fresh assignment rather than surfacing that crossover to the caller.
            // Mirrors the C connection client's open(), which likewise does not return to its caller until the device is
            // connected (or a terminal, non-recoverable error is hit).
            while (true)
            {
                // When a re-provision demand is standing -- an identity rejection at CONNACK or the hub-unreachable
                // threshold decided the cached assignment is no good -- ask Device Provisioning Service for a fresh
                // assignment rather than reconnecting to the hub that was rejected or unreachable, and keep
                // re-provisioning under the retry policy until it connects. In C this demand (needs_reprovision)
                // deliberately survives a manual close()/open() so the application cannot walk back into the stale hub by
                // reconnecting by hand; routing here does the same. The demand is consumed before routing so the
                // re-provisioning connect that follows does not re-enter this branch, matching C's "consumed once
                // provisioning is under way".
                if (_needsReprovision && CanReprovision)
                {
                    Trace.TraceInformation("A re-provision is pending, so connecting through Device Provisioning Service rather than to the cached IoT hub.");
                    _needsReprovision = false;
                    await ReprovisionUntilConnectedAsync(_lastProvisioningSettings!, _lastProvisioningAuthentication!, cancellationToken);
                    return;
                }

                try
                {
                    await ConnectToHubAsync(connectionContext, cancellationToken);
                    return;
                }
                catch (DeviceException) when (_needsReprovision && CanReprovision)
                {
                    // The connect tried the cached hub and either the hub rejected this device's identity or the
                    // hub-unreachable threshold was crossed, which left a standing demand to re-provision. Rather than
                    // surfacing that crossover to the caller, loop back to the standing-demand branch above, which
                    // re-provisions and connects to the freshly assigned hub -- so a single ConnectAsync keeps working
                    // until it is connected, mirroring the C client's open().
                    Trace.TraceInformation("The cached IoT hub assignment is no good; re-provisioning through Device Provisioning Service to recover the connection.");
                }
            }
        }

        /// <summary>
        /// Connect this device to the IoT hub named by the given connection context and run the device presence flow,
        /// throwing if the attempt fails. Unlike <see cref="ConnectAsync(ConnectionContext, CancellationToken)"/>, this
        /// does not fall back to re-provisioning on its own; it is the single hub-connect attempt that both the connect
        /// loop above and the re-provisioning retry loop are built from.
        /// </summary>
        /// <param name="connectionContext">The hub and identity to connect as.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        private async Task ConnectToHubAsync(ConnectionContext connectionContext, CancellationToken cancellationToken)
        {
            // From here on, every connection this client establishes targets IoT hub, so every connection (including the
            // ones the connection layer re-establishes on its own) runs the device presence flow.
            Trace.TraceInformation("ConnectionClient's current endpoint is now IoT Hub");
            CurrentEndpoint = ConnectionEndpoint.IotHub;

            // Tell the connection layer whether this client could actually re-provision -- that is, whether it holds the
            // inputs of a previous provisioning run. A device connected with credentials the application supplied
            // directly has no registration to renew, so retrying the hub is all it can do, and the layer must not cross
            // over to re-provisioning no matter what the retry policy advises. Mirrors the C client, which only treats
            // hub failures as a re-provision trigger when DPS is configured. The number of hub attempts before
            // re-provisioning is advised lives in the retry policy, not here.
            ManagedMqttConnection.CanReprovision = CanReprovision;

            // Tell the connection layer (and, through it, the retry policy) that every connection it now maintains
            // targets an IoT hub.
            ManagedMqttConnection.ConnectionEndpoint = ConnectionEndpoint.IotHub;

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
                    RemoteCertificateValidationCallback = CurrentConnectionContext.AuthenticationProvider.RemoteCertificateValidationCallback,
                    LocalCertificateSelectionCallback = CurrentConnectionContext.AuthenticationProvider.LocalCertificateSelectionCallback,
                    ClientId = deviceId,

                    // It can save some SUBSCRIBE calls to attempt to resume sessions, but there is a race condition
                    // wherein a device attempts to reconnect w/ clean session=false, server sends back CONNACK w/ "session not resumed"
                    // and starts a new session with no subscriptions, but that CONNACK is lost. The device will likely reconnect then.
                    // If the device connects with cleanSession=false at that time, the broker may send back CONNACK with "session resumed", but
                    // it isn't the same session that the device initially wanted to resume, and so subscriptions are unknowingly lost.
                    //
                    // Because of the above, it is simpler to just start clean sessions each time.
                    CleanSession = true,
                };

                MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connectPacket, cancellationToken);

                var devicePresenceFlowCompletedArgs = await devicePresenceFlowResult.Task.WaitAsync(cancellationToken);

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

            await ManagedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
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

            if (args.IsIdentityFault || args.ReprovisionRequired)
            {
                // Mirror the C connection client's needs_reprovision: an identity rejection at CONNACK or the
                // hub-unreachable threshold both mean the cached assignment is no good and this device should ask DPS
                // for a fresh one. This flag is the single input to the Hub-vs-DPS recovery decision below, and it
                // persists until a re-provisioning attempt actually starts.
                _needsReprovision = true;
            }

            if (_needsReprovision && TryStartReprovisioning(args))
            {
                // This client is recovering from the fault on its own, so anything waiting for the connection should
                // keep waiting for that recovery to re-establish it.
                return;
            }

            // Stop anything that is waiting on this connection: a fault during a connect attempt is thrown back to the
            // caller that requested it, while a fault after CONNACK is surfaced through this completion.
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs(args.Exception));

            if (args.LastDisconnect == null && (Volatile.Read(ref _isReprovisioning) != 0 || (_needsReprovision && CanReprovision)))
            {
                // This fault ended a connect attempt that a caller is awaiting (LastDisconnect is null), so that caller
                // is already being told about it -- through the exception thrown back to it -- and owns what happens
                // next. Declaring this client unrecoverably faulted on top of that would raise a spurious application
                // fault for a connection that is still being actively driven. That is the case either when a
                // re-provisioning attempt is already in flight and this fault ended one of its own connect attempts
                // (that loop owns the decision to retry or give up), or when the fault left a standing re-provision
                // demand this client can act on: a single ConnectAsync catches that crossover and self-heals by
                // re-provisioning, so the application must not be told the connection is gone for good. Mirrors the C
                // connection client's open(), which recovers across this crossover without surfacing an
                // application-visible fault.
                return;
            }

            // Nothing is going to bring this connection back, so let the application know it must connect again itself.
            await MarkUnrecoverablyFaultedAsync(args.Exception);
        }

        /// <summary>
        /// Whether this client holds the inputs of a previous provisioning run and can therefore re-provision on its
        /// own. A device connected with credentials the application supplied directly cannot, since there is no
        /// registration for this client to renew.
        /// </summary>
        private bool CanReprovision => _lastProvisioningSettings != null && _lastProvisioningAuthentication != null;

        /// <summary>
        /// Start provisioning this device again in response to a standing re-provision demand (an identity fault or the
        /// hub-unreachable threshold), and connect to the hub it gets assigned.
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
                Trace.TraceWarning("Not re-provisioning because the fault is reported to the caller that requested the connection.");
                return false;
            }

            ProvisioningSettings? provisioningSettings = _lastProvisioningSettings;
            X509AuthenticationProvider? provisioningAuthentication = _lastProvisioningAuthentication;

            if (provisioningSettings == null || provisioningAuthentication == null)
            {
                // This device was connected with credentials that the application supplied directly, so there is no
                // registration for this client to renew. Only the application can recover from here.
                Trace.TraceError("The connection demanded re-provisioning, but this device was not provisioned through Device Provisioning Service so it cannot re-provision. {0}", args.Exception);
                return false;
            }

            if (Interlocked.CompareExchange(ref _isReprovisioning, 1, 0) != 0)
            {
                // An earlier fault already started this recovery, and a second one would fight it over this client's connection.
                Trace.TraceInformation("Ignoring a re-provision demand because this device is already re-provisioning.");
                return true;
            }

            // The decision to re-provision has been taken, so the standing demand is consumed. Mirrors the C client,
            // which clears needs_reprovision once a registration attempt is actually under way; a trigger that could
            // not be acted on above leaves the demand standing instead.
            _needsReprovision = false;

            var reprovisioningCancellation = new CancellationTokenSource();
            _currentReprovisioningCancellation = reprovisioningCancellation;

            _ = Task.Run(async () =>
            {
                try
                {
                    Trace.TraceInformation("Re-provisioning this device because the connection demanded it. {0}", args.Exception);

                    await ReprovisionWithRetryAsync(provisioningSettings, provisioningAuthentication, reprovisioningCancellation.Token);
                }
                catch (OperationCanceledException)
                {
                    Trace.TraceWarning("Re-provisioning was abandoned because this client was disconnected or disposed.");
                }
                catch (Exception e)
                {
                    // This task is unmonitored, so nothing may escape it. The retry loop already reports an exhausted
                    // retry policy as an unrecoverable fault, so reaching here means something unexpected ended it.
                    Trace.TraceError("An unexpected error ended the re-provisioning of this device. {0}", e);
                    await MarkUnrecoverablyFaultedAsync(AsUnrecoverableFault(e));
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
        /// Drive <see cref="ReprovisionWithRetryAsync"/> inline (awaited by the caller) to re-provision this device and
        /// connect it to the hub it is assigned, keeping at it under the retry policy until it connects.
        /// </summary>
        /// <remarks>
        /// This takes ownership of re-provisioning for the duration of the loop -- the same ownership the background
        /// re-provisioning path claims in <see cref="TryStartReprovisioning"/> -- so that a connect attempt that fails
        /// on its way to the next retry is thrown back to that loop (which owns the decision to retry or give up) rather
        /// than being surfaced to the application as an unrecoverable fault while recovery is still in progress.
        /// </remarks>
        /// <param name="provisioningSettings">The settings of the provisioning run to repeat.</param>
        /// <param name="provisioningAuthentication">The authentication of the provisioning run to repeat.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        private async Task ReprovisionUntilConnectedAsync(
            ProvisioningSettings provisioningSettings,
            X509AuthenticationProvider provisioningAuthentication,
            CancellationToken cancellationToken)
        {
            bool ownedReprovisioning = Interlocked.CompareExchange(ref _isReprovisioning, 1, 0) == 0;

            try
            {
                // This client is recovering the connection on its own, so forget any fault an earlier attempt recorded.
                ClearUnrecoverableFault();

                await ReprovisionWithRetryAsync(provisioningSettings, provisioningAuthentication, cancellationToken);
            }
            finally
            {
                if (ownedReprovisioning)
                {
                    Volatile.Write(ref _isReprovisioning, 0);
                }
            }

            // ReprovisionWithRetryAsync returns normally both when it connected and when it gave up after the retry
            // policy was exhausted (which it never is under the default indefinite policy). If it gave up, it marked this
            // client unrecoverably faulted; surface that to the caller rather than returning as though connected.
            ThrowIfUnrecoverablyFaulted();
        }

        /// <summary>
        /// Provision this device again and connect it to the hub it is assigned, repeating the attempt for as long as
        /// the configured retry policy allows after each failure.
        /// </summary>
        /// <remarks>
        /// Mirrors the C connection client, whose needs_reprovision intent keeps sending the device back to Device
        /// Provisioning Service until a registration finally succeeds, rather than giving up after a failed attempt. A
        /// hub that was only transiently unreachable, a DPS enrollment that is briefly absent (which fails the device's
        /// TLS handshake or registration), or an assignment that is not yet ready to authorize the device are all
        /// recovered from once they resolve. Because that standing demand persists, this loop retries every failure --
        /// including ones the connection layer classifies as terminal, since re-provisioning is itself the recovery and
        /// a terminal result at one point in time (a missing enrollment, say) can become valid once it is restored. The
        /// loop only stops when provisioning succeeds, when this client is disconnected or disposed (which cancels it),
        /// or when the retry policy is exhausted (which it never is under the default indefinite policy).
        /// </remarks>
        /// <param name="provisioningSettings">The settings of the provisioning run to repeat.</param>
        /// <param name="provisioningAuthentication">The authentication of the provisioning run to repeat.</param>
        /// <param name="cancellationToken">Cancels the whole retry loop when this client is disconnected or disposed.</param>
        private async Task ReprovisionWithRetryAsync(
            ProvisioningSettings provisioningSettings,
            X509AuthenticationProvider provisioningAuthentication,
            CancellationToken cancellationToken)
        {
            uint attempt = 0;

            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                // Start this attempt with no standing service guidance so that only a Retry-After the service sends
                // during this attempt can floor the backoff after it fails.
                ResetProvisioningServiceRetryAfter();

                try
                {
                    await ProvisionAndConnectAsync(provisioningSettings, provisioningAuthentication, cancellationToken);

                    Trace.TraceInformation("Finished re-provisioning this device and connected it to the IoT hub it was assigned.");
                    return;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    // This client was disconnected or disposed, so this recovery is abandoned rather than retried.
                    throw;
                }
                catch (Exception e)
                {
                    attempt++;

                    // This loop is itself the re-provisioning recovery, so it always consults the policy for a Device
                    // Provisioning Service endpoint. Only AbandonRetry stops it; both Retry and
                    // AbandonHubRetryAndReprovision (which is treated the same as Retry for a DPS endpoint) continue it.
                    RetryGuidance guidance = _connectionRetryPolicy.GetRetryGuidance(attempt, e, ConnectionEndpoint.DeviceProvisioningService, out TimeSpan retryDelay);

                    if (guidance == RetryGuidance.AbandonRetry)
                    {
                        // The retry policy asked this device to stop retrying, so this recovery has run out of options.
                        // This was the only thing left that could have re-established the connection, so anything waiting
                        // for it is waiting for something that will never happen.
                        Trace.TraceError("Giving up on re-provisioning this device after {0} attempt(s) because the retry policy asked to abandon retrying. {1}", attempt, e);
                        await MarkUnrecoverablyFaultedAsync(AsUnrecoverableFault(e));
                        return;
                    }

                    // The service's Retry-After floors the policy's backoff: when Device Provisioning Service asked to
                    // be left alone for longer than the policy would wait on its own, honor the service. Mirrors the C
                    // client (connection_client.c:2149-2166), where the policy's maximum delay deliberately does NOT cap
                    // this -- it bounds how long the SDK waits of its own accord, not how long the service asked to be
                    // left alone -- so the two combine as a floor rather than either one alone deciding.
                    TimeSpan serviceRetryAfter = GetProvisioningServiceRetryAfter();
                    if (serviceRetryAfter > retryDelay)
                    {
                        Trace.TraceWarning("Device Provisioning Service asked for a Retry-After of {0}; honoring it over the reconnection policy's backoff of {1}.", serviceRetryAfter, retryDelay);
                        retryDelay = serviceRetryAfter;
                    }

                    Trace.TraceWarning("Re-provisioning attempt {0} failed; retrying in {1}. {2}", attempt, retryDelay, e);

                    if (retryDelay > TimeSpan.Zero)
                    {
                        await Task.Delay(retryDelay, cancellationToken);
                    }
                }
            }
        }

        /// <summary>
        /// Record that this client has stopped maintaining its connection for a reason that neither the connection
        /// layer nor this client will recover from, release everything that is waiting for the connection, and let
        /// the application know that it must connect again itself if it wants to keep using this client.
        /// </summary>
        private async Task MarkUnrecoverablyFaultedAsync(DeviceException fault)
        {
            Trace.TraceError("ConnectionClient encountered an unrecoverable exception", fault);
            _unrecoverableFault = fault;

            // Release everything inside this client that is waiting for the connection (for example feature operations
            // parked in PerformWhileRespectingConnectionState) so they stop waiting for a connection that is never
            // coming back. Each waiter is released independently: one that throws must not stop the others from being
            // released, nor prevent the application notification below. Without this isolation a single misbehaving
            // waiter would suppress the application's connection-faulted callback entirely, leaving it unaware that the
            // connection is gone for good.
            Action? unrecoverablyFaulted = UnrecoverablyFaulted;
            if (unrecoverablyFaulted != null)
            {
                foreach (Delegate releaseWaiter in unrecoverablyFaulted.GetInvocationList())
                {
                    try
                    {
                        ((Action)releaseWaiter).Invoke();
                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("An internal handler for the unrecoverable fault threw and was ignored. {0}", e);
                    }
                }
            }

            // Always let the application know, even if one of the internal waiters above threw, so that it can connect
            // again itself if it wants to keep using this client.
            if (ConnectionFaultedAsync != null)
            {
                await ConnectionFaultedAsync.Invoke(new ConnectionFaultedEventArgs { Exception = fault });
            }
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
                ClientCertificateSigningRequest = provisioningSettings.CertificateSigningRequest?.Base64CertificateSigningRequest,
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

            // A connection to DPS never crosses over to re-provisioning on the retry policy's hub-unreachable guidance;
            // that only applies to hub connections. Clear the flag so a DPS reconnection just retries DPS under the usual
            // policy.
            ManagedMqttConnection.CanReprovision = false;

            // Tell the connection layer (and, through it, the retry policy) that every connection it now maintains
            // targets Device Provisioning Service.
            ManagedMqttConnection.ConnectionEndpoint = ConnectionEndpoint.DeviceProvisioningService;

            // This client is establishing a connection again, so any earlier fault no longer describes its state.
            ClearUnrecoverableFault();

            MqttConnect connect = CreateProvisioningConnectPacket(
                authentication,
                provisioningSettings.IdScope,
                provisioningSettings.GlobalEndpointAddress);

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
                var disconnect = new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection };

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
                throw new Exception($"DPS did not grant QoS 1 for the mandatory response-topic subscription; received SUBACK reason '{subscribeResults.Items.FirstOrDefault()!.ReasonCode}'.");
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

            _startProvisioningRequestStatusSource = new TaskCompletionSource<ProvisioningServiceResponse>(TaskCreationOptions.RunContinuationsAsynchronously);

            Trace.TraceInformation("Publishing to DPS on topic {0}", registrationTopic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(publish, cancellationToken);

            Trace.TraceInformation("Successfully published registration request to DPS with request Id {0}", _provisioningRequestId);

            try
            {
                ProvisioningServiceResponse response = await _startProvisioningRequestStatusSource.Task.WaitAsync(cancellationToken);

                RegistrationOperationStatus? operation = response.Operation;

                if (operation == null || operation.Status != ProvisioningRegistrationStatus.Assigning)
                {
                    // Anything else is the service refusing to start this registration, and its response says why:
                    // the status code lives in the response topic and the reason in the body. Reporting both is the
                    // only way the caller can tell, say, a malformed request from an enrollment that does not exist.
                    throw new Exception(
                        $"Device Provisioning Service did not start this registration. Response topic: '{response.Topic}'. Response body: '{response.Payload}'.");
                }

                return operation;
            }
            catch (OperationCanceledException e)
            {
                throw new OperationCanceledException("Timed out waiting for DPS to send the initial provisioning response", e);
            }
        }

        /// <summary>
        /// A response that Device Provisioning Service sent to a registration request, kept with the topic it arrived
        /// on and the body as it was received so that a response that is not a registration operation (an error, for
        /// instance) can still be reported in full.
        /// </summary>
        private sealed record ProvisioningServiceResponse(string Topic, string Payload, RegistrationOperationStatus? Operation);

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
                // state at the same time. The service's value is never used to poll faster than that default cadence.
                TimeSpan pollingDelay = currentStatus.RetryAfter is { } serviceRetryAfter
                    ? (serviceRetryAfter < s_defaultOperationPollingInterval ? s_defaultOperationPollingInterval : serviceRetryAfter)
                    : RetryJitter.GenerateDelayWithJitterForRetry(s_defaultOperationPollingInterval);

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
                WebsocketUri = $"wss://{hostName}:443",
                ClientCertificate = authentication.ClientCertificate,
                RemoteCertificateValidationCallback = authentication.RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = authentication.LocalCertificateSelectionCallback,
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

            TaskCompletionSource<ProvisioningServiceResponse>? startProvisioningRequestStatusSource = _startProvisioningRequestStatusSource;

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
                RegistrationOperationStatus? operation = null;
                try
                {
                    operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options);
                }
                catch (JsonException)
                {
                    // An error response does not have to be shaped like a registration operation, and its body is
                    // reported as-is by the caller of this flow rather than hidden behind a deserialization failure.
                }

                // The service may ask to be left alone for a while on this response -- including on an error response
                // that refuses to start the registration -- so remember it as a floor on the re-provisioning backoff.
                CaptureProvisioningServiceRetryAfter(GetRetryAfterFromTopic(topic));

                startProvisioningRequestStatusSource.TrySetResult(new ProvisioningServiceResponse(topic, jsonString, operation));
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
                operation.RetryAfter = GetRetryAfterFromTopic(topic);

                // Remember what the service asked for on this response so that, if this is the response that ends the
                // registration without an assignment, the re-provisioning backoff waits at least this long.
                CaptureProvisioningServiceRetryAfter(operation.RetryAfter);

                checkRegistrationOperationStatusSource.TrySetResult(operation);
            }

            return Task.CompletedTask;
        }

        /// <summary>
        /// Remember the Retry-After the service asked for on a provisioning response, so that a re-provisioning attempt
        /// that then fails waits at least this long before registering again. Mirrors the C client, where the service's
        /// retry-after floors the reconnection policy's backoff (connection_client.c:2149-2166). A response that carries
        /// no Retry-After clears the stored value, so the response that ends the registration is the one that counts.
        /// </summary>
        private void CaptureProvisioningServiceRetryAfter(TimeSpan? retryAfter)
        {
            Volatile.Write(
                ref _lastProvisioningServiceRetryAfterTicks,
                retryAfter is { } value && value > TimeSpan.Zero ? value.Ticks : 0);
        }

        /// <summary>
        /// Forget any Retry-After the service asked for, so that a fresh re-provisioning attempt starts with no standing
        /// service guidance until the service sends some during that attempt.
        /// </summary>
        private void ResetProvisioningServiceRetryAfter()
        {
            Volatile.Write(ref _lastProvisioningServiceRetryAfterTicks, 0);
        }

        /// <summary>
        /// The Retry-After the service most recently asked for during a registration, or <see cref="TimeSpan.Zero"/>
        /// when it asked for none.
        /// </summary>
        private TimeSpan GetProvisioningServiceRetryAfter()
        {
            long ticks = Volatile.Read(ref _lastProvisioningServiceRetryAfterTicks);
            return ticks > 0 ? TimeSpan.FromTicks(ticks) : TimeSpan.Zero;
        }

        private static TimeSpan? GetRetryAfterFromTopic(string topic)
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
                            return TimeSpan.FromSeconds(secondsToWait);
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

            // Release the latch, tolerating this operation having already completed and disposed it in a race with the
            // connection event that is releasing it. The connection event (a device presence flow completing, or an
            // unrecoverable fault) fires on the connection layer's threads and may capture this subscriber an instant
            // before the finally below removes it, so a disposed latch here is expected and simply means there is
            // nothing left to release. Swallowing it keeps that race from propagating into -- and aborting -- the fault
            // notification path that raises it.
            void ReleaseLatch()
            {
                try
                {
                    latch.Set();
                }
                catch (ObjectDisposedException)
                {
                }
            }

            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                ReleaseLatch();
                return Task.CompletedTask;
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            Action HandleUnrecoverableFault = ReleaseLatch; // Stop waiting for a connection that is never coming back
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

                        // If this client is deliberately idle -- it has never connected, or it was deliberately
                        // disconnected -- then no reconnection and no re-provisioning will ever arrive to release this
                        // operation, so fail fast with the not-connected error instead of waiting forever. While a
                        // connection is being (re-)established, a terminal fault is still being delivered, or a
                        // re-provision is in flight (which has its own gaps where no socket is up), a release is coming,
                        // so keep waiting in those cases.
                        if (!ManagedMqttConnection.IsConnectionLifecycleActive && Volatile.Read(ref _isReprovisioning) == 0)
                        {
                            throw;
                        }

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
