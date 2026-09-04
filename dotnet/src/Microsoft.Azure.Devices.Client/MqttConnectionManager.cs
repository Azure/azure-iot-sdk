// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Exceptions;
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

                    DeviceException? disconnectException = IsFatal(args.Reason);
                    if (disconnectException is { Retryability: not ErrorRetryability.Retryable })
                    {
                        var retryException = new RetryExpiredException("A fatal error was encountered while trying to re-establish the session, so this request cannot be completed.", disconnectException);
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

                DeviceException? deviceException = IsFatal(
                    lastException,
                    _reconnectionCancellationToken?.Token.IsCancellationRequested ?? cancellationToken.IsCancellationRequested);

                if (deviceException != null)
                {
                    lastException = deviceException;
                }

                if (deviceException is { Retryability: not ErrorRetryability.Retryable })
                {
                    Trace.TraceError("Encountered a fatal exception while maintaining connection {0}", deviceException);
                    if (isReconnection)
                    {
                        var retryException = new RetryExpiredException("A fatal error was encountered while trying to re-establish the session, so this request cannot be completed.", deviceException);

                        // This function was called to reconnect after an unexpected disconnect. Since the error is fatal,
                        // notify the user via callback that the client has crashed, but don't throw the exception since
                        // this task is unmonitored.
                        await EndConnectionMaintanceAsync(retryException, lastDisconnect!, cancellationToken);
                        return null;
                    }
                    else
                    {
                        // This function was called directly by the user via ConnectAsync, so just throw the exception.
                        throw deviceException;
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

            if (connectResult.ResultCode != MqttConnectReasonCode.Success)
            {
                throw new MqttConnectingFailedException($"Client tried to connect but server denied connection with reason '{connectResult.ResultCode}'.", connectResult);
            }

            return connectResult;
        }

        private async Task EndConnectionMaintanceAsync(Exception queuedItemException, MqttClientDisconnectedEventArgs disconnectedEventArgs, CancellationToken cancellationToken)
        {
            _isDesiredConnected = false; //TODO need to actually notify ConnectionClient that a fatal disconnect happened so it can notify the user
        }

        /// <summary>
        /// Classify a server DISCONNECT reason code per connection.md section 9.3.7.
        /// </summary>
        /// <param name="code">The reason code carried by the server's DISCONNECT packet.</param>
        /// <returns>Null if the code is benign, otherwise an exception whose retryability describes the code.</returns>
        private static DeviceException? IsFatal(MqttDisconnectReason code)
        {
            ErrorRetryability retryability;

            switch (code)
            {
                // Benign: an orderly close. Reconnect under the usual policy.
                case MqttDisconnectReason.NormalDisconnection:
                case MqttDisconnectReason.DisconnectWithWillMessage:
                    return null;

                case MqttDisconnectReason.UnspecifiedError:
                case MqttDisconnectReason.ImplementationSpecificError:
                case MqttDisconnectReason.ServerBusy:
                case MqttDisconnectReason.ServerShuttingDown:
                case MqttDisconnectReason.KeepAliveTimeout:
                case MqttDisconnectReason.MessageRateTooHigh:
                case MqttDisconnectReason.QuotaExceeded:
                case MqttDisconnectReason.ConnectionRateExceeded:
                case MqttDisconnectReason.MaximumConnectTime:
                    retryability = ErrorRetryability.Retryable;
                    break;

                // Authorization was revoked mid-session, or what the device presented is no longer acceptable.
                case MqttDisconnectReason.NotAuthorized:
                case MqttDisconnectReason.BadAuthenticationMethod:
                    retryability = ErrorRetryability.IdentityTerminal;
                    break;

                case MqttDisconnectReason.MalformedPacket:
                case MqttDisconnectReason.ProtocolError:
                case MqttDisconnectReason.SessionTakenOver:
                case MqttDisconnectReason.TopicFilterInvalid:
                case MqttDisconnectReason.TopicNameInvalid:
                case MqttDisconnectReason.ReceiveMaximumExceeded:
                case MqttDisconnectReason.TopicAliasInvalid:
                case MqttDisconnectReason.PacketTooLarge:
                case MqttDisconnectReason.AdministrativeAction:
                case MqttDisconnectReason.PayloadFormatInvalid:
                case MqttDisconnectReason.RetainNotSupported:
                case MqttDisconnectReason.QosNotSupported:
                case MqttDisconnectReason.SharedSubscriptionsNotSupported:
                case MqttDisconnectReason.SubscriptionIdentifiersNotSupported:
                case MqttDisconnectReason.WildcardSubscriptionsNotSupported:
                // "Terminal at this endpoint" has no distinct retryability value, so it is reported as terminal
                // rather than being softened into a retry that would repeat the redirection forever.
                case MqttDisconnectReason.UseAnotherServer:
                case MqttDisconnectReason.ServerMoved:
                    retryability = ErrorRetryability.Terminal;
                    break;

                // Unrecognised. Preserve the value and take the conservative branch.
                default:
                    retryability = ErrorRetryability.Retryable;
                    break;
            }

            return new DeviceException($"The server closed the connection with reason '{code}'.")
            {
                Retryability = retryability,
                IsContained = false,
            };
        }

        /// <summary>
        /// Classify a CONNACK reason code per connection.md section 9.3.3.
        /// </summary>
        /// <param name="code">The reason code carried by the server's CONNACK packet.</param>
        /// <returns>Null if the code is benign, otherwise an exception whose retryability describes the code.</returns>
        private static DeviceException? IsFatal(MqttConnectReasonCode code)
        {
            ErrorRetryability retryability;

            switch (code)
            {
                case MqttConnectReasonCode.Success:
                    return null;

                case MqttConnectReasonCode.UnspecifiedError:
                case MqttConnectReasonCode.ImplementationSpecificError:
                case MqttConnectReasonCode.ServerUnavailable:
                case MqttConnectReasonCode.ServerBusy:
                case MqttConnectReasonCode.QuotaExceeded:
                case MqttConnectReasonCode.ConnectionRateExceeded:
                    retryability = ErrorRetryability.Retryable;
                    break;

                // What the device presented is not acceptable. A DPS-provisioned device re-provisions from here.
                case MqttConnectReasonCode.ClientIdentifierNotValid:
                case MqttConnectReasonCode.BadUserNameOrPassword:
                case MqttConnectReasonCode.NotAuthorized:
                case MqttConnectReasonCode.BadAuthenticationMethod:
                    retryability = ErrorRetryability.IdentityTerminal;
                    break;

                case MqttConnectReasonCode.MalformedPacket:
                case MqttConnectReasonCode.ProtocolError:
                case MqttConnectReasonCode.UnsupportedProtocolVersion:
                case MqttConnectReasonCode.Banned:
                case MqttConnectReasonCode.TopicNameInvalid:
                case MqttConnectReasonCode.PacketTooLarge:
                case MqttConnectReasonCode.PayloadFormatInvalid:
                case MqttConnectReasonCode.RetainNotSupported:
                case MqttConnectReasonCode.QoSNotSupported:
                // "Terminal at this endpoint" has no distinct retryability value, so it is reported as terminal
                // rather than being softened into a retry that would repeat the redirection forever.
                case MqttConnectReasonCode.UseAnotherServer:
                case MqttConnectReasonCode.ServerMoved:
                    retryability = ErrorRetryability.Terminal;
                    break;

                // Unrecognised. Preserve the value and take the conservative branch.
                default:
                    retryability = ErrorRetryability.Retryable;
                    break;
            }

            return new DeviceException($"The server refused the connection with reason '{code}'.")
            {
                Retryability = retryability,
                IsContained = false,
            };
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            try
            {
                var puback = await _underlyingMqttClient.PublishAsync(publish, cancellationToken);
                ThrowIfPubackHasErrorCode(puback);
                return puback;
            }
            catch (Exception e)
            {
                throw new DeviceException("TODO", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = true,
                };
            }
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            try
            {
                var suback = await _underlyingMqttClient.SubscribeAsync(subscribe, cancellationToken);
                ThrowIfSubackHasErrorCode(suback);
                return suback;
            }
            catch (Exception e)
            {
                throw new DeviceException("TODO", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = true,
                };
            }
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            try
            {
                var unsuback = await _underlyingMqttClient.UnsubscribeAsync(unsubscribe, cancellationToken);
                ThrowIfUnsubackHasErrorCode(unsuback);
                return unsuback;
            }
            catch (Exception e)
            {
                throw new DeviceException("TODO", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = true,
                };
            }
        }

        public void ThrowIfPubackHasErrorCode(MqttPublishAck puback)
        {
            switch (puback.ReasonCode)
            {
                case MqttPublishAckReasonCode.UnspecifiedError:
                case MqttPublishAckReasonCode.ImplementationSpecificError:
                case MqttPublishAckReasonCode.QuotaExceeded:
                    throw new DeviceException($"Received MQTT puback code {puback.ReasonCode}")
                    {
                        Retryability = ErrorRetryability.Retryable,
                        IsContained = true,
                    };

                case MqttPublishAckReasonCode.NotAuthorized:
                case MqttPublishAckReasonCode.PacketIdentifierInUse:
                case MqttPublishAckReasonCode.TopicNameInvalid:
                case MqttPublishAckReasonCode.PayloadFormatInvalid:
                    throw new DeviceException($"Received MQTT puback code {puback.ReasonCode}")
                    {
                        Retryability = ErrorRetryability.Terminal,
                        IsContained = true,
                    };

                default:
                    return; // benign case, do not throw any exception
            }
        }

        public void ThrowIfSubackHasErrorCode(MqttSubscribeAck suback)
        {
            foreach (var subackItem in suback.Items)
            {
                switch (subackItem.ReasonCode)
                {
                    case MqttClientSubscribeReasonCode.UnspecifiedError:
                    case MqttClientSubscribeReasonCode.ImplementationSpecificError:
                    case MqttClientSubscribeReasonCode.QuotaExceeded:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Retryable,
                            IsContained = true,
                        };

                    case MqttClientSubscribeReasonCode.NotAuthorized:
                    case MqttClientSubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientSubscribeReasonCode.SharedSubscriptionsNotSupported:
                    case MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = true,
                        };

                    case MqttClientSubscribeReasonCode.PacketIdentifierInUse:
                    case MqttClientSubscribeReasonCode.SubscriptionIdentifiersNotSupported:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = false,
                        };

                    default:
                        return; // benign case, do not throw any exception
                }
            }
        }

        public void ThrowIfUnsubackHasErrorCode(MqttUnsubscribeAck unsuback)
        {
            foreach (var unsubackItem in unsuback.Items)
            {
                switch (unsubackItem.ReasonCode)
                {
                    case MqttClientUnsubscribeReasonCode.UnspecifiedError:
                    case MqttClientUnsubscribeReasonCode.ImplementationSpecificError:
                        throw new DeviceException($"Received MQTT unsuback code {unsubackItem.ReasonCode} when subscribing to topic {unsubackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Retryable,
                            IsContained = true,
                        };

                    case MqttClientUnsubscribeReasonCode.NotAuthorized:
                    case MqttClientUnsubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientUnsubscribeReasonCode.PacketIdentifierInUse:
                        throw new DeviceException($"Received MQTT unsuback code {unsubackItem.ReasonCode} when subscribing to topic {unsubackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = true,
                        };

                    default:
                        return; // benign case, do not throw any exception
                }
            }
        }
    }
}
