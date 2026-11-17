// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// An HTTP proxy on loopback that records the request line of each tunnel it is asked to open (for example,
    /// "CONNECT some-hub.azure-devices.net:443 HTTP/1.1") and then refuses it, so that tests can see where traffic
    /// would have gone without reaching the network.
    /// </summary>
    internal sealed class LoopbackConnectProxy : IDisposable
    {
        private static readonly byte[] s_refusal = Encoding.ASCII.GetBytes("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");

        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _stop = new();
        private readonly ConcurrentQueue<string> _requestLines = new();
        private readonly Task _acceptLoop;

        public LoopbackConnectProxy()
        {
            _listener.Start();
            Address = new Uri($"http://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}");
            _acceptLoop = AcceptAsync();
        }

        public Uri Address { get; }

        public IReadOnlyCollection<string> RequestLines => _requestLines.ToArray();

        private async Task AcceptAsync()
        {
            try
            {
                while (true)
                {
                    TcpClient client = await _listener.AcceptTcpClientAsync(_stop.Token);
                    _ = RefuseAsync(client);
                }
            }
            catch (Exception ex) when (ex is OperationCanceledException or SocketException or ObjectDisposedException)
            {
            }
        }

        private async Task RefuseAsync(TcpClient client)
        {
            using (client)
            {
                try
                {
                    NetworkStream stream = client.GetStream();
                    using StreamReader reader = new(stream, Encoding.ASCII, false, 1024, leaveOpen: true);
                    string? requestLine = await reader.ReadLineAsync(_stop.Token);
                    if (requestLine != null)
                    {
                        _requestLines.Enqueue(requestLine);
                    }

                    // Read the rest of the request headers so that the client sees the refusal rather than a reset.
                    while (!string.IsNullOrEmpty(await reader.ReadLineAsync(_stop.Token)))
                    {
                    }

                    await stream.WriteAsync(s_refusal, _stop.Token);
                }
                catch (Exception ex) when (ex is OperationCanceledException or IOException or SocketException or ObjectDisposedException)
                {
                }
            }
        }

        public void Dispose()
        {
            _stop.Cancel();
            _listener.Stop();
            _acceptLoop.GetAwaiter().GetResult();
            _stop.Dispose();
        }
    }
}
