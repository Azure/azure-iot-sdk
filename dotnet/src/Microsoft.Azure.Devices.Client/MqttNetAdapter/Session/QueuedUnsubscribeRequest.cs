// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session
{
    /// <summary>
    /// A single enqueued unsubscribe and its associated metadata.
    /// </summary>
    internal class QueuedUnsubscribeRequest : QueuedRequest
    {
        internal MqttUnsubscribe Request { get; }
        
        internal TaskCompletionSource<MqttUnsubscribeAck> ResultTaskCompletionSource { get; }

        internal QueuedUnsubscribeRequest(
            MqttUnsubscribe request,
            TaskCompletionSource<MqttUnsubscribeAck> resultTaskCompletionSource,
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
