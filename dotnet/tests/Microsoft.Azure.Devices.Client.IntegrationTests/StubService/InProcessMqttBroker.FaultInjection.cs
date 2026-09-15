using MQTTnet;
using MQTTnet.Protocol;
using MQTTnet.Server;
using System.Buffers;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The half of the broker that injects faults, and the only way to make it do so: a PUBLISH to
    /// <see cref="MqttFaultInjection.RequestTopic"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The broker deliberately exposes no .NET method that injects a fault. Everything goes through the control topics, so
    /// a fault can be triggered from outside the process that hosts this broker - the whole point of the exercise - and an
    /// in-process caller gets no shortcut that an out-of-process one lacks.
    /// </para>
    /// <para>
    /// See <see cref="MqttFaultInjectionClient"/> for the client half of the protocol.
    /// </para>
    /// </remarks>
    public sealed partial class InProcessMqttBroker
    {
        private readonly CancellationTokenSource _faultInjectionCancellation = new();

        private void AttachFaultInjection()
        {
            _server.InterceptingPublishAsync += InterceptFaultInjectionRequestAsync;
        }

        private void DetachFaultInjection()
        {
            _server.InterceptingPublishAsync -= InterceptFaultInjectionRequestAsync;
        }

        private Task InterceptFaultInjectionRequestAsync(InterceptingPublishEventArgs args)
        {
            if (!string.Equals(args.ApplicationMessage.Topic, MqttFaultInjection.RequestTopic, StringComparison.Ordinal))
            {
                return Task.CompletedTask;
            }

            // Control traffic is between the caller and the broker, so it never reaches the topic space the devices and
            // the stubs share.
            args.ProcessPublish = false;

            byte[] payload = args.ApplicationMessage.Payload.ToArray();

            // A fault can disconnect the very client that asked for it, and can be asked for with a delay, so it must not
            // run on the pipeline that still owes this PUBLISH its acknowledgement. The caller correlates on the response,
            // which it subscribed to before publishing.
            _ = Task.Run(() => HandleFaultInjectionRequestAsync(args.ClientId, payload));

            return Task.CompletedTask;
        }

        private async Task HandleFaultInjectionRequestAsync(string senderClientId, byte[] payload)
        {
            MqttFaultInjectionRequest? request = MqttFaultInjection.DeserializeRequest(payload);
            if (request == null)
            {
                // Without a parse there is no request id, so there is nowhere to send an error. Logging it is all that is
                // left, and a malformed request must not take the broker down.
                Log($"The broker ignored an unreadable fault injection request from '{senderClientId}'.");
                return;
            }

            MqttFaultInjectionResponse response;

            try
            {
                response = await ExecuteFaultAsync(request, _faultInjectionCancellation.Token);
            }
            catch (OperationCanceledException)
            {
                // The broker is shutting down, which leaves nothing to answer on.
                return;
            }
            catch (Exception e)
            {
                response = Failed(request, e.Message);
            }

            await PublishFaultInjectionResponseAsync(request, response);
        }

        private async Task<MqttFaultInjectionResponse> ExecuteFaultAsync(
            MqttFaultInjectionRequest request,
            CancellationToken cancellationToken)
        {
            if (string.IsNullOrEmpty(request.Fault))
            {
                return Failed(request, "The request named no fault.");
            }

            if (request.DelayMilliseconds < 0)
            {
                return Failed(request, $"A fault cannot be delayed by {request.DelayMilliseconds} milliseconds.");
            }

            switch (request.Fault)
            {
                case MqttFaultInjection.Faults.Disconnect:
                    return await ExecuteDisconnectFaultAsync(request, cancellationToken);

                case MqttFaultInjection.Faults.ListClients:
                    return new MqttFaultInjectionResponse
                    {
                        RequestId = request.RequestId,
                        Fault = request.Fault,
                        Succeeded = true,
                        FaultApplied = true,
                        ClientIds = await GetConnectedClientIdsAsync(cancellationToken),
                    };

                default:
                    return Failed(request, $"The broker cannot inject a fault named '{request.Fault}'.");
            }
        }

        private async Task<MqttFaultInjectionResponse> ExecuteDisconnectFaultAsync(
            MqttFaultInjectionRequest request,
            CancellationToken cancellationToken)
        {
            if (string.IsNullOrEmpty(request.ClientId))
            {
                return Failed(request, $"The '{MqttFaultInjection.Faults.Disconnect}' fault requires a client id.");
            }

            int rawReasonCode = request.ReasonCode ?? (int)MqttDisconnectReasonCode.NormalDisconnection;
            if (!Enum.IsDefined((MqttDisconnectReasonCode)rawReasonCode))
            {
                return Failed(request, $"{rawReasonCode} is not an MQTT disconnect reason code.");
            }

            if (request.DelayMilliseconds > 0)
            {
                await Task.Delay(request.DelayMilliseconds, cancellationToken);
            }

            bool disconnected = await DisconnectClientCoreAsync(
                request.ClientId,
                (MqttDisconnectReasonCode)rawReasonCode,
                request.ReasonString,
                cancellationToken);

            return new MqttFaultInjectionResponse
            {
                RequestId = request.RequestId,
                Fault = request.Fault,
                Succeeded = true,
                FaultApplied = disconnected,
            };
        }

        /// <summary>
        /// Terminate a client's connection with the given MQTT disconnect reason code.
        /// </summary>
        /// <remarks>
        /// MQTT 3.1.1 has no server-to-client DISCONNECT packet, so a real classic hub or DPS endpoint can only close the
        /// socket. MQTTnet's broker is more forthcoming and hands the reason code to 3.1.1 clients as well, which means a
        /// gen1 or provisioning device sees a reason here that it would not see in the cloud. Tests that care about the
        /// difference should assert against the stub's own drop history rather than the device's disconnect arguments.
        /// </remarks>
        /// <returns>True if a connection was found and dropped, false if no such client was connected.</returns>
        private async Task<bool> DisconnectClientCoreAsync(
            string clientId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString,
            CancellationToken cancellationToken)
        {
            MqttServerClientDisconnectOptionsBuilder optionsBuilder = new MqttServerClientDisconnectOptionsBuilder()
                .WithReasonCode(reasonCode);

            if (reasonString != null)
            {
                optionsBuilder.WithReasonString(reasonString);
            }

            IReadOnlyList<string> connectedClientIds = await GetConnectedClientIdsAsync(cancellationToken).ConfigureAwait(false);
            if (!connectedClientIds.Contains(clientId, StringComparer.Ordinal))
            {
                return false;
            }

            try
            {
                await _server.DisconnectClientAsync(clientId, optionsBuilder.Build()).WaitAsync(cancellationToken).ConfigureAwait(false);
                Log($"The broker disconnected '{clientId}' with reason code {reasonCode}.");

                return true;
            }
            catch (Exception e) when (e is ObjectDisposedException or InvalidOperationException or KeyNotFoundException)
            {
                // The client raced this call and disconnected on its own, or the broker is shutting down.
                return false;
            }
        }

        private async Task PublishFaultInjectionResponseAsync(MqttFaultInjectionRequest request, MqttFaultInjectionResponse response)
        {
            string? topic = request.ResponseTopic
                ?? (string.IsNullOrEmpty(request.RequestId) ? null : MqttFaultInjection.ResponseTopicFor(request.RequestId));

            if (string.IsNullOrEmpty(topic))
            {
                // A caller that supplied neither a request id nor a response topic asked for the fault and nothing else.
                return;
            }

            MqttApplicationMessage message = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(MqttFaultInjection.Serialize(response))
                .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                .Build();

            try
            {
                await _server.InjectApplicationMessage(
                    new InjectedMqttApplicationMessage(message) { SenderClientId = MqttFaultInjection.BrokerClientId },
                    _faultInjectionCancellation.Token);
            }
            catch (Exception e) when (e is ObjectDisposedException or OperationCanceledException)
            {
                // The broker stopped between injecting the fault and reporting it.
            }
        }

        private static MqttFaultInjectionResponse Failed(MqttFaultInjectionRequest request, string error)
        {
            return new MqttFaultInjectionResponse
            {
                RequestId = request.RequestId,
                Fault = request.Fault,
                Succeeded = false,
                FaultApplied = false,
                Error = error,
            };
        }
    }
}
