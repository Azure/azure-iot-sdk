// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Events
{
    /// <summary>
    /// An asynchronous event whose handlers each transform a value, such as a CONNECT packet about to be sent.
    /// </summary>
    /// <remarks>
    /// The handlers are chained in the order they were added: each receives the value the previous one returned, and the
    /// last one's result is the event's result. A handler that throws ends the chain, since the handlers after it have no
    /// value to work on.
    /// </remarks>
    internal sealed class AsyncPipelineEvent<T> : DelegateList<Func<T, CancellationToken, Task<T>>>
    {
        public async Task<T> InvokeAsync(T value, CancellationToken cancellationToken)
        {
            foreach (Func<T, CancellationToken, Task<T>> handler in Handlers)
            {
                value = await handler(value, cancellationToken);
            }

            return value;
        }
    }
}
