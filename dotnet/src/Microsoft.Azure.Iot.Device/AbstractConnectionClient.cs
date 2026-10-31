// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
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
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// A base class for all <see cref="ConnectionClient"/> classes to extend.
    /// </summary>
    /// <remarks>
    /// Broadly, this class is responsible for connection management, managing when to try connecting to IoT Hub vs falling back to provisioning the device. It is also
    /// responsible for locally queueing all feature client traffic while the device re-establishes its presence with IoT Hub.
    /// 
    /// Specific connection clients that inherit from this should only implement the specific connection needs for that client (for example, choosing CONNECT packet details, subscribing
    /// to topics that are relevant for each Hub type, etc).
    /// </remarks>
    public abstract class AbstractConnectionClient
    {
        private sealed class CancellationTokenHolder
        {
            public CancellationTokenHolder(CancellationToken token)
            {
                Token = token;
            }

            public CancellationToken Token { get; }
        }

        private sealed class ReprovisioningCallbackScope
        {
            private int _isActive = 1;

            public bool IsActive => Volatile.Read(ref _isActive) != 0;

            public void Close() => Volatile.Write(ref _isActive, 0);
        }

        private readonly AsyncEvent<MqttPublishReceivedEventArgs> _publishReceived = new();
        private readonly AsyncEvent<ConnectionFaultedEventArgs> _connectionFaulted = new();
        private readonly AsyncEvent<ReprovisioningStartedEventArgs> _reprovisioningStarted = new();
        private readonly AsyncEvent<DevicePresenceFlowCompletedArgs> _devicePresenceFlowCompleted = new();
        private readonly AsyncEvent<ProvisioningFlowCompletedArgs> _provisioningFlowCompleted = new();

        /// <remarks>
        /// Every handler is invoked and awaited in the order it was added, even if an earlier one throws.
        /// </remarks>
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync
        {
            add => _publishReceived.Add(value);
            remove => _publishReceived.Remove(value);
        }

        /// <remarks>
        /// Every handler is invoked and awaited in the order it was added, even if an earlier one throws.
        /// </remarks>
        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync
        {
            add => _connectionFaulted.Add(value);
            remove => _connectionFaulted.Remove(value);
        }

        /// <summary>
        /// Raised, for informational purposes only, when this client starts re-provisioning the device through Device
        /// Provisioning Service on its own to recover a connection.
        /// </summary>
        /// <remarks>
        /// This is raised once when the re-provisioning recovery starts, not once for each registration attempt. Handlers
        /// should return quickly because re-provisioning starts once they complete; any exception they throw is logged
        /// and ignored.
        /// </remarks>
        public event Func<ReprovisioningStartedEventArgs, Task>? ReprovisioningStartedAsync
        {
            add => _reprovisioningStarted.Add(value);
            remove => _reprovisioningStarted.Remove(value);
        }

        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync
        {
            add => _devicePresenceFlowCompleted.Add(value);
            remove => _devicePresenceFlowCompleted.Remove(value);
        }

        /// <summary>
        /// Raised once the provisioning flow that runs upon connecting to Device Provisioning Service has either
        /// produced a registration result or failed. This is the provisioning counterpart of
        /// <see cref="DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        private event Func<ProvisioningFlowCompletedArgs, Task>? ProvisioningFlowCompletedAsync
        {
            add => _provisioningFlowCompleted.Add(value);
            remove => _provisioningFlowCompleted.Remove(value);
        }

        /// <summary>
        /// Raised whenever this client's connection to IoT hub is lost or closed (including deliberately), before this
        /// client starts reconnecting. Handlers must return quickly; any exception they throw is logged and ignored.
        /// </summary>
        internal event Action? HubConnectionLost;

        private const string ProvisioningUsernameFormat = "{0}/registrations/{1}/api-version={2}&ClientVersion={3}";

        // WebSocket settings mirror the C SDK (AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH):
        // DPS and the MQTTv3 hub use "/$iothub/websocket" on port 443 with the "mqtt" subprotocol.
        internal const int WebSocketPort = 443;
        internal const string DefaultWebSocketPath = "/$iothub/websocket";

        internal static string BuildWebSocketUri(string hostName, int port, string path) => $"wss://{hostName}:{port}{path}";

        // Used for every DPS session. Required for the "connectionProfile" registration result field and supports
        // certificate signing requests.
        private const string ProvisioningApiVersion = "2026-11-02-preview";
        private const string ProvisioningSubscribeFilter = "$dps/registrations/res/#";
        private const string ProvisioningResponseTopicPrefix = "$dps/registrations/res/";
        private const string ProvisioningRegisterTopic = "$dps/registrations/PUT/iotdps-register/?$rid={0}";
        private const string ProvisioningGetOperationsTopic = "$dps/registrations/GET/iotdps-get-operationstatus/?$rid={0}&operationId={1}";
        private const string RetryAfterHeader = "Retry-After";

        private static readonly TimeSpan s_defaultOperationPollingInterval = TimeSpan.FromSeconds(2);

        // The abstract methods cover all the differences between an MQTTv5 client and a unified client.
        internal abstract MqttConnect IotHubMqttConnectOverride(MqttConnect connect);

        // In MQTTv5 case, SUB to devicebound, send birth message, wait for birth ack. In MQTTv3 case, send all DM/Twin/Telem SUBs.
        // In both cases, this method should trigger the "OnDevicePresenceFlowCompleted" callback
        public abstract Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args);

        // Workaround so that inheriting classes can invoke the "DevicePresenceFlowCompletedAsync" event.
        protected Task RaiseDevicePresenceFlowCompletedAsync(DevicePresenceFlowCompletedArgs args)
        {
            return _devicePresenceFlowCompleted.InvokeAsync(args);
        }

        /// <summary>
        /// Decide whether the device presence flow for the connection that was just established may skip its SUBSCRIBE.
        /// </summary>
        /// <remarks>
        /// Per the device presence spec's subscribe-elide rule, the SUBSCRIBE is skipped only when the broker resumed the
        /// session and this process cleanly observed a SUBACK on that session before. When it may be skipped, the
        /// subscriptions are known to still be in place, so they stay marked as such. Otherwise they remain unmarked
        /// (the write-ahead clear) until <see cref="MarkDevicePresenceSubscribed"/> records a successful SUBACK.
        /// Must be called before the presence flow first awaits, so that it reads the state of this connection's CONNECT.
        /// </remarks>
        internal bool TryElideDevicePresenceSubscribe(MqttConnectAck connack)
        {
            if (connack.IsSessionPresent && _wasSubscribedAtConnect)
            {
                Volatile.Write(ref _isDevicePresenceSubscribed, 1);
                return true;
            }

            return false;
        }

        /// <summary>
        /// Record that a SUBACK for the device presence subscriptions was observed on the current session.
        /// </summary>
        internal void MarkDevicePresenceSubscribed()
        {
            Volatile.Write(ref _isDevicePresenceSubscribed, 1);
        }

        /// <summary>
        /// Forget the device presence subscriptions, so that the next connection re-subscribes even if its session is resumed.
        /// </summary>
        internal void ClearDevicePresenceSubscribed()
        {
            Volatile.Write(ref _isDevicePresenceSubscribed, 0);
        }

        internal bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        internal MqttConnectionManager ManagedMqttConnection;
        private CancellationTokenSource _operationLifetimeCancellation = new();
        private readonly object _operationLifetimeCancellationLock = new();

        internal abstract bool DoesClientSupportHubType(ConnectionProfile connectionProfile);

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

        // The endpoint that the most recently established connection was established for, or None once its loss has been
        // handled. The connection layer reports a connection's loss before the next connection is reported as established,
        // so this identifies which endpoint a reported loss belongs to even after CurrentEndpoint has moved on.
        private volatile ConnectionEndpoint _connectedEndpoint = ConnectionEndpoint.None;

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

        /// <summary>
        /// Whether this client has selected a connection endpoint, or is automatically re-provisioning to select one.
        /// </summary>
        /// <remarks>
        /// A failed re-provisioning attempt resets <see cref="CurrentEndpoint"/> to <see cref="ConnectionEndpoint.None"/>
        /// while the retry loop waits out its backoff, but recovery is still active then, so operations must wait for it
        /// rather than fail. The re-provisioning cancellation source is checked (rather than _isReprovisioning) because
        /// an explicit disconnect clears it immediately, whereas _isReprovisioning stays set until cleanup finishes.
        /// </remarks>
        public bool IsConnectionTargetSet =>
            CurrentEndpoint != ConnectionEndpoint.None
            || Volatile.Read(ref _currentReprovisioningCancellation) != null;

        // The registration request to send on every connection to Device Provisioning Service. Only set while provisioning.
        private RegistrationRequestPayload? _provisioningRequestPayload;

        // The authentication to present on every connection to Device Provisioning Service. Only set while provisioning.
        private AuthenticationProvider? _provisioningAuthentication;

        // The inputs of the most recent provisioning run, kept for the lifetime of this client so that a connection
        // that faults on this device's identity can be recovered from by provisioning again with the same credentials.
        private ProvisioningSettings? _lastProvisioningSettings;
        private AuthenticationProvider? _lastProvisioningAuthentication;

        // Set while a re-provisioning attempt started by an identity fault is in flight. Only one such attempt may run
        // at a time because each one takes over this client's single connection.
        private int _isReprovisioning;

        // The retry policy that governs how many times, and how quickly, an automatic re-provisioning attempt is
        // repeated after it fails. Mirrors the C connection client, whose needs_reprovision intent keeps sending the
        // device back to DPS until a registration succeeds rather than giving up after a single failed attempt.
        private readonly IRetryPolicy _connectionRetryPolicy;

        // True if the application asked for a clean session on every IoT hub connection.
        private readonly bool _useCleanSession;

        // The device presence spec's process-memory "subscribed" flag: 1 once a SUBACK for the device presence
        // subscriptions has been observed on the current IoT hub session, with nothing since putting it in doubt. Never
        // persisted, so every process starts by subscribing.
        //
        // Each CONNECT attempt moves the flag into _wasSubscribedAtConnect and clears it, which implements the spec's
        // failed-CONNECT clear: if the attempt never reaches a successful CONNACK (for example because the CONNACK was
        // lost after the broker created an empty session), the next attempt sees 0 and re-subscribes even if that
        // session is then resumed. A successful CONNACK hands the snapshot to TryElideDevicePresenceSubscribe instead.
        private int _isDevicePresenceSubscribed;
        private volatile bool _wasSubscribedAtConnect;

        // Standing intent to re-provision: the cached assignment is no good -- an identity rejection at CONNACK or the
        // hub-unreachable threshold -- so this client should ask DPS for a fresh assignment rather than reconnecting to
        // the same hub. Mirrors the C client's needs_reprovision: it is the single input to the Hub-vs-DPS recovery
        // decision in HandleConnectionFaultedAsync, and it is consumed only once a re-provisioning attempt actually
        // starts, so a trigger that cannot be acted on leaves the intent standing. Volatile because the connection
        // layer's callback threads set it while caller threads read it (and vice versa), so the decision must be made
        // on the latest value rather than a stale cache.
        private volatile bool _needsReprovision;

        // Whether connections to Device Provisioning Service present the secondary boot credentials, because DPS
        // rejected the primary ones during the current provisioning run.
        private volatile bool _usesSecondaryBootCredentials;

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
        private CancellationTokenHolder? _currentReprovisioningCancellationToken;

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

        // Incremented each time the application deliberately disconnects this client. An operation that was waiting for
        // the connection to come back, or a re-provisioning attempt that was about to start, compares this against the
        // value it saw when it began so that a deliberate disconnect always takes precedence over automatic recovery.
        private int _deliberateDisconnectCount;

        // The number of deliberate disconnects currently in progress. An operation that finds the connection gone while
        // one is in progress fails fast rather than waiting for a recovery that the disconnect is stopping.
        private int _deliberateDisconnectsInProgress;

        /// <summary>
        /// Raised when the application deliberately disconnects this client, so that operations waiting for the
        /// connection to come back stop waiting for a recovery that the disconnect has abandoned.
        /// </summary>
        private event Action? DeliberatelyDisconnected;

        // Completed once the most recently started re-provisioning attempt has fully stopped, so that a deliberate
        // disconnect can wait for it rather than racing it for this client's connection.
        private Task _reprovisioningCompletion = Task.CompletedTask;

        // Set on the flow of a re-provisioning attempt, so that callbacks can identify when they may need to avoid
        // waiting for that same attempt.
        private readonly AsyncLocal<bool> _isOnReprovisioningFlow = new();
        private readonly AsyncLocal<ReprovisioningCallbackScope?> _reprovisioningCallbackScope = new();

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
        private CancellationToken _provisioningFlowParentCancellationToken;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        /// <param name="connectionContext">
        /// An optional IoT hub assignment the application restored (for example, one persisted to disk across a device
        /// reboot). When supplied, <see cref="ProvisionAndConnectAsync"/> attempts to connect directly to this IoT hub
        /// before provisioning for a new assignment.
        /// </param>
        public AbstractConnectionClient(ConnectionClientOptions? options = null, ConnectionContext? connectionContext = null)
        {
            options ??= new ConnectionClientOptions();

            // Seed any assignment the application restored so that ProvisionAndConnectAsync can try connecting directly
            // to it before falling back to provisioning for a fresh one.
            CurrentConnectionContext = connectionContext;

            _connectionRetryPolicy = options.ConnectionRetryPolicy;
            _useCleanSession = options.UseCleanSession;

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
            ManagedMqttConnection.TrySwitchCredentialsAsync = TrySwitchCredentialsAsync;
            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedProvisioningPublishAsync;
        }

        /// <summary>
        /// Connect this device to IoT hub, provisioning it through Device Provisioning Service first if it cannot
        /// connect directly to an IoT hub assignment it already holds.
        /// </summary>
        /// <remarks>
        /// <para>
        /// Unless <paramref name="forceProvisioning"/> is set, this method first tries to connect directly to the IoT
        /// hub named by the <see cref="ConnectionContext"/> this client already holds -- one returned by an earlier
        /// provisioning run in this process, or one the application restored into this client at construction. The
        /// connection layer retries that direct connection under the configured retry policy, so the attempt is only
        /// abandoned once the retry policy is exhausted or the hub rejects this device's identity with a fatal error.
        /// When there is no cached connection context, when the direct connection is abandoned, or when
        /// <paramref name="forceProvisioning"/> is set, this device provisions through Device Provisioning Service and
        /// connects to the hub it is assigned.
        /// </para>
        /// <para>
        /// If the connection to the assigned IoT hub later faults because of this device's identity, this client first
        /// retries at once with the other of the provider's primary and secondary credentials (see
        /// <see cref="AuthenticationProvider.GetSecondaryOperationalAuthenticationCredentialsAsync"/>), if it has
        /// both. Only once the hub has rejected both does this client provision again with these same credentials
        /// and reconnect to whichever hub it is assigned, without the application having to do anything. Device
        /// Provisioning Service rejecting the primary boot credentials is likewise retried with the secondary ones.
        /// </para>
        /// </remarks>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="forceProvisioning">
        /// When <c>true</c>, skip trying to connect directly to any cached IoT hub assignment and provision through
        /// Device Provisioning Service immediately.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The connection context for the IoT hub this device connected to.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, AuthenticationProvider authentication, bool forceProvisioning = false, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            StartOperationLifetime();

            Volatile.Write(ref _currentReprovisioningCancellationToken, null);

            return await ProvisionAndConnectCoreAsync(provisioningSettings, authentication, forceProvisioning, cancellationToken);
        }

        private async Task<ConnectionContext> ProvisionAndConnectCoreAsync(ProvisioningSettings provisioningSettings, AuthenticationProvider authentication, bool forceProvisioning, CancellationToken cancellationToken)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // Remember what this device was asked to provision with before attempting either path, so that a connection
            // established directly from a cached assignment -- not only one established after provisioning -- can still
            // be recovered by re-provisioning if it later faults on this device's identity.
            _lastProvisioningSettings = provisioningSettings;
            _lastProvisioningAuthentication = authentication;

            // Unless the caller insists on provisioning, try the IoT hub assignment this client already holds first. A
            // cached context comes either from a previous provisioning run in this process or from one the application
            // restored into this client at construction. The connection layer retries the direct connection under the
            // configured retry policy, so it only fails once that policy is exhausted or the hub rejects this device's
            // identity -- exactly the cases in which the cached assignment is no good and this device must ask Device
            // Provisioning Service for a fresh one.
            ConnectionContext? cachedConnectionContext = CurrentConnectionContext;
            if (!forceProvisioning && cachedConnectionContext != null)
            {
                try
                {
                    await ConnectToHubAsync(cachedConnectionContext, cancellationToken);
                    return cachedConnectionContext;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    // The caller cancelled, so this is not a failure of the cached assignment to fall back from.
                    throw;
                }
                catch (DeviceException e)
                {
                    Trace.TraceWarning("Could not connect directly to the cached IoT hub assignment; provisioning for a fresh one. {0}", e);
                }
            }

            return await ProvisionThenConnectAsync(provisioningSettings, authentication, cancellationToken);
        }

        /// <inheritdoc cref="Unified.Connection.IConnectionClient.PublishAsync(MqttPublish, CancellationToken)"/>
        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            // The device ID is resolved on every attempt rather than once up front, because this operation may wait out a
            // re-provisioning that assigns this device a different ID. The caller's publish is left untouched so that
            // its topic template is still available to later attempts. The ID is only read once the attempt has been
            // admitted for IoT hub, because switching away from IoT hub (and so re-provisioning) waits for admitted
            // operations to finish; reading it any earlier could pair this device's old ID with a newer connection.
            Func<CancellationToken, Task<MqttPublishAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.BuildAndPublishAsync(
                    ConnectionEndpoint.IotHub,
                    () => ContainsDeviceIdPlaceholder(publish.Topic)
                        ? publish.WithTopic(ResolveDeviceIdPlaceholder(publish.Topic, GetDeviceIdForTopics()))
                        : publish,
                    throwOnPubackErrorCode: false,
                    cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        /// <inheritdoc cref="Unified.Connection.IConnectionClient.SubscribeAsync(MqttSubscribe, CancellationToken)"/>
        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            // See PublishAsync for why the device ID is resolved on every attempt, and only once it has been admitted.
            Func<CancellationToken, Task<MqttSubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.BuildAndSubscribeAsync(
                    ConnectionEndpoint.IotHub,
                    () =>
                    {
                        if (!subscribe.TopicFilters.Any(topicFilter => ContainsDeviceIdPlaceholder(topicFilter.Topic)))
                        {
                            return subscribe;
                        }

                        string deviceId = GetDeviceIdForTopics();
                        return subscribe.WithTopicFilters(subscribe.TopicFilters
                            .Select(topicFilter => ContainsDeviceIdPlaceholder(topicFilter.Topic)
                                ? new MqttTopicFilter(ResolveDeviceIdPlaceholder(topicFilter.Topic, deviceId), topicFilter.QualityOfServiceLevel)
                                : topicFilter)
                            .ToList());
                    },
                    throwOnSubackErrorCode: false,
                    cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        /// <inheritdoc cref="Unified.Connection.IConnectionClient.UnsubscribeAsync(MqttUnsubscribe, CancellationToken)"/>
        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            // See PublishAsync for why the device ID is resolved on every attempt, and only once it has been admitted.
            Func<CancellationToken, Task<MqttUnsubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.BuildAndUnsubscribeAsync(
                    ConnectionEndpoint.IotHub,
                    () =>
                    {
                        if (!unsubscribe.TopicFilters.Any(ContainsDeviceIdPlaceholder))
                        {
                            return unsubscribe;
                        }

                        string deviceId = GetDeviceIdForTopics();
                        return unsubscribe.WithTopicFilters(unsubscribe.TopicFilters
                            .Select(topicFilter => ResolveDeviceIdPlaceholder(topicFilter, deviceId))
                            .ToList());
                    },
                    throwOnUnsubackErrorCode: false,
                    cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        private static bool ContainsDeviceIdPlaceholder(string topic)
        {
            return topic.Contains(Unified.Connection.IConnectionClient.DeviceIdPlaceholder, StringComparison.Ordinal);
        }

        private static string ResolveDeviceIdPlaceholder(string topic, string deviceId)
        {
            return topic.Replace(Unified.Connection.IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal);
        }

        /// <summary>
        /// The device ID to substitute into a feature client's topic for the attempt that is about to be sent.
        /// </summary>
        /// <exception cref="MqttClientNotConnectedException">
        /// This client holds no IoT hub assignment, so it cannot be present on a hub. Reported as not-connected so that
        /// the operation waits for the device to become present (or fails fast if nothing is going to make it present),
        /// exactly as it would if it had found the connection gone.
        /// </exception>
        internal virtual string GetDeviceIdForTopics()
        {
            return CurrentConnectionContext?.DeviceId
                ?? throw new MqttClientNotConnectedException("This device has no IoT hub assignment, so it has no device ID to send this operation with.");
        }

        /// <summary>
        /// Disconnect this device from IoT hub with optional disconnect settings.
        /// </summary>
        /// <remarks>
        /// <para>
        /// The cached <see cref="ConnectionContext"/> survives a disconnect. A later
        /// <see cref="ProvisionAndConnectAsync"/> reconnects directly to the same IoT hub unless forceProvisioning is
        /// set or the cached credentials are rejected.
        /// </para>
        /// <para>
        /// A deliberate disconnect stops automatic reconnection and re-provisioning. Feature operations waiting for
        /// recovery are released and throw <see cref="OperationCanceledException"/> instead of being sent after recovery.
        /// New feature operations fail fast until this client is connected again.
        /// </para>
        /// </remarks>
        /// <param name="options">The disconnect settings, or null to use the defaults.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(DisconnectOptions? options = null, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            cancellationToken.ThrowIfCancellationRequested();

            options ??= new DisconnectOptions();
            MqttDisconnect disconnect = new()
            {
                Reason = MqttDisconnectReasonCode.NormalDisconnection,
                SessionExpiryInterval = options.SessionExpiryInterval,
            };

            // Record the disconnect before cancelling anything, so that a re-provisioning attempt or a waiting operation
            // that races this call observes it and stands down.
            Interlocked.Increment(ref _deliberateDisconnectsInProgress);
            Interlocked.Increment(ref _deliberateDisconnectCount);

            try
            {
                // Cancel every feature operation tied to this connection's lifetime.
                CancelOperationLifetime();

                // Abandon any queued feature operations straight away rather than leaving them to wait for a recovery
                // that this disconnect is about to stop.
                ReleaseOperationsWaitingForConnection();

                // The application is closing this connection deliberately, so any recovery that an earlier identity fault
                // started is no longer wanted.
                CancelCurrentReprovisioning();

                // Wait for the cancelled re-provisioning attempt to stop, so that it cannot re-establish a connection
                // (or change the endpoint this client targets) after this disconnect completes. This is skipped only
                // while an awaited callback on that attempt is executing, which would otherwise wait on itself.
                if (_reprovisioningCallbackScope.Value?.IsActive != true)
                {
                    await Volatile.Read(ref _reprovisioningCompletion).WaitAsync(cancellationToken);
                }

                // This also stops the connection layer from reconnecting to the IoT hub.
                await ManagedMqttConnection.DisconnectAsync(false, disconnect, cancellationToken);

                Trace.TraceInformation("ConnectionClient's current endpoint is now neither IoT Hub or DPS");
                CurrentEndpoint = ConnectionEndpoint.None;
            }
            finally
            {
                // Release once more for any operation that started while this disconnect was in progress and began
                // waiting for a recovery that has since been stopped. Such an operation re-evaluates, finds this client
                // deliberately idle and fails fast.
                Interlocked.Decrement(ref _deliberateDisconnectsInProgress);
                ReleaseOperationsWaitingForConnection();
            }
        }

        /// <summary>
        /// Release every feature operation that is waiting for the connection to be re-established, because the
        /// application deliberately disconnected this client.
        /// </summary>
        private void ReleaseOperationsWaitingForConnection()
        {
            Action? deliberatelyDisconnected = DeliberatelyDisconnected;
            if (deliberatelyDisconnected == null)
            {
                return;
            }

            // Each waiter is released independently so that one that throws cannot keep the others waiting.
            foreach (Delegate releaseWaiter in deliberatelyDisconnected.GetInvocationList())
            {
                try
                {
                    ((Action)releaseWaiter).Invoke();
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("An internal handler for a deliberate disconnect threw and was ignored. {0}", e);
                }
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public virtual void Dispose(bool disposing)
        {
            CancelOperationLifetime();
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
            CancelOperationLifetime();
            DetachConnectionCallbacks();

            ManagedMqttConnection.Dispose();

            _isDisposed = true;
        }

        private Task DelegatePublishAsync(MqttPublishReceivedEventArgs args)
        {
            return _publishReceived.InvokeAsync(args);
        }
        /// <summary>
        /// Provision this device through Device Provisioning Service and connect it to the IoT hub it is assigned.
        /// </summary>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields.</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The connection context for the IoT hub this device was assigned and connected to.</returns>
        private async Task<ConnectionContext> ProvisionThenConnectAsync(ProvisioningSettings provisioningSettings, AuthenticationProvider authentication, CancellationToken cancellationToken)
        {
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
            ConnectionProfile connectionProfile = provisioningResult.ConnectionProfile?.ToConnectionProfile() ?? ConnectionProfile.Classic;

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
                || !DoesClientSupportHubType(connectionProfile))
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
                ConnectionProfile = connectionProfile.ToIotHubConnectionType(),
            };

            await ConnectToHubAsync(CurrentConnectionContext, cancellationToken);

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Connect this device to the IoT hub named by the given connection context and run the device presence flow,
        /// throwing if the attempt fails.
        /// </summary>
        /// <remarks>
        /// The connection layer retries a retryable failure under the configured retry policy, so this call only
        /// throws once the retry policy is exhausted or the failure is terminal (including an identity-terminal
        /// rejection). This method does not provision the device.
        /// </remarks>
        /// <param name="connectionContext">The hub and identity to connect as.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        internal async Task ConnectToHubAsync(ConnectionContext connectionContext, CancellationToken cancellationToken)
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
            // targets an IoT hub. This also waits for any provisioning operation still in flight to finish, so that
            // none of them can be sent over the hub connection established below.
            await ManagedMqttConnection.SwitchEndpointAsync(ConnectionEndpoint.IotHub, cancellationToken);

            // This client is establishing a connection again, so any earlier fault no longer describes its state.
            ClearUnrecoverableFault();

            // Every connection to a hub starts again with the primary credentials, mirroring the C client's open().
            connectionContext.UsesSecondaryCredentials = false;
            CurrentConnectionContext = connectionContext;

            // Any session left on this IoT hub was not set up by this connection's presence flow, so its subscriptions are unknown.
            ClearDevicePresenceSubscribed();

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
                    WebsocketPort = WebSocketPort,
                    ClientId = deviceId,

                    // Overwritten on each connect attempt by PatchConnectPacketAsync when session resumption is allowed.
                    CleanSession = true,
                    CleanStart = true,
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

        /// <summary>
        /// Patch the CONNECT packet that is about to be sent, including on every reconnect attempt.
        /// </summary>
        private async Task<MqttConnect> PatchConnectPacketAsync(MqttConnect connectPacketToEdit, CancellationToken cancellationToken)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // The connection layer replays the same CONNECT on every retry, so fetch the boot credentials again for
                // each attempt. Otherwise a SAS token signed for the first attempt would be presented until it expired
                // and then on every attempt after that.
                AuthenticationProvider? provisioningAuthentication = _provisioningAuthentication;
                if (provisioningAuthentication != null)
                {
                    AuthenticationCredentials? bootCredentials = null;
                    if (_usesSecondaryBootCredentials)
                    {
                        bootCredentials = await provisioningAuthentication.GetSecondaryBootAuthenticationCredentialsAsync(cancellationToken).ConfigureAwait(false);
                        if (bootCredentials == null)
                        {
                            _usesSecondaryBootCredentials = false;
                        }
                    }

                    bootCredentials ??= await provisioningAuthentication.GetPrimaryBootAuthenticationCredentialsAsync(cancellationToken).ConfigureAwait(false);
                    connectPacketToEdit = ApplyAuthentication(bootCredentials, connectPacketToEdit);
                }

                return connectPacketToEdit;
            }

            // Authenticate with the credentials of the current hub connection rather than the provisioning inputs: a
            // device connected directly to a hub has no provisioning inputs, and a device provisioned with a CSR must
            // connect with the operational certificate DPS issued rather than its boot credentials. These are the
            // secondary credentials if the hub rejected the primary ones.
            ConnectionContext connectionContext = CurrentConnectionContext!;
            AuthenticationCredentials hubCredentials = await connectionContext.GetOperationalAuthenticationCredentialsAsync(
                cancellationToken,
                updateSecondaryCredentialSelection: true).ConfigureAwait(false);
            connectPacketToEdit = ApplyAuthentication(hubCredentials, connectPacketToEdit);

            // Always ask to resume the session unless the application opted out: correctness never depends on it, but it
            // preserves the broker's QoS 1 redelivery across transient drops. Whether the resumed session still holds the
            // device presence subscriptions is decided separately, from the subscribed flag.
            _wasSubscribedAtConnect = Interlocked.Exchange(ref _isDevicePresenceSubscribed, 0) == 1;
            connectPacketToEdit.CleanSession = _useCleanSession;
            connectPacketToEdit.CleanStart = _useCleanSession;

            // Gen 1 hub clients and gen 2 hub clients can insert their specific username/password/protocol version/etc before each connect attempt
            return IotHubMqttConnectOverride(connectPacketToEdit);
        }

        /// <summary>
        /// Switch between the primary and secondary credentials for the current endpoint after it rejected the current
        /// ones as an identity failure. The connection layer calls this before giving up on the connection, so that the
        /// device only re-provisions once both sets of credentials have been rejected.
        /// </summary>
        /// <returns>False if there are no secondary credentials to switch between.</returns>
        private async Task<bool> TrySwitchCredentialsAsync(CancellationToken cancellationToken)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                AuthenticationProvider? provisioningAuthentication = _provisioningAuthentication;
                if (_usesSecondaryBootCredentials)
                {
                    _usesSecondaryBootCredentials = false;
                    return true;
                }

                if (provisioningAuthentication == null
                    || await provisioningAuthentication.GetSecondaryBootAuthenticationCredentialsAsync(cancellationToken).ConfigureAwait(false) == null)
                {
                    return false;
                }

                _usesSecondaryBootCredentials = true;
                return true;
            }

            ConnectionContext? connectionContext = CurrentConnectionContext;
            return connectionContext != null
                && await connectionContext.TrySwitchCredentialsAsync(cancellationToken).ConfigureAwait(false);
        }

        // Maps the protocol-agnostic authentication material onto an MQTT CONNECT packet
        private static MqttConnect ApplyAuthentication(AuthenticationCredentials credentials, MqttConnect mqttConnect)
        {
            mqttConnect.Password = credentials.Password;
            mqttConnect.ClientCertificate = credentials.ClientCertificate;
            mqttConnect.RemoteCertificateValidationCallback = credentials.RemoteCertificateValidationCallback;
            mqttConnect.LocalCertificateSelectionCallback = credentials.LocalCertificateSelectionCallback;
            return mqttConnect;
        }

        /// <summary>
        /// Run the flow that the endpoint of the newly established connection expects.
        /// </summary>
        private async Task HandleConnectedAsync(MqttClientConnectedEventArgs args)
        {
            ConnectionEndpoint endpoint = CurrentEndpoint;
            _connectedEndpoint = endpoint;

            switch (endpoint)
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
            // A connection whose connected notification never ran (because it was lost first) was established for the
            // current endpoint.
            ConnectionEndpoint lostEndpoint = _connectedEndpoint;
            _connectedEndpoint = ConnectionEndpoint.None;
            if (lostEndpoint == ConnectionEndpoint.None)
            {
                lostEndpoint = CurrentEndpoint;
            }

            if (lostEndpoint == ConnectionEndpoint.DeviceProvisioningService
                && CurrentEndpoint != ConnectionEndpoint.DeviceProvisioningService)
            {
                // The underlying client may report the loss of the Device Provisioning Service connection after this
                // client has already moved on to connecting to the IoT hub. That loss says nothing about the IoT hub
                // connection, so it must not abandon that connection's work.
                Trace.TraceInformation("Ignoring the loss of a Device Provisioning Service connection that this client already moved on from.");
                return Task.CompletedTask;
            }

            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // Because DPS cannot persist sessions, any connection loss should be treated as a session loss. Abandon
                // the in-progress flow so that it can be restarted from the beginning once the connection is re-established.
                CancelCurrentProvisioningFlow();
            }
            else
            {
                RaiseHubConnectionLost();
            }

            return Task.CompletedTask;
        }

        private void RaiseHubConnectionLost()
        {
            Action? handlers = HubConnectionLost;
            if (handlers == null)
            {
                return;
            }

            foreach (Action handler in handlers.GetInvocationList().Cast<Action>())
            {
                try
                {
                    handler();
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("A connection lost handler threw and was ignored. {0}", e);
                }
            }
        }

        /// <summary>
        /// Raise an event that only informs the application, invoking each handler in turn so that one that throws
        /// cannot stop the others from running or affect this client's own recovery.
        /// </summary>
        private async Task RaiseInformationalEventAsync<TArgs>(AsyncEvent<TArgs> handlers, TArgs args, string eventName)
        {
            try
            {
                await handlers.InvokeAsync(args, InvokeReprovisioningCallbackAsync);
            }
            catch (Exception e)
            {
                Trace.TraceWarning("A {0} handler threw and was ignored. {1}", eventName, e);
            }
        }

        private async Task InvokeReprovisioningCallbackAsync(Func<Task> callback)
        {
            if (!_isOnReprovisioningFlow.Value)
            {
                await callback();
                return;
            }

            ReprovisioningCallbackScope? previousScope = _reprovisioningCallbackScope.Value;
            ReprovisioningCallbackScope scope = new();
            _reprovisioningCallbackScope.Value = scope;

            try
            {
                await callback();
            }
            finally
            {
                scope.Close();
                _reprovisioningCallbackScope.Value = previousScope;
            }
        }

        private async Task HandleConnectionFaultedAsync(MqttConnectionFaultedEventArgs args)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                Trace.TraceError("ConnectionClient encountered an unrecoverable error during provisioning.", args.Exception);
                // The connection layer has stopped maintaining the connection to DPS, so no further connection will
                // arrive to start the provisioning flow again.
                CancelCurrentProvisioningFlow();
                await _provisioningFlowCompleted.InvokeAsync(new ProvisioningFlowCompletedArgs(args.Exception));

                return;
            }

            if (!ManagedMqttConnection.IsConnectionLifecycleActive)
            {
                // The application deliberately disconnected this client while this fault was being delivered. The
                // disconnect takes precedence, so neither recover from the fault nor report it as unrecoverable; only
                // release a connect attempt that may still be waiting on the device presence flow.
                Trace.TraceInformation("Ignoring a connection fault because this client was deliberately disconnected. {0}", args.Exception);
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs(args.Exception));
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
                // (that loop owns the decision to retry or give up), or when the failed direct connection left a
                // standing re-provision demand that the caller can act on after the direct connect attempt fails.
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
            AuthenticationProvider? provisioningAuthentication = _lastProvisioningAuthentication;

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

            int deliberateDisconnectCount = Volatile.Read(ref _deliberateDisconnectCount);

            var reprovisioningCompletion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            Volatile.Write(ref _reprovisioningCompletion, reprovisioningCompletion.Task);

            var reprovisioningCancellation = new CancellationTokenSource();
            Interlocked.Exchange(ref _currentReprovisioningCancellation, reprovisioningCancellation);
            Volatile.Write(ref _currentReprovisioningCancellationToken, new(reprovisioningCancellation.Token));

            if (Volatile.Read(ref _deliberateDisconnectCount) != deliberateDisconnectCount
                || Volatile.Read(ref _deliberateDisconnectsInProgress) != 0
                || !ManagedMqttConnection.IsConnectionLifecycleActive)
            {
                // The application deliberately disconnected this client while this recovery was being set up, possibly
                // before that disconnect could see (and cancel) it, or the connection lifecycle already ended. The
                // disconnect takes precedence, so do not start.
                Trace.TraceInformation("Not re-provisioning because this client was deliberately disconnected.");
                Interlocked.CompareExchange(ref _currentReprovisioningCancellation, null, reprovisioningCancellation);
                Volatile.Write(ref _currentReprovisioningCancellationToken, null);
                reprovisioningCancellation.Dispose();
                Volatile.Write(ref _isReprovisioning, 0);
                reprovisioningCompletion.SetResult();
                return true;
            }

            // The decision to re-provision has been taken, so the standing demand is consumed. Mirrors the C client,
            // which clears needs_reprovision once a registration attempt is actually under way; a trigger that could
            // not be acted on above leaves the demand standing instead.
            _needsReprovision = false;

            _ = Task.Run(async () =>
            {
                _isOnReprovisioningFlow.Value = true;

                try
                {
                    Trace.TraceInformation("Re-provisioning this device because the connection demanded it. {0}", args.Exception);

                    await RaiseInformationalEventAsync(
                        _reprovisioningStarted,
                        new ReprovisioningStartedEventArgs { Exception = args.Exception },
                        nameof(ReprovisioningStartedAsync));

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
                    reprovisioningCompletion.SetResult();
                }
            });

            return true;
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
            AuthenticationProvider provisioningAuthentication,
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
                    // This loop is itself the re-provisioning recovery: the cached assignment is known to be no good, so
                    // force provisioning rather than letting ProvisionAndConnectAsync try the cached assignment again.
                    await ProvisionAndConnectCoreAsync(provisioningSettings, provisioningAuthentication, forceProvisioning: true, cancellationToken);

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
                    cancellationToken.ThrowIfCancellationRequested();

                    // Saturate rather than wrap: under the default indefinite policy this loop can run without bound, and
                    // a uint that wrapped back to 0 would make the retry policy see this as a first attempt again and
                    // reset its backoff. Pinning at the maximum keeps the policy seeing an ever-growing attempt count.
                    if (attempt < uint.MaxValue)
                    {
                        attempt++;
                    }

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
            await _connectionFaulted.InvokeAsync(new ConnectionFaultedEventArgs { Exception = fault }, InvokeReprovisioningCallbackAsync);
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

        internal CancellationToken CurrentReprovisioningCancellationToken
        {
            get => Volatile.Read(ref _currentReprovisioningCancellationToken)?.Token ?? CancellationToken.None;
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
        /// <param name="authentication">The authentication to use when connecting to Device Provisioning Service.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The terminal registration result reported by Device Provisioning Service.</returns>
        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            cancellationToken.ThrowIfCancellationRequested();
            _provisioningFlowParentCancellationToken = cancellationToken;
            int deliberateDisconnectCount = Volatile.Read(ref _deliberateDisconnectCount);

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
            // targets Device Provisioning Service. This also waits for any feature operation meant for IoT hub that is
            // still in flight to finish, so that none of them can be sent over the DPS connection established below.
            await ManagedMqttConnection.SwitchEndpointAsync(ConnectionEndpoint.DeviceProvisioningService, cancellationToken);

            // This client is establishing a connection again, so any earlier fault no longer describes its state.
            ClearUnrecoverableFault();

            // Every provisioning run starts again with the primary boot credentials.
            _usesSecondaryBootCredentials = false;
            _provisioningAuthentication = authentication;

            MqttConnect connect = CreateProvisioningConnectPacket(
                authentication,
                provisioningSettings.IdScope,
                provisioningSettings.GlobalEndpointAddress);

            TaskCompletionSource<ProvisioningFlowCompletedArgs> provisioningFlowResult = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Task HandleProvisioningFlowCompletedAsync(ProvisioningFlowCompletedArgs args)
            {
                Volatile.Read(ref provisioningFlowResult).TrySetResult(args);
                return Task.CompletedTask;
            }

            // Setup the callback BEFORE sending CONNECT so that the CONNACK can be handled regardless of how quickly it arrives
            ProvisioningFlowCompletedAsync += HandleProvisioningFlowCompletedAsync;

            try
            {
                // Set once DPS has rejected one set of boot credentials in this run, so each set is tried at most once.
                bool switchedCredentialsAfterRejectedRegistration = false;

                while (true)
                {
                    // MQTT connection manager already checks connack for non-success cases, so no need to check it here as well
                    MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connect, cancellationToken);

                    ProvisioningFlowCompletedArgs provisioningFlowCompletedArgs = await provisioningFlowResult.Task.WaitAsync(cancellationToken);

                    if (provisioningFlowCompletedArgs.Exception is DeviceException { Retryability: ErrorRetryability.IdentityTerminal }
                        && !switchedCredentialsAfterRejectedRegistration
                        && await TrySwitchCredentialsAsync(cancellationToken))
                    {
                        // DPS rejected these boot credentials when registering rather than when connecting, so connect
                        // again to present the other set.
                        Trace.TraceWarning("Device Provisioning Service rejected the device's credentials, retrying with its other credentials. {0}", provisioningFlowCompletedArgs.Exception);
                        switchedCredentialsAfterRejectedRegistration = true;
                        CancelCurrentProvisioningFlow();
                        Volatile.Write(ref provisioningFlowResult, new(TaskCreationOptions.RunContinuationsAsynchronously));
                        await DisconnectFromDpsAsync();

                        if (Volatile.Read(ref _deliberateDisconnectCount) != deliberateDisconnectCount
                            || Volatile.Read(ref _deliberateDisconnectsInProgress) != 0)
                        {
                            throw new OperationCanceledException("Provisioning was canceled because the connection client was deliberately disconnected.");
                        }

                        continue;
                    }

                    if (provisioningFlowCompletedArgs.Exception != null)
                    {
                        throw provisioningFlowCompletedArgs.Exception;
                    }

                    Debug.Assert(provisioningFlowCompletedArgs.RegistrationResult != null);

                    //TODO do we care about initial twin as returned by DPS?
                    return provisioningFlowCompletedArgs.RegistrationResult;
                }
            }
            finally
            {
                ProvisioningFlowCompletedAsync -= HandleProvisioningFlowCompletedAsync;

                // Stop any provisioning flow that is still waiting on a DPS response now that no one is listening for its result.
                CancelCurrentProvisioningFlow();
                _provisioningRequestPayload = null;
                _provisioningAuthentication = null;
                Trace.TraceInformation("ConnectionClient's current endpoint is now neither Hub or DPS");
                CurrentEndpoint = ConnectionEndpoint.None;

                // Always close the MQTT connection once provisioning has finished so that the connection can be
                // re-established against the assigned IoT hub.
                await DisconnectFromDpsAsync();
            }
        }

        private async Task DisconnectFromDpsAsync()
        {
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

            var provisioningFlowCancellation = CancellationTokenSource.CreateLinkedTokenSource(_provisioningFlowParentCancellationToken);
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

                await _provisioningFlowCompleted.InvokeAsync(new ProvisioningFlowCompletedArgs(registrationResult));
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
                await _provisioningFlowCompleted.InvokeAsync(new ProvisioningFlowCompletedArgs(e));
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
            MqttSubscribeAck subscribeResults = await ManagedMqttConnection.SubscribeAsync(ConnectionEndpoint.DeviceProvisioningService, new(ProvisioningSubscribeFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);

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
            MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(ConnectionEndpoint.DeviceProvisioningService, publish, cancellationToken);

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
                    string message = $"Device Provisioning Service did not start this registration. Response topic: '{response.Topic}'. Response body: '{response.Payload}'.";

                    if (GetStatusCodeFromTopic(response.Topic) == (int)System.Net.HttpStatusCode.Unauthorized)
                    {
                        // DPS accepts the CONNECT and only checks the credentials when registering, so this is how it
                        // rejects them. ProvisionAsync retries with the other boot credentials, if there are any.
                        throw new DeviceException(message)
                        {
                            Retryability = ErrorRetryability.IdentityTerminal,
                            IsContained = false,
                        };
                    }

                    throw new Exception(message);
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
                MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(ConnectionEndpoint.DeviceProvisioningService, message, cancellationToken);

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

        private MqttConnect CreateProvisioningConnectPacket(AuthenticationProvider authentication, string idScope, string globalDeviceEndpoint)
        {
            string hostName = globalDeviceEndpoint;

            string username = string.Format(
                CultureInfo.InvariantCulture,
                ProvisioningUsernameFormat,
                idScope,
                authentication.GetRegistrationId(),
                ProvisioningApiVersion,
                Uri.EscapeDataString(GetProvisioningUserAgentString()));

            var mqttConnect = new MqttConnect()
            {
                HostName = hostName,
                TcpPort = 8883,
                WebsocketPort = WebSocketPort,
                WebsocketUri = BuildWebSocketUri(hostName, WebSocketPort, DefaultWebSocketPath),
                CleanSession = true, // The DPS MQTT broker does not support session persistence, so setting these clean start/clean session flags does nothing
                CleanStart = true,
                SessionExpiryInterval = 0,
                Username = username,
                ClientId = authentication.GetRegistrationId(),
                ProtocolVersion = MqttProtocolVersion.V311
            };

            // The client certificate or SAS token is applied by PatchConnectPacketAsync, freshly for each connect attempt
            return mqttConnect;
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

        // Response topics are shaped like "$dps/registrations/res/{status}/?$rid={request id}".
        private static int? GetStatusCodeFromTopic(string topic)
        {
            if (!topic.StartsWith(ProvisioningResponseTopicPrefix, StringComparison.Ordinal))
            {
                return null;
            }

            string statusCode = topic[ProvisioningResponseTopicPrefix.Length..].Split('/')[0];
            return int.TryParse(statusCode, NumberStyles.None, CultureInfo.InvariantCulture, out int value) ? value : null;
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

            // Counts the connection events that release the latch, so that an event which fires after an attempt started
            // but before the latch is reset below is not lost (which would leave this operation waiting for the next one).
            int connectionEventCount = 0;
            // An operation started after the client's operation lifetime already ended (for example, after a deliberate
            // disconnect) is not tied to that lifetime. It is attempted and fails fast as not-connected instead.
            CancellationToken operationLifetimeToken = GetOperationLifetimeCancellationToken();
            using CancellationTokenSource connectionLifetimeCancellation = operationLifetimeToken.IsCancellationRequested
                ? CancellationTokenSource.CreateLinkedTokenSource(cancellationToken)
                : CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, operationLifetimeToken);

            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                Interlocked.Increment(ref connectionEventCount);
                ReleaseLatch();
                return Task.CompletedTask;
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            Action HandleUnrecoverableFault = () =>
            {
                // Stop waiting for a connection that is never coming back
                Interlocked.Increment(ref connectionEventCount);
                ReleaseLatch();
            };
            UnrecoverablyFaulted += HandleUnrecoverableFault;
            Action HandleDeliberateDisconnect = () =>
            {
                // Stop waiting for a recovery that the application's deliberate disconnect has abandoned
                Interlocked.Increment(ref connectionEventCount);
                ReleaseLatch();
            };
            DeliberatelyDisconnected += HandleDeliberateDisconnect;

            // Read only after subscribing above, so that a deliberate disconnect is either counted here or releases this
            // operation through the handler, never neither.
            int deliberateDisconnectCount = Volatile.Read(ref _deliberateDisconnectCount);

            // Throw if nothing is going to bring the connection back for this operation: either this client hit a fault
            // it will not recover from, or the application deliberately disconnected it after this operation started.
            void ThrowIfAbandoned()
            {
                ThrowIfUnrecoverablyFaulted();

                if (Volatile.Read(ref _deliberateDisconnectCount) != deliberateDisconnectCount)
                {
                    throw new OperationCanceledException("Operation canceled because the connection client was deliberately disconnected while this operation was waiting for the connection to be re-established.");
                }
            }

            try
            {
                while (true) // Retry sending publish until user cancels as long as the failure is just that the underlying mqtt client was disconnected.
                {
                    connectionLifetimeCancellation.Token.ThrowIfCancellationRequested();
                    int connectionEventCountBeforeAttempt = Volatile.Read(ref connectionEventCount);

                    try
                    {
                        // The connection layer only sends this over a connection to IoT hub. While it targets Device
                        // Provisioning Service instead (for example, while re-provisioning) it throws
                        // MqttClientNotConnectedException, so this operation waits for the device to be present on its
                        // hub again rather than being sent over a DPS connection.
                        return await funcToRetry.Invoke(cancellationToken);
                    }
                    catch (MqttClientNotConnectedException)
                    {
                        latch.Reset(); // No-op if the HandleDisconnection already reset this latch. Only here because there is a chance that this method was called while MQTT client was disconnected

                        if (Volatile.Read(ref connectionEventCount) != connectionEventCountBeforeAttempt)
                        {
                            // The device became present again (or this client faulted or was deliberately disconnected)
                            // after this attempt started, so that release must not be waited for again. Re-evaluate
                            // straight away.
                            ThrowIfAbandoned();
                            connectionLifetimeCancellation.Token.ThrowIfCancellationRequested();
                            continue;
                        }

                        // A fault that this client will not recover from, or a deliberate disconnect, must not be
                        // waited out, whether it happened before this operation started or while it was waiting.
                        ThrowIfAbandoned();

                        // If this client is deliberately idle -- it has never connected, or it was deliberately
                        // disconnected -- then no reconnection and no re-provisioning will ever arrive to release this
                        // operation, so fail fast with the not-connected error instead of waiting forever. While a
                        // connection is being (re-)established, a terminal fault is still being delivered, or a
                        // re-provision is in flight (which has its own gaps where no socket is up), a release is coming,
                        // so keep waiting in those cases. A deliberate disconnect that is still in progress is stopping
                        // all of that, so fail fast during it as well.
                        if (Volatile.Read(ref _deliberateDisconnectsInProgress) > 0
                            || (!ManagedMqttConnection.IsConnectionLifecycleActive && Volatile.Read(ref _isReprovisioning) == 0))
                        {
                            throw;
                        }

                        try
                        {
                            latch.Wait(connectionLifetimeCancellation.Token); // Wait for device birth flow to finish before resuming this feature client-level traffic
                        }
                        catch (OperationCanceledException)
                        {
                            throw new OperationCanceledException("Operation canceled while waiting for reconnection to finish");
                        }

                        ThrowIfAbandoned();
                    }
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
                UnrecoverablyFaulted -= HandleUnrecoverableFault;
                DeliberatelyDisconnected -= HandleDeliberateDisconnect;
            }
        }

        private void StartOperationLifetime()
        {
            lock (_operationLifetimeCancellationLock)
            {
                if (_operationLifetimeCancellation.IsCancellationRequested)
                {
                    _operationLifetimeCancellation = new CancellationTokenSource();
                }
            }
        }

        private CancellationToken GetOperationLifetimeCancellationToken()
        {
            lock (_operationLifetimeCancellationLock)
            {
                return _operationLifetimeCancellation.Token;
            }
        }

        private void CancelOperationLifetime()
        {
            lock (_operationLifetimeCancellationLock)
            {
                _operationLifetimeCancellation.Cancel();
            }
        }
    }
}
