// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session
{
    /// <summary>
    /// A single enqueued subscribe and its associated metadata.
    /// </summary>
    internal class QueuedSubscribeRequest : QueuedRequest
    {
        internal MqttSubscribe Request { get; }
        
        internal TaskCompletionSource<MqttSubscribeAck> ResultTaskCompletionSource { get; }

        internal QueuedSubscribeRequest(
            MqttSubscribe request,
            TaskCompletionSource<MqttSubscribeAck> resultTaskCompletionSource,
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
