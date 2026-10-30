// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Runtime.ExceptionServices;

namespace Microsoft.Azure.Iot.Device.Events
{
    /// <summary>
    /// An asynchronous event that is broadcast to every handler.
    /// </summary>
    /// <remarks>
    /// Invoking a multicast <see cref="Func{T, TResult}"/> directly only returns the last handler's task, so the other
    /// handlers are not awaited and their exceptions are lost. This instead awaits each handler in the order they were
    /// added. A handler that throws does not stop the ones after it; once all have run, the exception is rethrown, or an
    /// <see cref="AggregateException"/> if more than one handler threw.
    /// </remarks>
    internal sealed class AsyncEvent<TArgs> : DelegateList<Func<TArgs, Task>>
    {
        /// <summary>
        /// Raise this event to every handler.
        /// </summary>
        /// <param name="args">The event arguments.</param>
        /// <param name="invokeHandler">
        /// Optionally wraps the invocation of each handler, for example to run it in a particular scope.
        /// </param>
        public async Task InvokeAsync(TArgs args, Func<Func<Task>, Task>? invokeHandler = null)
        {
            List<Exception>? exceptions = null;
            foreach (Func<TArgs, Task> handler in Handlers)
            {
                try
                {
                    if (invokeHandler == null)
                    {
                        await handler(args);
                    }
                    else
                    {
                        await invokeHandler(() => handler(args));
                    }
                }
                catch (Exception e)
                {
                    (exceptions ??= []).Add(e);
                }
            }

            if (exceptions?.Count == 1)
            {
                ExceptionDispatchInfo.Throw(exceptions[0]);
            }
            else if (exceptions != null)
            {
                throw new AggregateException(exceptions);
            }
        }
    }
}
