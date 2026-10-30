// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Events
{
    /// <summary>
    /// The handlers of an event, stored individually so that each one can be invoked (and awaited) on its own.
    /// </summary>
    /// <remarks>
    /// Adding and removing follow the semantics of <c>+=</c> and <c>-=</c> on a delegate: a multicast delegate is added
    /// as each of its handlers, and removing a delegate removes the last occurrence of its handlers. Raising an event
    /// works on a snapshot, so handlers added or removed while it is being raised take effect from the next raise.
    /// </remarks>
    internal abstract class DelegateList<THandler> where THandler : Delegate
    {
        private readonly object _lock = new();
        private THandler[] _handlers = [];

        protected THandler[] Handlers => Volatile.Read(ref _handlers);

        public bool HasHandlers => Handlers.Length > 0;

        public void Add(THandler? handler)
        {
            if (handler == null)
            {
                return;
            }

            THandler[] added = handler.GetInvocationList().Cast<THandler>().ToArray();
            lock (_lock)
            {
                Volatile.Write(ref _handlers, [.. _handlers, .. added]);
            }
        }

        public void Remove(THandler? handler)
        {
            if (handler == null)
            {
                return;
            }

            Delegate[] removed = handler.GetInvocationList();
            lock (_lock)
            {
                THandler[] current = _handlers;
                for (int start = current.Length - removed.Length; start >= 0; start--)
                {
                    bool matches = true;
                    for (int i = 0; i < removed.Length && matches; i++)
                    {
                        matches = current[start + i].Equals(removed[i]);
                    }

                    if (matches)
                    {
                        Volatile.Write(ref _handlers, [.. current[..start], .. current[(start + removed.Length)..]]);
                        return;
                    }
                }
            }
        }
    }
}
