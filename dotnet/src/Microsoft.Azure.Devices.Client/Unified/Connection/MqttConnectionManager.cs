// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Unified.Connection.Retry;
using System.Diagnostics;
using System.Net.Sockets;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    internal class MqttConnectionManager
    {
        private readonly MqttSessionClientOptions _sessionClientOptions;
        private MqttConnect? _mostRecentConnect;
        private readonly bool _isDisposed = false;

        private bool _isDesiredConnected;
        private bool _isClosing;
        private CancellationTokenSource? _reconnectionCancellationToken;

        private readonly SemaphoreSlim _disconnectedEventLock = new(1);

        private IMqttClient _underlyingMqttClient;

        /// <summary>
        /// Create a MQTT session client where the underlying MQTT client is created for you and the connection is maintained
        /// for you.
        /// </summary>
        /// <remarks>
        /// <para>
        /// When an MQTT session client is constructed with this constructor, it will automatically recover the connection
        /// and all previous subscriptions if it detects that the previous connection was lost.
        /// It will also enqueue publishes/subscribes/unsubscribes and send them when the connection is alive.
        /// </para>
        /// <para>
        /// An MQTT session client created with this constructor will only report connection loss and/or publish/subscribe/unsubscribe
        /// failures if they are deemed fatal or if the provided retry policy is exhausted. All transient failures will cause the
        /// retry policy to be checked, but won't cause the <see cref="DisconnectedAsync"/> event to fire.
        /// </para>
        /// </remarks>
        /// <param name="connectionSettings">The configurable options for the underlying MQTT connection(s)</param>
        /// <param name="sessionClientOptions">The configurable options for this MQTT session client.</param>
        public MqttConnectionManager(IMqttClient underlyingMqttClient, MqttSessionClientOptions? sessionClientOptions = null)
        {
            _underlyingMqttClient = underlyingMqttClient;
            _sessionClientOptions = sessionClientOptions ?? new MqttSessionClientOptions();
            _sessionClientOptions.Validate();

            _underlyingMqttClient.DisconnectedAsync += InternalDisconnectedAsync;
        }

        /// <summary>
        /// Connect this client and start a clean MQTT session. Once connected, this client will automatically reconnect
        /// as needed and recover the MQTT session.
        /// </summary>
        /// <param name="options">The details about how to connect to the MQTT broker.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The CONNACK received from the MQTT broker.</returns>
        /// <remarks>
        /// This operation does not retry by default, but can be configured to retry. To do so, set the 
        /// <see cref="MqttSessionClientOptions.RetryOnFirstConnect"/> flag and optionally configure the retry policy
        /// via <see cref="MqttSessionClientOptions.ConnectionRetryPolicy"/>.
        /// </remarks>
        /// <exception cref="InvalidOperationException">If this method is called when the client is already managing the connection.</exception>
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
            Trace.TraceInformation("Successfully connected the session client to the MQTT broker. This connection will now be maintained.");

            return connectResult;
        }

        /// <summary>
        /// Disconnect this client and end the MQTT session.
        /// </summary>
        /// <param name="options">The optional parameters that can be sent in the DISCONNECT packet to the MQTT broker.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(bool desireReconnection, MqttDisconnect? options = null, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            if (options != null && options.SessionExpiryInterval != 0)
            {
                // This method should only be called when the session is no longer needed. By providing a non-zero value, you are trying
                // to keep the session alive on the broker.
                throw new ArgumentException("Cannot use a non-zero session expiry interval");
            }

            options ??= new MqttDisconnect();
            options.SessionExpiryInterval = 0;

            _isClosing = true;
            _reconnectionCancellationToken?.Cancel();
            await _underlyingMqttClient.DisconnectAsync(options, cancellationToken);

            var disconnectedArgs = new MqttClientDisconnectedEventArgs()
            {
                Reason = MqttDisconnectReason.NormalDisconnection,
                ReasonString = "User closed the connection manually"
            };

            await FinalizeSessionAsync(new OperationCanceledException("This operation was canceled because the MQTT client was closed."), disconnectedArgs, cancellationToken);
            Trace.TraceInformation("Successfully disconnected the session client from the MQTT broker. This connection will no longer be maintained.");
        }

        public new void Dispose()
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
                        Trace.TraceInformation("Disconnect reported by underlying MQTT client, but it was already handled");
                        return;
                    }

                    if (IsFatal(args.Reason))
                    {
                        Trace.TraceInformation("Disconnect detected and it was due to fatal error. The client will not attempt to reconnect. Disconnect reason: {0}", args.Reason);
                        var retryException = new RetryExpiredException("A fatal error was encountered while trying to re-establish the session, so this request cannot be completed.", args.Exception);
                        await FinalizeSessionAsync(retryException, args, CancellationToken.None);
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
                        await FinalizeSessionAsync(retryException, lastDisconnect!, cancellationToken);
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
                    && !_sessionClientOptions.ConnectionRetryPolicy.ShouldRetry(attemptCount, lastException!, out retryDelay))
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

                        await FinalizeSessionAsync(retryException, disconnectedEventArgs, cancellationToken);
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
                    reconnectionTimeoutCancellationToken.CancelAfter(_sessionClientOptions.ConnectionAttemptTimeout);
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
                    Trace.TraceInformation("Session client reconnection cancelled because the client is being closed.");
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

        private async Task FinalizeSessionAsync(Exception queuedItemException, MqttClientDisconnectedEventArgs disconnectedEventArgs, CancellationToken cancellationToken)
        {
            _isDesiredConnected = false;
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
    }
}
