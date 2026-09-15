using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The wire contract for injecting faults into an <see cref="InProcessMqttBroker"/>: the control topics, the JSON
    /// payloads they carry, and the fault names those payloads can ask for.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Every fault the broker can inject is triggered by an ordinary PUBLISH to <see cref="RequestTopic"/>. Nothing about
    /// the trigger is a .NET call, so anything that can reach the broker's TCP port can inject a fault - another process,
    /// a test written in another language, or a <c>mosquitto_pub</c> one liner:
    /// </para>
    /// <code>
    /// mosquitto_pub -h 127.0.0.1 -p 1883 -t '$fault/req' \
    ///     -m '{"requestId":"1","fault":"disconnect","clientId":"device-1","reasonCode":137}'
    /// </code>
    /// <para>
    /// The whole structure of a fault lives in the payload rather than in MQTT 5 user properties, because MQTT 3.1.1 has
    /// no user properties and both the gen1 stubs and a classic device speak 3.1.1. The same bytes therefore work over
    /// either protocol version.
    /// </para>
    /// <para>
    /// A request that carries a <see cref="MqttFaultInjectionRequest.RequestId"/> is answered on
    /// <see cref="ResponseTopicFor(string)"/>, or on <see cref="MqttFaultInjectionRequest.ResponseTopic"/> when the caller
    /// names one. A request without either is still executed, so a caller that does not care about the outcome can fire
    /// and forget.
    /// </para>
    /// </remarks>
    public static class MqttFaultInjection
    {
        /// <summary>
        /// The topic the broker watches for fault injection requests. It is never forwarded to subscribers.
        /// </summary>
        public const string RequestTopic = "$fault/req";

        /// <summary>
        /// The prefix of the topic a request's response is published to.
        /// </summary>
        public const string ResponseTopicPrefix = "$fault/res/";

        /// <summary>
        /// The topic filter that receives the responses to every request, whoever made them.
        /// </summary>
        public const string ResponseTopicFilter = ResponseTopicPrefix + "#";

        /// <summary>
        /// The sender client id the broker attributes its responses to.
        /// </summary>
        public const string BrokerClientId = "$fault-injection-broker";

        /// <summary>
        /// The faults an <see cref="InProcessMqttBroker"/> knows how to inject, as they appear in
        /// <see cref="MqttFaultInjectionRequest.Fault"/>.
        /// </summary>
        public static class Faults
        {
            /// <summary>
            /// Terminate <see cref="MqttFaultInjectionRequest.ClientId"/>'s session with
            /// <see cref="MqttFaultInjectionRequest.ReasonCode"/>, optionally after
            /// <see cref="MqttFaultInjectionRequest.DelayMilliseconds"/>.
            /// </summary>
            public const string Disconnect = "disconnect";

            /// <summary>
            /// Report the client ids that currently hold a session, in
            /// <see cref="MqttFaultInjectionResponse.ClientIds"/>. Not a fault in itself: it is how a caller outside the
            /// broker's process picks a victim for one.
            /// </summary>
            public const string ListClients = "listClients";
        }

        private static readonly JsonSerializerOptions s_serializerOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        };

        /// <summary>
        /// The topic the response to the given request id is published to.
        /// </summary>
        public static string ResponseTopicFor(string requestId)
        {
            ArgumentException.ThrowIfNullOrEmpty(requestId);

            return ResponseTopicPrefix + requestId;
        }

        public static byte[] Serialize(MqttFaultInjectionRequest request)
        {
            ArgumentNullException.ThrowIfNull(request);

            return JsonSerializer.SerializeToUtf8Bytes(request, s_serializerOptions);
        }

        public static byte[] Serialize(MqttFaultInjectionResponse response)
        {
            ArgumentNullException.ThrowIfNull(response);

            return JsonSerializer.SerializeToUtf8Bytes(response, s_serializerOptions);
        }

        /// <summary>
        /// Read a request payload, or return null if the bytes are not a fault injection request at all.
        /// </summary>
        /// <remarks>
        /// A payload that cannot be read this far cannot be correlated to a response either, so the broker logs it and
        /// carries on rather than answering it. Requests that parse but ask for something impossible are answered with an
        /// error, which is the case a caller can actually act on.
        /// </remarks>
        public static MqttFaultInjectionRequest? DeserializeRequest(ReadOnlySpan<byte> payload)
        {
            try
            {
                return JsonSerializer.Deserialize<MqttFaultInjectionRequest>(payload, s_serializerOptions);
            }
            catch (JsonException)
            {
                return null;
            }
        }

        public static MqttFaultInjectionResponse? DeserializeResponse(ReadOnlySpan<byte> payload)
        {
            try
            {
                return JsonSerializer.Deserialize<MqttFaultInjectionResponse>(payload, s_serializerOptions);
            }
            catch (JsonException)
            {
                return null;
            }
        }
    }

    /// <summary>
    /// The payload of a PUBLISH to <see cref="MqttFaultInjection.RequestTopic"/>, which is the only way to make an
    /// <see cref="InProcessMqttBroker"/> inject a fault.
    /// </summary>
    public sealed class MqttFaultInjectionRequest
    {
        /// <summary>
        /// Correlates this request with its response. When null or empty the broker executes the request and stays silent.
        /// </summary>
        public string? RequestId { get; set; }

        /// <summary>
        /// Which fault to inject. See <see cref="MqttFaultInjection.Faults"/>.
        /// </summary>
        public string? Fault { get; set; }

        /// <summary>
        /// The MQTT client id the fault applies to. Required by <see cref="MqttFaultInjection.Faults.Disconnect"/>.
        /// </summary>
        /// <remarks>
        /// The SDK connects a device with its device id as the client id, so for a device under test the two are the same
        /// string.
        /// </remarks>
        public string? ClientId { get; set; }

        /// <summary>
        /// The numeric MQTT 5 DISCONNECT reason code to send, for example 137 for <c>ServerBusy</c>. Defaults to 0,
        /// <c>NormalDisconnection</c>.
        /// </summary>
        /// <remarks>
        /// This is the number rather than a name so that a caller in another language does not have to know MQTTnet's
        /// enum. A code that MQTT 5 does not define is rejected with an error response.
        /// </remarks>
        public int? ReasonCode { get; set; }

        /// <summary>
        /// The optional human readable reason string to accompany the reason code.
        /// </summary>
        public string? ReasonString { get; set; }

        /// <summary>
        /// How long the broker waits before injecting the fault. The response is published once the fault has been
        /// injected, so a caller that waits for it waits out the delay too.
        /// </summary>
        public int DelayMilliseconds { get; set; }

        /// <summary>
        /// Where to publish the response. Defaults to <see cref="MqttFaultInjection.ResponseTopicFor(string)"/> of
        /// <see cref="RequestId"/>.
        /// </summary>
        public string? ResponseTopic { get; set; }
    }

    /// <summary>
    /// The payload the broker publishes once it has handled a <see cref="MqttFaultInjectionRequest"/>.
    /// </summary>
    public sealed class MqttFaultInjectionResponse
    {
        /// <summary>
        /// The <see cref="MqttFaultInjectionRequest.RequestId"/> this answers.
        /// </summary>
        public string? RequestId { get; set; }

        /// <summary>
        /// The <see cref="MqttFaultInjectionRequest.Fault"/> that was asked for, echoed back.
        /// </summary>
        public string? Fault { get; set; }

        /// <summary>
        /// Whether the broker understood the request and carried it out without error.
        /// </summary>
        public bool Succeeded { get; set; }

        /// <summary>
        /// Whether the fault actually landed on something. A well formed disconnect request for a client that is not
        /// connected succeeds without applying anything.
        /// </summary>
        public bool FaultApplied { get; set; }

        /// <summary>
        /// The connected client ids, for <see cref="MqttFaultInjection.Faults.ListClients"/>.
        /// </summary>
        public IReadOnlyList<string>? ClientIds { get; set; }

        /// <summary>
        /// Why the request failed, when <see cref="Succeeded"/> is false.
        /// </summary>
        public string? Error { get; set; }
    }
}
