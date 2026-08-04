using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.ComponentModel;

namespace Microsoft.Azure.Devices.Client.Connection.Gen2
{
    public interface IConnectionClient : IDisposable
    {
        /// <summary>
        /// Get the current connection context.
        /// </summary>
        /// <returns>Null if this connection client has not been connected yet. Otherwise, it returns the current connection context.</returns>
        public ConnectionContext? GetCurrentConnectionContext();

        /// <summary>
        /// An event that executes each time this client receives an MQTT publish.
        /// </summary>
        /// <remarks>This event is deliberately hidden from editors because a user of this SDK should never need to subscribe to this event. It is only public for mocking purposes.</remarks>
        [EditorBrowsableAttribute(EditorBrowsableState.Advanced)]
        public event Func<MqttPublishReceivedEventArgs, Task>? ApplicationMessageReceivedAsync;

        /// <summary>
        /// An event that executes each time this client is connected to IoT hub.
        /// </summary>
        public event Action<MqttClientConnectedEventArgs>? ConnectedAsync;

        /// <summary>
        /// An event that executes each time this client is disconnected from IoT hub.
        /// </summary>
        public event Action<MqttClientDisconnectedEventArgs>? DisconnectedAsync;

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        public Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(CertificateSigningRequest request, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an MQTT publish on this connection.
        /// </summary>
        /// <param name="publish">The publish to send.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>The MQTT puback.</returns>
        /// <remarks>This method is deliberately hidden from editors because a user of this SDK should never need to call this method. It is only public for mocking purposes.</remarks>
        [EditorBrowsable(EditorBrowsableState.Advanced)]
        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an MQTT subscribe on this connection
        /// </summary>
        /// <param name="subscribe">The subscribe to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The MQTT suback.</returns>
        /// <remarks>This method is deliberately hidden from editors because a user of this SDK should never need to call this method. It is only public for mocking purposes.</remarks>
        [EditorBrowsable(EditorBrowsableState.Advanced)]
        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an MQTT unsubscribe on this connection
        /// </summary>
        /// <param name="unsubscribe">The unsubscribe to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The MQTT unsuback.</returns>
        /// <remarks>This method is deliberately hidden from editors because a user of this SDK should never need to call this method. It is only public for mocking purposes.</remarks>
        [EditorBrowsable(EditorBrowsableState.Advanced)]
        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default);
    }
}
