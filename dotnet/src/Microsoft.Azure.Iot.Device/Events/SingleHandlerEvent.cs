// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Events
{
    /// <summary>
    /// An event that accepts only one handler, for events whose handler produces a result that only one party can provide
    /// (such as the response to a direct method).
    /// </summary>
    internal sealed class SingleHandlerEvent<THandler> where THandler : Delegate
    {
        private readonly object _lock = new();
        private readonly string _eventName;
        private THandler? _handler;

        public SingleHandlerEvent(string eventName)
        {
            _eventName = eventName;
        }

        public THandler? Handler => Volatile.Read(ref _handler);

        /// <exception cref="InvalidOperationException">A handler was already added, or <paramref name="handler"/> combines more than one handler.</exception>
        public void Add(THandler? handler)
        {
            if (handler == null)
            {
                return;
            }

            if (handler.GetInvocationList().Length > 1)
            {
                throw new InvalidOperationException($"{_eventName} accepts only one handler, but a delegate combining several handlers was added.");
            }

            lock (_lock)
            {
                if (_handler != null)
                {
                    throw new InvalidOperationException($"{_eventName} accepts only one handler. Remove the existing handler before adding another.");
                }

                Volatile.Write(ref _handler, handler);
            }
        }

        public void Remove(THandler? handler)
        {
            lock (_lock)
            {
                if (handler != null && handler.Equals(_handler))
                {
                    Volatile.Write(ref _handler, null);
                }
            }
        }
    }
}
