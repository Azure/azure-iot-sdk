using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using Microsoft.Azure.Devices.Client.Retry;
using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    //TODO Does my setup already allow for user to publish via feature clients even when this client is connected to DPS during identity terminal exception handling? It does, right?

    //gen 2 client connect flow actually requires the session client to send a different looking connect packet each time due to connect nonce. Need
    // a way to inject that connect nonce from this client for both manual connects and for reocnnection cases. Probably just a "OnConnect" override callback thingy
    public class ConnectionClient : IConnectionClient
    {
        private bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        private MqttConnectionManager _managedMqttConnection;

        private readonly IRetryPolicy _connectionRetryPolicy;

        // Captured by ProvisionAndConnectAsync so that a later identity fault can re-provision without the
        // application having to call in again. Null when this device was connected directly rather than through DPS,
        // in which case re-provisioning is not an option this client can take on its own.
        private ProvisioningSettings? _provisioningSettings;
        private X509AuthenticationProvider? _provisioningAuthentication;
        private TwinPushOptions? _provisioningTwinPushOptions;

        // Set for the duration of a user-initiated connect. Faults raised in that window are surfaced to the caller
        // of ConnectAsync instead, so the background handler must not act on them as well.
        private volatile bool _isConnectInFlight;

        // 0 when no background recovery is running, 1 when one is. Guards against a burst of faults each starting
        // their own competing re-provisioning attempt.
        private int _isRecovering;

        private readonly CancellationTokenSource _backgroundRecoveryCts = new();

        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromSeconds(60); //TODO value is magic number

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        /// <summary>
        /// Raised when this client has permanently stopped maintaining its connection to IoT hub and no further
        /// automatic recovery will be attempted.
        /// </summary>
        /// <remarks>
        /// Retryable errors are retried internally and identity errors on a DPS-provisioned device are recovered by
        /// re-provisioning, so neither raises this event. It fires only once recovery is impossible or exhausted.
        /// </remarks>
        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync;

        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;

        private ConnectionContext? CurrentConnectionContext { get; set; }

        private Guid? CurrentConnectionNonce { get; set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public ConnectionClient(ConnectionClientOptions? options = null)
        {
            options ??= new ConnectionClientOptions();

            // This is the basic MQTT client that has no reconnection/retry logic
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(enableMqttLogs: options.EnableMqttLogging);

            // This is the wrapper that manages reconnection
            _managedMqttConnection = new(unmanagedMqttClient, options.ConnectionAttemptTimeout, options.ConnectionRetryPolicy);
            _managedMqttConnection.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client
            _managedMqttConnection.ConnectionFaultedAsync += HandleConnectionFaultedAsync; // recover from, or report, connection errors the layer below gave up on

            // The connection layer applies this to its own reconnect attempts. This client applies it separately to
            // re-provisioning attempts, which the connection layer cannot perform on its own behalf.
            _connectionRetryPolicy = options.ConnectionRetryPolicy;
        }

        private async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            //TODO do we need any sort of cancellation handling here if the connect call's cancellation token triggers?

            //TODO we definitely need some cancellation token to pass in to all these publishes/subscribes. Doesn't hub doc have some recommended defensive timeout here?

            var connack = args.ConnectAck;

            Debug.Assert(CurrentConnectionContext != null);
            Debug.Assert(CurrentConnectionNonce != null);
            string deviceId = CurrentConnectionContext.DeviceId;
            Guid connectNonce = CurrentConnectionNonce!.Value;

            // Only send the subscribe if it hasn't been sent in this MQTT session yet
            if (!connack.IsSessionPresent) //TODO subscribe elide logic
            {
                try
                {
                    // TODO this feels a bit optimistic since there is a chance that the session was established -> connection lost happened on the previous connection prior to this subscribe happening
                    var suback = await _managedMqttConnection.SubscribeAsync(new(string.Format("ih/{0}/dev/#", deviceId), MqttQualityOfServiceLevel.AtLeastOnce));
                    var subackFirstItem = suback.Items.FirstOrDefault();
                    
                    if (subackFirstItem == null)
                    {
                        Trace.TraceWarning("Received malformed SUBACK during birth flow. Attempting connection again...");
                        await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                        return;
                    }

                    if (subackFirstItem == null)
                    {
                        Trace.TraceWarning($"Received malformed SUBACK on devicebound SUBSCRIBE.");
                        await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                        return;
                    }

                    if (subackFirstItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
                    {
                        Trace.TraceWarning("Received SUBACK on devicebound SUBSCRIBE with unsuccessful result code: {0}.", subackFirstItem.ReasonCode);
                        await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                        return;
                    }
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("Exception thrown while subscribing to devicebound topic. Attempting connection again...", e);
                    await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                    return;
                }
            }

            // The device's last known version of the twin's reported and desired properties. Currently, there is no support for persisting this state on disk, so assume the device has never seen the twin.
            uint deviceDesiredPropertyVersion = 0;
            uint deviceReportedPropertyVersion = 0;

            // TODO user configurable
            bool pushDesired = true;
            bool pushReported = true;

            Birth birth = new()
            {
                SessionPresent = connack.IsSessionPresent,
                ReportedVersion = deviceReportedPropertyVersion, // Service allows for device to "resume" its previously known state if device boots up and has twin in durable storage somewhere. Not something we supported in v1, though AFAIK
                DesiredVersion = deviceDesiredPropertyVersion,

                PushDesired = pushDesired, // If false, users will only get desired properties via a GetTwin call. Akin to subscribing to desired properties in v1 land
                PushReported = pushReported, // If true, all current reported properties will be pushed to this device to "re-hydrate" its reported properties state. Not analogous to anything in v1 AFAIK
            };

            MqttPublish birthMessage = new MqttPublish()
            {
                Topic = string.Format("ih/{0}/srv/presence", deviceId),
                CorrelationData = connectNonce.ToByteArray(bigEndian: true),
                Payload = birth.ToByteArray(),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce, // QoS 0 because we don't care about the MQTT-level ack for this message.  The service will send a fully-fledged MQTT publish as the ack and we will listen for that below
            };

            birthMessage.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth:1")));

            TaskCompletionSource<BirthAck> birthAckReceivedTaskCompletionSource = new();
            Func<MqttPublishReceivedEventArgs, Task> HandleReceivedBirthAck = async (args) =>
            {
                // Birth ack messages are QoS 0, so no need to ack
                MqttPublish publish = args.Publish;
                if (publish.Topic.Equals(string.Format("ih/{0}/dev/presence", deviceId)))
                {
                    if (publish.UserProperties.TryGetType(out string? messageType, out int? version))
                    {
                        if (messageType.Equals("birth-ack"))
                        {
                            if (GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid))
                            {
                                if (receivedGuid.Equals(connectNonce))
                                {
                                    // The birth message flow is only complete once Hub sends a birth message ack with connection epoch equal to the latest connection epoch we have attempted
                                    birthAckReceivedTaskCompletionSource.TrySetResult(BirthAck.Parser.ParseFrom(args.Publish.Payload));
                                }
                                else
                                {
                                    Trace.TraceWarning("Received birth ack, but for an unexpected connection nonce. Expected {0}, but was {1}", connectNonce.ToString(), receivedGuid.ToString());
                                }
                            }
                            else
                            {
                                Trace.TraceWarning("Received birth ack, with a malformed connection nonce");
                            }
                        }
                    }
                }
            };

            TaskCompletionSource<TwinPush> twinPushReceivedTaskCompletionSource = new();

            _managedMqttConnection.PublishReceivedAsync += HandleReceivedBirthAck;
            try
            {
                MqttPublishAck birthMessagePuback;
                try
                {
                    birthMessagePuback = await _managedMqttConnection.PublishAsync(birthMessage);
                }
                catch (DeviceException e)
                {
                    Trace.TraceWarning("Exception thrown while publishing birth message. Attempting connection again...", e);
                    await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                    return;
                }

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    //TODO feels like unauth type errors should end retry here
                    Trace.TraceWarning($"Received unsuccessful PUBACK when publishing birth message with reason code: {birthMessagePuback.ReasonCode}. Attempting connection again...");
                    await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                }

                BirthAck birthAck;
                try
                {
                    birthAck = await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout);
                }
                catch (TimeoutException)
                {
                    // Did not receive mqtt birth ack message in timely manner (and user has not canceled this function yet)
                    Trace.TraceWarning("Timed out waiting for birth ack message. Attempting connection again...");
                    await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection });
                    return;
                }

                if (DevicePresenceFlowCompletedAsync != null)
                {
                    await DevicePresenceFlowCompletedAsync.Invoke(new());
                }
            }
            finally
            {
                // Stop listening for birth acks
                _managedMqttConnection.PublishReceivedAsync -= HandleReceivedBirthAck;
            }
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
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, TwinPushOptions? twinOptions = default, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // Remembered so that an identity fault arriving later, once no caller is waiting on a connect, can still
            // be recovered from by re-provisioning.
            _provisioningSettings = provisioningSettings;
            _provisioningAuthentication = authentication;
            _provisioningTwinPushOptions = twinOptions;

            return await ProvisionThenConnectAsync(provisioningSettings, authentication, twinOptions, cancellationToken);
        }

        /// <summary>
        /// Provision this device, then connect it to its assigned hub, re-provisioning for as long as the retry
        /// policy allows whenever the connection is refused for a reason attributable to the device's identity.
        /// </summary>
        private async Task<ConnectionContext> ProvisionThenConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, TwinPushOptions? twinOptions, CancellationToken cancellationToken)
        {
            uint reprovisioningAttempt = 0;

            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                // Remove any IoT Hub-specific handling of connect attempts when provisioning.
                _managedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
                _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;

                var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

                CurrentConnectionContext = new ConnectionContext()
                {
                    DeviceId = provisioningResult.DeviceId!,
                    IotHubHostName = provisioningResult.AssignedHub!,
                    IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                    AuthenticationProvider = authentication,
                    IsGen2Hub = true,
                };

                try
                {
                    await ConnectAsync(CurrentConnectionContext, twinOptions, cancellationToken);

                    return CurrentConnectionContext;
                }
                catch (DeviceException e) when (e.Retryability == ErrorRetryability.IdentityTerminal)
                {
                    // The credential or the hub assignment this device was provisioned with is no longer valid, so
                    // reconnecting with it can never succeed. Provisioning again is the only way forward, since it is
                    // what issues a fresh credential and re-evaluates the hub assignment. The connection layer cannot
                    // do this itself, which is why it surfaced the error rather than retrying it.
                    CurrentConnectionContext = null;

                    if (!_connectionRetryPolicy.ShouldRetry(++reprovisioningAttempt, e, out TimeSpan retryDelay))
                    {
                        Trace.TraceError("Retry policy declined further re-provisioning attempts after an identity error. {0}", e);

                        throw new DeviceException("Failed to provision and connect the device. The retry policy was exhausted while re-provisioning after an identity error. See inner exception for details.", e)
                        {
                            Retryability = ErrorRetryability.IdentityTerminal,
                            IsContained = false,
                        };
                    }

                    Trace.TraceWarning("Encountered an identity error while connecting. Re-provisioning in {0}. {1}", retryDelay, e);

                    await Task.Delay(retryDelay, cancellationToken);

                    // Provisioning reuses this same MQTT connection against the DPS endpoint, so any lingering
                    // connection to the previously assigned hub has to be torn down first. Reconnection is explicitly
                    // not desired here: the credential that reconnection would use is the one that just failed.
                    await _managedMqttConnection.DisconnectAsync(desireReconnection: false, cancellationToken: cancellationToken);
                }
            }
        }

        /// <summary>
        /// Handles a connection error that the connection layer has given up on, either by recovering from it or by
        /// reporting it to the application.
        /// </summary>
        /// <remarks>
        /// The connection layer raises this from inside its own disconnect handling, so this method must return
        /// promptly. Any recovery that needs to reconnect is therefore moved onto a background task rather than
        /// being awaited here.
        /// </remarks>
        private Task HandleConnectionFaultedAsync(MqttConnectionFaultedEventArgs args)
        {
            // A connect the application is still waiting on reports its own failure through that call, and
            // ProvisionAndConnectAsync already re-provisions inline. Acting here too would duplicate that work.
            if (_isConnectInFlight)
            {
                return Task.CompletedTask;
            }

            if (_isDisposed || _backgroundRecoveryCts.IsCancellationRequested)
            {
                return Task.CompletedTask;
            }

            bool canReprovision = args.IsIdentityFault
                && _provisioningSettings != null //TODO only a relevant check while we expose a direct ConnectAsync func and skip provisioning. Remove this later
                && _provisioningAuthentication != null;

            if (!canReprovision)
            {
                // Either the error has nothing to do with this device's identity, or the device was connected
                // directly rather than through DPS and so has no provisioning settings to fall back on.
                return NotifyConnectionFaultedAsync(args.Exception);
            }

            if (Interlocked.CompareExchange(ref _isRecovering, 1, 0) != 0)
            {
                // Recovery is already underway. A second one would race the first for the same connection.
                Trace.TraceInformation("Ignoring an identity fault because re-provisioning is already in progress.");
                return Task.CompletedTask;
            }

            Trace.TraceWarning("Connection faulted for a reason attributable to this device's identity. Re-provisioning. {0}", args.Exception);

            _ = Task.Run(async () =>
            {
                try
                {
                    await ProvisionThenConnectAsync(
                        _provisioningSettings!,
                        _provisioningAuthentication!,
                        _provisioningTwinPushOptions,
                        _backgroundRecoveryCts.Token);

                    Trace.TraceInformation("Re-provisioned and reconnected successfully after an identity fault.");
                }
                catch (OperationCanceledException) when (_backgroundRecoveryCts.IsCancellationRequested)
                {
                    // This client is shutting down, so there is no one left to report to.
                }
                catch (DeviceException e)
                {
                    await NotifyConnectionFaultedAsync(e);
                }
                catch (Exception e)
                {
                    await NotifyConnectionFaultedAsync(new DeviceException("Failed to re-provision this device after an identity error. See inner exception for details.", e)
                    {
                        Retryability = ErrorRetryability.IdentityTerminal,
                        IsContained = false,
                    });
                }
                finally
                {
                    Interlocked.Exchange(ref _isRecovering, 0);
                }
            });

            return Task.CompletedTask;
        }

        /// <summary>
        /// Report to the application that this client will not recover this connection on its own.
        /// </summary>
        private async Task NotifyConnectionFaultedAsync(DeviceException exception)
        {
            CurrentConnectionContext = null;

            Trace.TraceError("Connection permanently faulted. {0}", exception);

            Func<ConnectionFaultedEventArgs, Task>? handler = ConnectionFaultedAsync;

            if (handler == null)
            {
                return;
            }

            try
            {
                await handler.Invoke(new ConnectionFaultedEventArgs { Exception = exception });
            }
            catch (Exception e)
            {
                // This may run on an unmonitored background task, so a misbehaving application handler must not be
                // allowed to escape and go unobserved.
                Trace.TraceError("A ConnectionFaultedAsync handler threw. {0}", e);
            }
        }

        private Task<MqttConnect> ConstructConnectPatcketAsync(MqttConnect connectPacketToEdit)
        {
            CurrentConnectionNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

            string hexEncodedConnectNonce = Convert.ToHexString(CurrentConnectionNonce.Value.ToByteArray(bigEndian: true));

            // Should look something like "correlationId=4f3c2a1b9d8e47f0a1b2c3d4e5f60718&clientVersion=csharp%2F1.42.0"
            // TODO do we want to also include previous user agent details like OS, architecture, etc? Service currently discards those
            string username = $"correlationId={Uri.EscapeDataString(hexEncodedConnectNonce)}&clientVersion={Uri.EscapeDataString($"csharp/{GetPackageVersion()}")}";

            connectPacketToEdit.Username = username;

            return Task.FromResult(connectPacketToEdit);
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            await _managedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
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
            ObjectDisposedException.ThrowIf(_isDisposed, this);

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
        public async Task ConnectAsync(ConnectionContext connectionContext, TwinPushOptions? twinPushOptions = default, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // Setup mqtt client to handle connections now that they will connect to IoT Hub rather than DPS
            _managedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync; //Covers against calling connect, losing connection, then calling connect again?
            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            _managedMqttConnection.ConnectingAsync += ConstructConnectPatcketAsync;
            _managedMqttConnection.ConnectedAsync += HandleConnectedToHubAsync;

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

            // The MqttConnectionManager layer absorbs and retries every retryable connection-level error, so it only raises this
            // when it has given up for good. Without this handler, a fault that lands after the CONNACK — while the
            // birth flow is still in flight, or during a reconnection that ends terminally — would leave the wait on
            // devicePresenceFlowResult below hanging until the caller's own cancellation token fires.
            Func<MqttConnectionFaultedEventArgs, Task> HandleConnectionFaulted = (args) =>
            {
                Trace.TraceError("Connection to IoT Hub encountered an unrecoverable connection error. {1}.", args.Exception);

                devicePresenceFlowResult.TrySetResult(new DevicePresenceFlowCompletedArgs(args.Exception));
                return Task.CompletedTask;
            };

            // Setup callbacks BEFORE sending CONNECT so that CONNACK can be handled regardless of how quickly it arrives
            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            _managedMqttConnection.ConnectionFaultedAsync += HandleConnectionFaulted;
            _isConnectInFlight = true;

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

                MqttConnectAck connack = await _managedMqttConnection.ConnectAsync(connectPacket, cancellationToken);

                var devicePresenceFlowCompletedArgs = await devicePresenceFlowResult.Task.WaitAsync(cancellationToken);

                if (devicePresenceFlowCompletedArgs.Exception != null)
                {
                    throw devicePresenceFlowCompletedArgs.Exception;
                }
            }
            catch (DeviceException)
            {
                // A fatal connection-level error thrown by the connection layer itself. It has already stopped
                // maintaining the connection, so this client is no longer connected to anything.
                CurrentConnectionContext = null;
                throw;
            }
            finally
            {
                _isConnectInFlight = false;
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
                _managedMqttConnection.ConnectionFaultedAsync -= HandleConnectionFaulted;
            }
        }

        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ProvisioningConnection provisioningConnection = new();
            return await provisioningConnection.RegisterAsync(_managedMqttConnection, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO do we care about initial twin as returned by DPS?
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _backgroundRecoveryCts.Cancel();

            _managedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
            _managedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;
            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            _managedMqttConnection.ConnectionFaultedAsync -= HandleConnectionFaultedAsync;

            if (disposing)
            {
                _managedMqttConnection.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                _managedMqttConnection.Dispose();
            }

            _backgroundRecoveryCts.Dispose();

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _backgroundRecoveryCts.Cancel();

            _managedMqttConnection.ConnectingAsync -= ConstructConnectPatcketAsync;
            _managedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;
            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            _managedMqttConnection.ConnectionFaultedAsync -= HandleConnectionFaultedAsync;

            _managedMqttConnection.Dispose();

            _backgroundRecoveryCts.Dispose();

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
                return await _managedMqttConnection.PublishAsync(publish, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttSubscribeAck>> funcToRetry = async (args) =>
            {
                return await _managedMqttConnection.SubscribeAsync(subscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttUnsubscribeAck>> funcToRetry = async (args) =>
            {
                return await _managedMqttConnection.UnsubscribeAsync(unsubscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        private static string GetPackageVersion()
        {
            return typeof(ConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }
    }
}
