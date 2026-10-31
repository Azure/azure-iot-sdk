// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Events;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class AsyncEventUnitTests
    {
        [Fact]
        public async Task InvokeAsyncAwaitsEveryHandlerEvenWhenOneThrows()
        {
            AsyncEvent<int> evt = new();
            List<string> calls = [];

            evt.Add(async _ =>
            {
                await Task.Yield();
                calls.Add("first");
            });
            evt.Add(_ => throw new InvalidOperationException("boom"));
            evt.Add(async _ =>
            {
                await Task.Delay(50);
                calls.Add("third");
            });

            InvalidOperationException e = await Assert.ThrowsAsync<InvalidOperationException>(() => evt.InvokeAsync(0));

            Assert.Equal("boom", e.Message);
            Assert.Equal(["first", "third"], calls);
        }

        [Fact]
        public async Task InvokeAsyncAggregatesExceptionsFromSeveralHandlers()
        {
            AsyncEvent<int> evt = new();
            evt.Add(_ => throw new InvalidOperationException());
            evt.Add(_ => Task.FromException(new TimeoutException()));

            AggregateException e = await Assert.ThrowsAsync<AggregateException>(() => evt.InvokeAsync(0));

            Assert.Collection(
                e.InnerExceptions,
                inner => Assert.IsType<InvalidOperationException>(inner),
                inner => Assert.IsType<TimeoutException>(inner));
        }

        [Fact]
        public async Task RemoveMatchesDelegateSemanticsForMulticastAndDuplicateHandlers()
        {
            AsyncEvent<int> evt = new();
            List<string> calls = [];
            Func<int, Task> a = _ => { calls.Add("a"); return Task.CompletedTask; };
            Func<int, Task> b = _ => { calls.Add("b"); return Task.CompletedTask; };

            evt.Add(a);
            evt.Add((Func<int, Task>)Delegate.Combine(b, a));
            evt.Remove(a);
            await evt.InvokeAsync(0);

            // Removing a single handler removes its last occurrence, so the first "a" and the "b" remain.
            Assert.Equal(["a", "b"], calls);

            evt.Remove(a);
            evt.Remove(b);
            Assert.False(evt.HasHandlers);
        }

        [Fact]
        public async Task PipelineEventChainsHandlersInOrder()
        {
            AsyncPipelineEvent<string> evt = new();
            evt.Add((value, _) => Task.FromResult(value + "1"));
            evt.Add((value, _) => Task.FromResult(value + "2"));

            Assert.Equal("x12", await evt.InvokeAsync("x", CancellationToken.None));
        }

        [Fact]
        public void SingleHandlerEventRejectsASecondHandler()
        {
            SingleHandlerEvent<Func<int, Task>> evt = new("TestEvent");
            Func<int, Task> first = _ => Task.CompletedTask;
            Func<int, Task> second = _ => Task.CompletedTask;

            evt.Add(first);
            Assert.Throws<InvalidOperationException>(() => evt.Add(second));
            Assert.Throws<InvalidOperationException>(() => new SingleHandlerEvent<Func<int, Task>>("TestEvent").Add(first + second));

            evt.Remove(first);
            evt.Add(second);
            Assert.Same(second, evt.Handler);
        }
    }
}
