// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Retry;
using System.Diagnostics;
using System.Net.Sockets;

namespace Microsoft.Azure.Devices.Client
{
    internal class MqttConnectionManager //TODO need some logging at this level for the higher-order concepts around going from maintaining connection -> stopping and so on.
    {
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        public event Func<MqttClientConnectedEventArgs, Task>? ConnectedAsync;

        public event Func<MqttConnect, Task<MqttConnect>>? ConnectingAsync;

        public event Func<MqttClientDisconnectedEventArgs, Task>? DisconnectedAsync;

        private readonly IRetryPolicy _connectionRetryPolicy;

        private readonly TimeSpan _connectionAttemptTimeout;

        private MqttConnect? _mostRecentConnect;
        private readonly bool _isDisposed = false;

        private bool _isDesiredConnected;
        private bool _isClosing;
        private CancellationTokenSource? _reconnectionCancellationToken;

        private readonly SemaphoreSlim _disconnectedEventLock = new(1);

        private IMqttClient _underlyingMqttClient;

        public MqttConnectionManager(IMqttClient underlyingMqttClient, TimeSpan connectionAttemptTimeout, IRetryPolicy retryPolicy)
        {
            _underlyingMqttClient = underlyingMqttClient;
            _connectionAttemptTimeout = connectionAttemptTimeout;
            _connectionRetryPolicy = retryPolicy;

            _underlyingMqttClient.DisconnectedAsync += InternalDisconnectedAsync;

            _underlyingMqttClient.DisconnectedAsync += DelegateDisconnectedAsync;
            _underlyingMqttClient.ConnectingAsync += DelegateConnectingAsync;
            _underlyingMqttClient.ConnectedAsync += DelegateConnectedAsync;
            _underlyingMqttClient.PublishReceivedAsync += DelegatePublishReceivedAsync;

        }

