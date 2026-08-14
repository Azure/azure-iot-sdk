using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class ConnectionClient : IConnectionClient
    {
        private bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        private IMqttClient _mqttClient;


        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const bool UseSubscribeElide = false; // Maybe user-configurable? It is a very small optimization that is probably more risk than it is worth for .NET users compared to C users
        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromSeconds(60); //TODO value is magic number

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        private ConnectionContext? CurrentConnectionContext { get; set; }

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

            MqttSessionClientOptions sessionClientOptions = new()
            {
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                EnableMqttLogging = options.EnableMqttLogging,
            };

            _mqttClient = options.MqttClient ?? new MqttSessionClient(sessionClientOptions);
            _mqttClient.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client
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

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            //TODO several mqtt client options should not be provided by the user (ie, host name). Add checks here that validate all of them

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
                IsGen2Hub = true,
            };

            await ConnectAsync(CurrentConnectionContext, twinOptions, cancellationToken);

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

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

            IotHubConnection iotHubConnection = new();

            CurrentConnectionContext = connectionContext;

            bool subscribed = false;

            string deviceId = CurrentConnectionContext.DeviceId;
            string hostname = CurrentConnectionContext.IotHubHostName;

            while (true) // Loop until presence is established successfully (return called) or user signals cancellation
            {
                cancellationToken.ThrowIfCancellationRequested();

                Trace.TraceInformation("Attempting to establish connection and presence for device {0} with IoT Hub {1}", deviceId, hostname);

                Guid connectNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

                string clientId = deviceId;

                string hexEncodedConnectNonce = Convert.ToHexString(connectNonce.ToByteArray(bigEndian: true));

                // Should look something like "correlationId=4f3c2a1b9d8e47f0a1b2c3d4e5f60718&clientVersion=csharp%2F1.42.0"
                // TODO do we want to also include previous user agent details like OS, architecture, etc? Service currently discards those
                string username = $"correlationId={Uri.EscapeDataString(hexEncodedConnectNonce)}&clientVersion={Uri.EscapeDataString($"csharp/{GetPackageVersion()}")}";

                MqttConnect connectPacket = new MqttConnect()
                {
                    HostName = hostname,
                    TcpPort = 8883,
                    WebsocketPort = 443,
                    WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                    ClientCertificate = CurrentConnectionContext.AuthenticationProvider.ClientCertificate,
                    CleanSession = true, // TODO user configurable? Less applicable in gen 2 hub connection
                    Username = username,
                    Password = Array.Empty<byte>(),
                    ClientId = clientId,
                    ProtocolVersion = MqttProtocolVersion.V500
                };

                MqttConnectAck connack;
                try
                {
                    connack = await _mqttClient.ConnectAsync(connectPacket, cancellationToken);
                }
                catch (Exception ex)
                {
                    Trace.TraceWarning("Exception thrown while connecting to MQTT broker: {0}. Attempting connection again...", ex.Message);
                    continue; // Start the connect process over again
                }

                if (connack.ResultCode != MqttConnectResultCode.Success)
                {
                    subscribed = false;
                    Trace.TraceWarning("Received CONNACK with unsuccessful result code: {0}. Attempting connection again...", connack.ResultCode);
                    continue; // Start the connect process over again
                }

                // Only send the subscribe if it hasn't been sent in this MQTT session yet
                if (!subscribed || !connack.IsSessionPresent) //TODO subscribe elide logic
                {
                    subscribed = false;

                    try
                    {
                        // TODO this feels a bit optimistic since there is a chance that the session was established -> connection lost happened on the previous connection prior to this subscribe happening
                        var suback = await _mqttClient.SubscribeAsync(new(string.Format("ih/{0}/dev/#", deviceId), MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);
                        var subackFirstItem = suback.Items.FirstOrDefault();
                        if (subackFirstItem == null)
                        {
                            Trace.TraceWarning("Received malformed SUBACK. Attempting connection again...");
                            continue; // Start the connect process over again
                        }

                        if (subackFirstItem.ResultCode != MqttClientSubscribeResultCode.GrantedQoS1)
                        {
                            Trace.TraceWarning("Received SUBACK on devicebound SUBSCRIBE with unsuccessful result code: {0}. Attempting connection again...", subackFirstItem.ResultCode);
                            continue; // Start the connect process over again
                        }

                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("Exception thrown while subscribing to devicebound topic: {0}. Attempting connection again...", e.Message);
                        continue; // Start the connect process over again
                    }
                }

                subscribed = true;

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

                _mqttClient.PublishReceivedAsync += HandleReceivedBirthAck;

                MqttPublishAck birthMessagePuback;
                try
                {
                    birthMessagePuback = await _mqttClient.PublishAsync(birthMessage, cancellationToken);
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("Exception thrown while publishing birth message: {0}. Disconnecting from the MQTT broker and attempting connection again...", e.Message);

                    await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "MQTT client threw an exception while sending birth message" }, cancellationToken);
                    subscribed = false;

                    _mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    continue; // Start the connect process over again
                }

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message: {0}. Disconnecting from the MQTT broker and attempting connection again...", birthMessagePuback);

                    await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Birth message wasn't sent successfully" }, cancellationToken);
                    subscribed = false;

                    _mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    continue; // // Start the whole connect process over again
                }

                BirthAck birthAck;
                try
                {
                    birthAck = await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    // Did not receive mqtt birth ack message in timely manner (and user has not canceled this function yet)
                    Trace.TraceWarning("Timed out waiting for birth ack message. Disconnecting from the MQTT broker and attempting connection again...");

                    await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Timed out waiting for birth-ack publish" }, cancellationToken);
                    subscribed = false;

                    // MQTT keep-alive is what guarantees the device-broker connection is alive. As long as keep-alive is healthy, the device is connected to the broker, and the nominal expectation is that birth-ack arrives, full stop. A birth-ack timeout firing on a connection that keep-alive still considers healthy is therefore an unambiguous signal of system degeneration somewhere the device cannot influence: a slow backend, EG/EH egress lag, an in-broker dispatch stall. The SDK's only sensible response is to wait long enough for the degenerate component to recover before adding more load to it.
                    /*
                     Attempt 1: base 5 min + uniform jitter [0, 5 min] → range 5–10 min.
                     Attempt 2: base 6 min + uniform jitter [0, 5 min] → range 6–11 min.
                     Attempt 3: base 8 min + uniform jitter [0, 5 min] → range 8–13 min.
                     Attempt 4 and beyond: base 10 min + uniform jitter [0, 5 min] → range 10–15 min. 
                     */
                    //TODO add delays here before next connect attempt according to service spec above

                    _mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    continue; // Start the whole connect process over again
                }

                // Birth ack was received, so stop listening for birth acks.
                _mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
            }
        }

        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ProvisioningConnection provisioningConnection = new();
            return await provisioningConnection.RegisterAsync(_mqttClient, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO do we care about initial twin as returned by DPS?
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _mqttClient.PublishReceivedAsync -= DelegatePublishAsync;

            if (disposing)
            {
                _mqttClient.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                _mqttClient.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _mqttClient.PublishReceivedAsync -= DelegatePublishAsync;

            _mqttClient.Dispose();

            _isDisposed = true;
        }

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            //TODO to achieve a sort of "pause" on user-traffic when a reconnection happens, I could cancel this request (upon disconnect) down to the session client and then
            // re-submit it after connection has been re-established. Can maybe factor this out so that gen2 and unified connection client share it.
            return _mqttClient.PublishAsync(publish, cancellationToken);
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            // same as publish
            return _mqttClient.SubscribeAsync(subscribe, cancellationToken);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            // same as publish
            return _mqttClient.UnsubscribeAsync(unsubscribe, cancellationToken);
        }

        private static string GetPackageVersion()
        {
            return typeof(IotHubConnection).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }
    }
}
