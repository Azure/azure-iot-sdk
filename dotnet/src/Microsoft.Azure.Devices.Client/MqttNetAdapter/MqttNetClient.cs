using Microsoft.Azure.Devices.Client.Mqtt;
using MQTTnet;
using System.Net;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter
{
    public class MqttNetClient : Mqtt.IMqttClient 
    {
        private MQTTnet.IMqttClient _underlyingClient;

        private bool _useWebsocket;
        private IWebProxy? _proxy;

        // TODO Is this an appropriate way to give the user a chance to change tls settings/proxy settings/etc? Or maybe just ask users to provide their own impl at that point
        public Func<MqttClientOptionsBuilder, MqttClientOptionsBuilder>? ClientOptionsOverrider { get; set; }

        public MqttNetClient(MQTTnet.IMqttClient? underlyingClient = null, bool useWebsocket = false, IWebProxy? proxy = null)
        {
            _underlyingClient = underlyingClient ?? new MQTTnet.MqttClientFactory().CreateMqttClient();
            _useWebsocket = useWebsocket;
            _proxy = proxy;

            _underlyingClient.ApplicationMessageReceivedAsync += DelegateReceivedPublishAsync;
            _underlyingClient.ConnectedAsync += DelegateConnectedAsync;
            _underlyingClient.DisconnectedAsync += DelegateDisconnectedAsync;
        }

        public event Func<Mqtt.MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;
        public event Func<Mqtt.MqttClientConnectedEventArgs, Task>? ConnectedAsync;
        public event Func<Mqtt.MqttClientDisconnectedEventArgs, Task>? DisconnectedAsync;

        public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
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
                .WithCredentials(connect.Username, connect.Password);

            if (!_useWebsocket)
            {
                optionsBuilder.WithTcpServer(connect.HostName, connect.TcpPort);
            }
            else
            {
                string uriString = connect.WebsocketUri; // TODO this is diff for hub vs DPS
                optionsBuilder.WithWebSocketServer(options =>
                {
                    options.WithUri(uriString);

                    if (_proxy != null)
                    {
                        Uri serviceUri = new(uriString);
                        Uri? proxyUri = _proxy.GetProxy(serviceUri);

                        options.WithProxyOptions(proxyOptions =>
                        {
                            if (_proxy.Credentials != null)
                            {
                                NetworkCredential credentials = _proxy.Credentials.GetCredential(serviceUri, "Basic");
                                string username = credentials.UserName;
                                string password = credentials.Password;
                                proxyOptions.WithUsername(username);
                                proxyOptions.WithPassword(password);
                            }

                            proxyOptions.WithAddress(proxyUri.AbsoluteUri);
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

                    /*
                    if (_settings.RemoteCertificateValidationCallback != null)
                    {
                        tlsOptions.WithCertificateValidationHandler((args) => _settings.RemoteCertificateValidationCallback.Invoke(
                            mqttClient,
                            args.Certificate,
                            args.Chain,
                            args.SslPolicyErrors));
                    }
                    */

                    tlsOptions.UseTls(true);
                    tlsOptions.WithSslProtocols(System.Security.Authentication.SslProtocols.Tls12); //TODO support 1.3
                                                                                                    //tlsOptions.WithIgnoreCertificateRevocationErrors(!_settings.CertificateRevocationCheck);
                });
            }

            if (ClientOptionsOverrider != null)
            {
                // This allows the user to provide any MQTTnet-level settings that we may not already handle easily for them. Think TLS level settings like custom cipher suites
                optionsBuilder = ClientOptionsOverrider.Invoke(optionsBuilder);
            }

            var o = optionsBuilder.Build();

            if (o.UserProperties == null)
            {
                o.UserProperties = new();
            }

            var connectResult = await _underlyingClient.ConnectAsync(o, cancellationToken);

            var genericConnectResult = new MqttConnectAck()
            {
                ResultCode = ModelConverter.ToGeneric(connectResult.ResultCode),
            };

            if (connectResult.UserProperties != null)
            {
                foreach (var userProperty in connectResult.UserProperties)
                {
                    genericConnectResult.UserProperties.Add(new(userProperty.Name, userProperty.ValueBuffer));
                }
            }

            IsConnected = genericConnectResult.ResultCode == MqttConnectResultCode.Success;

            return genericConnectResult;
        }

        public async Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
        {
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
            IsConnected = false;
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            var messageBuilder = new MqttApplicationMessageBuilder()
                .WithContentType(publish.ContentType)
                .WithTopic(publish.Topic)
                .WithPayloadFormatIndicator(ModelConverter.ToMqttNet(publish.PayloadFormatIndicator))
                .WithPayload(publish.PayloadAsReadOnlySequence)
                .WithQualityOfServiceLevel(ModelConverter.ToMqttNet(publish.QualityOfServiceLevel))
                .WithMessageExpiryInterval(publish.MessageExpiryInterval);

            if (publish.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in publish.UserProperties)
                {
                    messageBuilder.WithUserProperty(userProperty.Name, userProperty.Value);
                }
            }

            return ModelConverter.ToGeneric(await _underlyingClient.PublishAsync(messageBuilder.Build(), cancellationToken));
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe mqttSubscribe, CancellationToken cancellationToken = default)
        {
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

            return ModelConverter.ToGeneric(await _underlyingClient.SubscribeAsync(subscribeBuilder.Build(), cancellationToken));
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
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

            return ModelConverter.ToGeneric(await _underlyingClient.UnsubscribeAsync(unsubscribeBuilder.Build(), cancellationToken));
        }

        public void Dispose()
        {
            _underlyingClient.ApplicationMessageReceivedAsync -= DelegateReceivedPublishAsync;
            _underlyingClient.ConnectedAsync -= DelegateConnectedAsync;
            _underlyingClient.DisconnectedAsync -= DelegateDisconnectedAsync;

            _underlyingClient.Dispose();
        }

        public bool IsConnected { get; internal set; }

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

            IsConnected = true;

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

            IsConnected = false;

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
