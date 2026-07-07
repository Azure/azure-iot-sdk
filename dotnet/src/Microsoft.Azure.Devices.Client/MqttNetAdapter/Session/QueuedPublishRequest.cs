// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session
{
    /// <summary>
    /// A single enqueued publish and its associated metadata.
    /// </summary>
    internal class QueuedPublishRequest : QueuedRequest
    {
        internal MqttPublish Request { get; }
        
        internal TaskCompletionSource<MqttPublishAck> ResultTaskCompletionSource { get; }

        internal QueuedPublishRequest(
            MqttPublish request,
            TaskCompletionSource<MqttPublishAck> resultTaskCompletionSource,
            CancellationToken cancellationToken = default)
            : base(cancellationToken)
        {
            Request = request;
            ResultTaskCompletionSource = resultTaskCompletionSource;
        }

        internal override void OnException(Exception reason)
        {
            ResultTaskCompletionSource.TrySetException(reason);
        }
    }
}
