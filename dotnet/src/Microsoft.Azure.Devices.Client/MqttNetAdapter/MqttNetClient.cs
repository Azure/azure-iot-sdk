using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MqttNetAdapter;
using MQTTnet;
using System.Net;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter
{
    public class MqttNetClient : Mqtt.IMqttClient 
    {
        private bool _isDisposed = false;

        private MQTTnet.IMqttClient _underlyingClient;

        internal MqttConnectAck? mostRecentConnectAck;

        private bool _useWebsocket;
        private IWebProxy? _proxy;
        private TimeSpan _keepAlivePeriod;

        public MqttNetClient(MqttNetClientOptions? options = null)
        {
            options ??= new MqttNetClientOptions();

            if (options.EnableMqttLogs)
            {
                _underlyingClient = new MQTTnet.MqttClientFactory().CreateMqttClient(MqttNetTraceLogger.CreateTraceLogger());
            }
            else
            { 
                _underlyingClient = new MQTTnet.MqttClientFactory().CreateMqttClient();
            }

            _useWebsocket = options.UseWebsocket;
            _proxy = options.Proxy;
            _keepAlivePeriod = options.KeepAlivePeriod;

            _underlyingClient.ApplicationMessageReceivedAsync += DelegateReceivedPublishAsync;
            _underlyingClient.ConnectedAsync += DelegateConnectedAsync;
            _underlyingClient.DisconnectedAsync += DelegateDisconnectedAsync;
        }

        public event Func<Mqtt.MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;
        public event Func<Mqtt.MqttClientConnectedEventArgs, Task>? ConnectedAsync;
        public event Func<Mqtt.MqttClientDisconnectedEventArgs, Task>? DisconnectedAsync;
        public event Func<MqttConnect, Task<MqttConnect>>? ConnectingAsync;

        public virtual async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            if (ConnectingAsync != null)
            {
                connect = await ConnectingAsync.Invoke(connect); // Allow Gen2 connection client to inject a fresh connect nonce each time a connect happens
            }

            MqttClientOptionsBuilder optionsBuilder;
            if (connect.ProtocolVersion == MqttProtocolVersion.V500)
            {
                optionsBuilder = new MqttClientOptionsBuilder()
                    .WithProtocolVersion(MQTTnet.Formatter.MqttProtocolVersion.V500)
                    .WithCleanStart(connect.CleanStart);
            }
            else
            {
                optionsBuilder = new MqttClientOptionsBuilder()
                    .WithProtocolVersion(MQTTnet.Formatter.MqttProtocolVersion.V311)
                    .WithCleanSession(connect.CleanSession);
            }

            optionsBuilder
                .WithKeepAlivePeriod(connect.KeepAlivePeriod)
                .WithClientId(connect.ClientId)
                .WithKeepAlivePeriod(_keepAlivePeriod)
                .WithCredentials(connect.Username, connect.Password);

            if (!_useWebsocket)
            {
                optionsBuilder.WithTcpServer(connect.HostName, connect.TcpPort);
            }
            else
            {
                string? uriString = connect.WebsocketUri;
                optionsBuilder.WithWebSocketServer(options =>
                {
                    options.WithUri(uriString);

                    if (_proxy != null && uriString != null)
                    {
                        Uri serviceUri = new(uriString);
                        Uri? proxyUri = _proxy.GetProxy(serviceUri);

                        options.WithProxyOptions(proxyOptions =>
                        {
                            if (_proxy.Credentials != null)
                            {
                                NetworkCredential? credentials = _proxy.Credentials.GetCredential(serviceUri, "Basic");
                                if (credentials != null)
                                {
                                    string username = credentials.UserName;
                                    string password = credentials.Password;
                                    proxyOptions.WithUsername(username);
                                    proxyOptions.WithPassword(password);
                                }
                            }

                            if (proxyUri != null)
                            {
                                proxyOptions.WithAddress(proxyUri.AbsoluteUri);
                            }
                        });
                    }
                });
            }

            if (connect.ClientCertificate != null)
            {
                optionsBuilder.WithTlsOptions(tlsOptions =>
                {
                    tlsOptions.WithClientCertificates(new List<X509Certificate2>
                    {
                        connect.ClientCertificate,
                    });

                    tlsOptions.UseTls(true);
                    tlsOptions.WithSslProtocols(System.Security.Authentication.SslProtocols.Tls12 | System.Security.Authentication.SslProtocols.Tls13);
                    tlsOptions.WithIgnoreCertificateRevocationErrors(false);
                });
            }

            var o = optionsBuilder.Build();

            if (o.UserProperties == null)
            {
                o.UserProperties = new();
            }

            var connectResult = await _underlyingClient.ConnectAsync(o, cancellationToken);

            mostRecentConnectAck = new MqttConnectAck()
            {
                ResultCode = ModelConverter.ToGeneric(connectResult.ResultCode),
            };

            if (connectResult.UserProperties != null)
            {
                foreach (var userProperty in connectResult.UserProperties)
                {
                    mostRecentConnectAck.UserProperties.Add(new(userProperty.Name, userProperty.ValueBuffer));
                }
            }

            return mostRecentConnectAck;
        }

        public virtual async Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var disconnectBuilder = new MqttClientDisconnectOptionsBuilder()
                .WithReason(ModelConverter.ToMqttNet(disconnect.Reason))
                .WithReasonString(disconnect.ReasonString)
                .WithSessionExpiryInterval(disconnect.SessionExpiryInterval);

            if (disconnect.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in disconnect.UserProperties)
                {
                    disconnectBuilder.WithUserProperty(userProperty.Name, userProperty.Value);
                }
            }

            await _underlyingClient.DisconnectAsync(disconnectBuilder.Build(), cancellationToken);
        }

        //TODO throws MqttClientNotConnectedException
        public virtual async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var messageBuilder = new MqttApplicationMessageBuilder()
                .WithContentType(publish.ContentType)
                .WithTopic(publish.Topic)
                .WithPayloadFormatIndicator(ModelConverter.ToMqttNet(publish.PayloadFormatIndicator))
                .WithPayload(publish.PayloadAsReadOnlySequence)
                .WithQualityOfServiceLevel(ModelConverter.ToMqttNet(publish.QualityOfServiceLevel))
                .WithCorrelationData(publish.CorrelationData)
                .WithMessageExpiryInterval(publish.MessageExpiryInterval);

            if (publish.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in publish.UserProperties)
                {
                    messageBuilder.WithUserProperty(userProperty.Name, userProperty.Value);
                }
            }

            try
            {
                return ModelConverter.ToGeneric(await _underlyingClient.PublishAsync(messageBuilder.Build(), cancellationToken));
            }
            catch (MQTTnet.Exceptions.MqttClientNotConnectedException e)
            {
                throw new MqttClientNotConnectedException(e.Message, e);
            }
        }

        public virtual async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe mqttSubscribe, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var subscribeBuilder = new MqttClientSubscribeOptionsBuilder();
            foreach (var topicFilter in mqttSubscribe.TopicFilters)
            {
                subscribeBuilder.WithTopicFilter(topicFilter.Topic, ModelConverter.ToMqttNet(topicFilter.QualityOfServiceLevel));
            }

            if (mqttSubscribe.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in mqttSubscribe.UserProperties)
                {
                    subscribeBuilder.WithUserProperty(userProperty.Name, userProperty.Value);
                }
            }

            try
            {
                return ModelConverter.ToGeneric(await _underlyingClient.SubscribeAsync(subscribeBuilder.Build(), cancellationToken));
            }
            catch (MQTTnet.Exceptions.MqttClientNotConnectedException e)
            {
                throw new MqttClientNotConnectedException(e.Message, e);
            }
        }

        public virtual async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var unsubscribeBuilder = new MqttClientUnsubscribeOptionsBuilder();
            foreach (var topicFilter in unsubscribe.TopicFilters)
            { 
                unsubscribeBuilder.WithTopicFilter(topicFilter);
            }

            if (unsubscribe.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in unsubscribe.UserProperties)
                {
                    unsubscribeBuilder.WithUserProperty(userProperty.Name, userProperty.Value);
                }
            }

            try
            {
                return ModelConverter.ToGeneric(await _underlyingClient.UnsubscribeAsync(unsubscribeBuilder.Build(), cancellationToken));
            }
            catch (MQTTnet.Exceptions.MqttClientNotConnectedException e)
            {
                throw new MqttClientNotConnectedException(e.Message, e);
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _underlyingClient.ApplicationMessageReceivedAsync -= DelegateReceivedPublishAsync;
            _underlyingClient.ConnectedAsync -= DelegateConnectedAsync;
            _underlyingClient.DisconnectedAsync -= DelegateDisconnectedAsync;

            _underlyingClient.Dispose();
            _isDisposed = true;
        }

        public bool IsConnected() => _underlyingClient.IsConnected;

        private Task DelegateReceivedPublishAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            if (PublishReceivedAsync == null)
            {
                return Task.CompletedTask; //TODO what to do with received MQTT message when user doesn't have callback set. Does this even happen?
            }

            MqttPublishReceivedEventArgs genericArgs = new MqttPublishReceivedEventArgsImpl(args)
            {
                Publish = new()
                {
                    PayloadAsReadOnlySequence = args.ApplicationMessage.Payload,
                    ContentType = args.ApplicationMessage.ContentType,
                    CorrelationData = args.ApplicationMessage.CorrelationData,
                    MessageExpiryInterval = args.ApplicationMessage.MessageExpiryInterval,
                    PayloadFormatIndicator = ModelConverter.ToGeneric(args.ApplicationMessage.PayloadFormatIndicator),
                    QualityOfServiceLevel = ModelConverter.ToGeneric(args.ApplicationMessage.QualityOfServiceLevel),
                    Topic = args.ApplicationMessage.Topic,
                }
            };

            if (args.ApplicationMessage.UserProperties != null)
            {
                foreach (var userProperty in args.ApplicationMessage.UserProperties)
                {
                    genericArgs.Publish.UserProperties.Add(new(userProperty.Name, userProperty.ValueBuffer));
                }
            }

            args.AutoAcknowledge = false; // TODO do we want to do AutoAck things in generic interface as well? For now, assume always manual ack

            if (PublishReceivedAsync != null)
            {
                return PublishReceivedAsync.Invoke(genericArgs);
            }

            return Task.CompletedTask;
        }

        private Task DelegateConnectedAsync(MQTTnet.MqttClientConnectedEventArgs args)
        {
            if (ConnectedAsync == null)
            {
                return Task.CompletedTask;
            }

            return ConnectedAsync.Invoke(new()
            {
                ConnectAck = ModelConverter.ToGeneric(args.ConnectResult),
            });
        }

        private Task DelegateDisconnectedAsync(MQTTnet.MqttClientDisconnectedEventArgs args)
        {
            if (DisconnectedAsync == null)
            {
                return Task.CompletedTask;
            }

            var genericArgs = new Mqtt.MqttClientDisconnectedEventArgs()
            {
                Exception = args.Exception,
                Reason = ModelConverter.ToGeneric(args.Reason),
            };

            if (args.UserProperties != null)
            {
                foreach (var userProperty in args.UserProperties)
                {
                    genericArgs.UserProperties.Add(new(userProperty.Name, userProperty.ValueBuffer));
                }
            }

            if (DisconnectedAsync != null)
            {
                return DisconnectedAsync.Invoke(genericArgs);
            }

            return Task.CompletedTask;
        }
    }
}