        private async Task DelegatePublishReceivedAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                _ = PublishReceivedAsync.Invoke(args);
            }
        }

        private async Task DelegateConnectedAsync(MqttClientConnectedEventArgs args)
        {
            if (ConnectedAsync != null)
            {
                _ = ConnectedAsync.Invoke(args);
            }
        }

        private async Task<MqttConnect> DelegateConnectingAsync(MqttConnect connect)
        {
            if (ConnectingAsync != null)
            {
                connect = await ConnectingAsync.Invoke(connect);
            }

            return connect;
        }

        private async Task DelegateDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            if (DisconnectedAsync != null)
            {
                _ = DisconnectedAsync.Invoke(args);
            }
        }

        public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            cancellationToken.ThrowIfCancellationRequested();

            if (_isDesiredConnected)
            {
                // should this just return "OK"? Or null since no CONNACK was received?
                throw new InvalidOperationException("The client is already managing the connection.");
            }

            ArgumentNullException.ThrowIfNull(connect);

            _mostRecentConnect = connect;

            _isClosing = false;
            MqttConnectAck? connectResult = await MaintainConnectionAsync(connect, null, cancellationToken);

            // By design, MaintainConnectionAsync should only return null when called during reconnection.
            // When called by this method, MaintainConnectionAsync should return a non-null value or throw.
            Debug.Assert(connectResult != null);
            _isDesiredConnected = true;

            return connectResult;
        }

        public async Task DisconnectAsync(bool desireReconnection, MqttDisconnect? options = null, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            options ??= new MqttDisconnect();
            options.SessionExpiryInterval = 0;

            _isClosing = true;
            _reconnectionCancellationToken?.Cancel();
            await _underlyingMqttClient.DisconnectAsync(options, cancellationToken);

            if (!desireReconnection)
            {
                var disconnectedArgs = new MqttClientDisconnectedEventArgs()
                {
                    Reason = MqttDisconnectReason.NormalDisconnection,
                    ReasonString = "User closed the connection manually"
                };

                await EndConnectionMaintanceAsync(new OperationCanceledException("This operation was canceled because the MQTT client was closed."), disconnectedArgs, cancellationToken);
            }
        }

        public void Dispose()
        {
            if (!_isDisposed)
            {
                _underlyingMqttClient.DisconnectedAsync -= InternalDisconnectedAsync;

                _reconnectionCancellationToken?.Dispose();
                _disconnectedEventLock.Dispose();
            }

            _disconnectedEventLock.Dispose();

            // The underlying client has an MQTT client as a managed resource that no other client has access to, so always dispose it
            // alongside all unmanaged resources.
            _underlyingMqttClient.Dispose();

            GC.SuppressFinalize(this);
        }

        private async Task InternalDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            // MQTTNet's client often triggers the same "OnDisconnect" callback more times than expected, so only start reconnection once
            await _disconnectedEventLock.WaitAsync();

            try
            {
                if (_isDesiredConnected)
                {
                    if (_underlyingMqttClient.IsConnected())
                    {
                        return;
                    }

                    if (IsFatal(args.Reason))
                    {
                        var retryException = new RetryExpiredException("A fatal error was encountered while trying to re-establish the session, so this request cannot be completed.", args.Exception);
                        await EndConnectionMaintanceAsync(retryException, args, CancellationToken.None);
                        return;
                    }

                    Trace.TraceInformation("Disconnect detected, starting reconnection. Disconnect reason: {0}", args.Reason);

                    _reconnectionCancellationToken?.Dispose();
                    _reconnectionCancellationToken = new();

                    // The most recent connect cache should be set since at least one connect must happen before this "Disconnected" callback is triggered
                    Debug.Assert(_mostRecentConnect != null);

                    // start reconnection if the user didn't initiate this disconnect
                    await MaintainConnectionAsync(_mostRecentConnect, args, _reconnectionCancellationToken.Token);
                }
            }
            finally
            {
                _disconnectedEventLock.Release();
            }
        }

        private async Task<MqttConnectAck?> MaintainConnectionAsync(MqttConnect options, MqttClientDisconnectedEventArgs? lastDisconnect, CancellationToken cancellationToken)
        {
            // This function is either called when initially connecting the client or when reconnecting it. The behavior
            // of this function should change depending on the context it was called. For instance, thrown exceptions are the expected
            // behavior when called from the initial ConnectAsync thread, but any exceptions thrown in the reconnection thread will be
            // unhandled and may crash the client.
            bool isReconnection = lastDisconnect != null;
            uint attemptCount = 1;
            MqttConnectAck? mostRecentConnectResult = null;
            Exception? lastException = lastDisconnect?.Exception;
            TimeSpan retryDelay = TimeSpan.Zero;

            while (true)
            {
                // This flag signals that the user is trying to close the connection. If this happens when the client is reconnection,
                // simply abandon reconnecting and end this task.
                if (_isClosing && isReconnection)
                {
                    return null;
                }
                else if (_isClosing && lastException != null)
                {
                    // If the user disconnects the client while they were trying to connect it,
                    // stop trying to connect it and just report the most recent error.
                    throw lastException;
                }

                if (IsFatal(lastException!, _reconnectionCancellationToken?.Token.IsCancellationRequested ?? cancellationToken.IsCancellationRequested))
                {
                    Trace.TraceError("Encountered a fatal exception while maintaining connection {0}", lastException);
                    if (isReconnection)
                    {
                        var retryException = new RetryExpiredException("A fatal error was encountered while trying to re-establish the session, so this request cannot be completed.", lastException!);

                        // This function was called to reconnect after an unexpected disconnect. Since the error is fatal,
                        // notify the user via callback that the client has crashed, but don't throw the exception since
                        // this task is unmonitored.
                        await EndConnectionMaintanceAsync(retryException, lastDisconnect!, cancellationToken);
                        return null;
                    }
                    else
                    {
                        // This function was called directly by the user via ConnectAsync, so just throw the exception.
                        throw lastException!;
                    }
                }

                // Always consult the retry policy when reconnecting, but only consult it on attempt > 1 when
                // initially connecting
                if ((isReconnection || attemptCount > 1)
                    && !_connectionRetryPolicy.ShouldRetry(attemptCount, lastException!, out retryDelay))
                {
                    // Should not occur as it's indefinite retry
                    Trace.TraceError("Retry policy was exhausted while trying to maintain a connection {0}", lastException);
                    var retryException = new RetryExpiredException("Retry policy has been exhausted. See inner exception for the latest exception encountered while retrying.", lastException!);

                    if (lastDisconnect != null)
                    {
                        // This function was called to reconnect after an unexpected disconnect. Since the error is fatal,
                        // notify the user via callback that the client has crashed, but don't throw the exception since
                        // this task is unmonitored.
                        var disconnectedEventArgs = new MqttClientDisconnectedEventArgs()
                        {
                            Exception = retryException,
                            Reason = lastDisconnect.Reason,
                            ReasonString = lastDisconnect.ReasonString,
                            UserProperties = lastDisconnect.UserProperties,
                        };

                        await EndConnectionMaintanceAsync(retryException, disconnectedEventArgs, cancellationToken);
                        return null;
                    }
                    else
                    {
                        // This function was called directly by the user via ConnectAsync, so just throw the exception.
                        throw retryException;
                    }
                }

                // With all the above conditions checked, the client should attempt to connect again after a delay
                try
                {
                    if (retryDelay.CompareTo(TimeSpan.Zero) > 0)
                    {
                        Trace.TraceInformation("Waiting {0} before next reconnect attempt", retryDelay);
                        await Task.Delay(retryDelay, cancellationToken);
                    }

                    cancellationToken.ThrowIfCancellationRequested();
                    Trace.TraceInformation($"Trying to connect. Attempt number {attemptCount}");

                    using CancellationTokenSource reconnectionTimeoutCancellationToken = new();
                    reconnectionTimeoutCancellationToken.CancelAfter(_connectionAttemptTimeout);
                    using CancellationTokenSource linkedCancellationToken = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, reconnectionTimeoutCancellationToken.Token);
                    mostRecentConnectResult = await TryEstablishConnectionAsync(options, linkedCancellationToken.Token).ConfigureAwait(false);

                    if (isReconnection)
                    {
                        Trace.TraceInformation("Reconnection finished after successfully connecting to the MQTT broker again.");
                    }

                    return mostRecentConnectResult;
                }
                catch (Exception) when (_isClosing && isReconnection)
                {
                    // This happens when reconnecting if the user attempts to manually disconnect the session client. When
                    // that happens, we simply want to end the reconnection logic and let the thread end without throwing.
                    Trace.TraceInformation("MQTT reconnection cancelled because the client is being closed.");
                    return null;
                }
                catch (Exception e)
                {
                    lastException = e;
                    Trace.TraceWarning($"Encountered an exception while connecting. May attempt to reconnect. {e}");
                }

                attemptCount++;
            }
        }

        private async Task<MqttConnectAck?> TryEstablishConnectionAsync(MqttConnect options, CancellationToken cancellationToken)
        {
            if (_underlyingMqttClient.IsConnected())
            {
                return null;
            }

            MqttConnectAck? connectResult = await _underlyingMqttClient.ConnectAsync(options, cancellationToken).ConfigureAwait(false);

            if (connectResult.ResultCode != MqttConnectResultCode.Success)
            {
                throw new MqttConnectingFailedException($"Client tried to connect but server denied connection with reason '{connectResult.ResultCode}'.", connectResult);
            }

            return connectResult;
        }

        private async Task EndConnectionMaintanceAsync(Exception queuedItemException, MqttClientDisconnectedEventArgs disconnectedEventArgs, CancellationToken cancellationToken)
        {
            _isDesiredConnected = false; //TODO need to actually notify ConnectionClient that a fatal disconnect happened so it can notify the user
        }

        // These reason codes are fatal if the broker sends a DISCONNECT packet with this reason.
        private static bool IsFatal(MqttDisconnectReason code)
        {
            switch (code)
            {
                case MqttDisconnectReason.MalformedPacket:
                case MqttDisconnectReason.ProtocolError:
                case MqttDisconnectReason.NotAuthorized:
                case MqttDisconnectReason.SessionTakenOver:
                case MqttDisconnectReason.TopicFilterInvalid:
                case MqttDisconnectReason.TopicNameInvalid:
                case MqttDisconnectReason.TopicAliasInvalid:
                case MqttDisconnectReason.PacketTooLarge:
                case MqttDisconnectReason.PayloadFormatInvalid:
                case MqttDisconnectReason.RetainNotSupported:
                case MqttDisconnectReason.QosNotSupported:
                case MqttDisconnectReason.ServerMoved:
                case MqttDisconnectReason.SharedSubscriptionsNotSupported:
                case MqttDisconnectReason.SubscriptionIdentifiersNotSupported:
                case MqttDisconnectReason.WildcardSubscriptionsNotSupported:
                    return true;
            }

            return false;
        }

        private static bool IsFatal(Exception e, bool userCancellationRequested = false)
        {
            if (e is MqttConnectingFailedException)
            {
                MqttConnectResultCode code = ((MqttConnectingFailedException)e).ResultCode;

                switch (code)
                {
                    case MqttConnectResultCode.MalformedPacket:
                    case MqttConnectResultCode.ProtocolError:
                    case MqttConnectResultCode.UnsupportedProtocolVersion:
                    case MqttConnectResultCode.ClientIdentifierNotValid:
                    case MqttConnectResultCode.BadUserNameOrPassword:
                    case MqttConnectResultCode.Banned:
                    case MqttConnectResultCode.BadAuthenticationMethod:
                    case MqttConnectResultCode.TopicNameInvalid:
                    case MqttConnectResultCode.PacketTooLarge:
                    case MqttConnectResultCode.PayloadFormatInvalid:
                    case MqttConnectResultCode.RetainNotSupported:
                    case MqttConnectResultCode.QoSNotSupported:
                    case MqttConnectResultCode.ServerMoved:
                    case MqttConnectResultCode.ImplementationSpecificError:
                    case MqttConnectResultCode.UseAnotherServer:
                    case MqttConnectResultCode.NotAuthorized:
                        return true;
                }
            }

            if (e is SocketException)
            {
                //TODO there is room for a lot more nuance here. Some socket exceptions are more retryable than others so it may
                // be inappropriate to label them all as fatal.
                return true;
            }

            if (e is MQTTnet.Exceptions.MqttProtocolViolationException)
            {
                return true;
            }

            if (e is ArgumentException
                || e is ArgumentNullException
                || e is NotSupportedException)
            {
                return true;
            }

            // MQTTnet may throw an OperationCanceledException/TaskCanceledException even if
            // neither the user nor the session client provides a cancellation token. Because
            // of that, this exception is only fatal if the cancellation token this layer
            // is aware of actually requested cancellation. Other cases signify that MQTTnet
            // gave up on the operation, but the user still wants to retry.
            if ((e is OperationCanceledException || e is TaskCanceledException)
                && userCancellationRequested)
            {
                return true;
            }

            return false;
        }

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            return _underlyingMqttClient.PublishAsync(publish, cancellationToken);
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            return _underlyingMqttClient.SubscribeAsync(subscribe, cancellationToken);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            return _underlyingMqttClient.UnsubscribeAsync(unsubscribe, cancellationToken);
        }
    }
}
