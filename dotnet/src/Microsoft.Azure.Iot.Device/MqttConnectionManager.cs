// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using System.Diagnostics;
using System.Net.Sockets;
using System.Security.Authentication;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// Wraps a plain MQTT client with connection maintenance: it establishes the session, keeps it established
    /// across unexpected disconnects, and applies the caller-supplied retry policy between attempts.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Every connection-level error is classified per <c>connection.md</c> section 9.3 into a
    /// <see cref="DeviceException"/> carrying an <see cref="ErrorRetryability"/>.
    /// </para>
    /// <para>
    /// Retryable errors never leave this layer: they are logged, delayed per <see cref="IRetryPolicy"/>, and retried
    /// indefinitely. Terminal and identity-terminal errors end connection maintenance and are reported to the owning
    /// connection client through <see cref="ConnectionFaultedAsync"/> — and, when the error happened inside the
    /// caller's own <see cref="ConnectAsync"/> call, thrown to that caller as well.
    /// </para>
    /// <para>
    /// Per-operation (contained) errors from PUBLISH, SUBSCRIBE and UNSUBSCRIBE are not connection-level errors and
    /// are thrown to the caller of those methods rather than being retried or reported here.
    /// </para>
    /// <para>
    /// A QoS 1 PUBLISH, SUBSCRIBE or UNSUBSCRIBE to IoT hub whose connection is lost before its PUBACK, SUBACK or
    /// UNSUBACK arrives is not a contained error: the acknowledgement can never arrive, so it is reported to the caller as
    /// <see cref="MqttClientNotConnectedException"/>, which tells the caller to send it again once reconnected.
    /// </para>
    /// </remarks>
    internal class MqttConnectionManager : IDisposable
    {
        private readonly AsyncEvent<MqttPublishReceivedEventArgs> _publishReceived = new();
        private readonly AsyncEvent<MqttClientConnectedEventArgs> _connected = new();
        private readonly AsyncPipelineEvent<MqttConnect> _connecting = new();
        private readonly AsyncEvent<MqttClientDisconnectedEventArgs> _disconnected = new();
        private readonly AsyncEvent<MqttConnectionFaultedEventArgs> _connectionFaulted = new();

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync
        {
            add => _publishReceived.Add(value);
            remove => _publishReceived.Remove(value);
        }

        public event Func<MqttClientConnectedEventArgs, Task>? ConnectedAsync
        {
            add => _connected.Add(value);
            remove => _connected.Remove(value);
        }

        /// <remarks>
        /// Handlers are chained in the order they were added, each receiving the CONNECT packet the previous one returned.
        /// </remarks>
        public event Func<MqttConnect, CancellationToken, Task<MqttConnect>>? ConnectingAsync
        {
            add => _connecting.Add(value);
            remove => _connecting.Remove(value);
        }

        /// <summary>
        /// Raised exactly once for each connection that <see cref="ConnectedAsync"/> reported, once that connection has
        /// been lost.
        /// </summary>
        /// <remarks>
        /// The underlying client's own disconnect callback is not trusted as is: it may be raised for a connect attempt
        /// that never connected, more than once for one disconnection, or late, after a newer connection is already up.
        /// Such callbacks are filtered out, so this is never raised spuriously. It is always raised for a connection
        /// before <see cref="ConnectedAsync"/> is raised for the next one, and before this layer starts reconnecting, so
        /// that handlers can abandon work that belonged to the lost connection without affecting the next one.
        /// </remarks>
        public event Func<MqttClientDisconnectedEventArgs, Task>? DisconnectedAsync
        {
            add => _disconnected.Add(value);
            remove => _disconnected.Remove(value);
        }

        /// <summary>
        /// Raised when this layer stops maintaining the connection because it hit a terminal or identity-terminal
        /// connection-level error. Retryable errors are handled internally and never raise this event.
        /// </summary>
        public event Func<MqttConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync
        {
            add => _connectionFaulted.Add(value);
            remove => _connectionFaulted.Remove(value);
        }

        private readonly IRetryPolicy _connectionRetryPolicy;

        private readonly TimeSpan _connectionAttemptTimeout;

        /// <summary>
        /// Whether the device whose connection this layer maintains was provisioned through Device Provisioning
        /// Service and therefore has a registration it can renew. This gates whether a
        /// <see cref="RetryGuidance.Reprovision"/> from the retry policy actually crosses over to
        /// re-provisioning: a device that holds no provisioning inputs has nothing to re-provision from, so the
        /// guidance is treated as an ordinary retry and the hub keeps being retried.
        /// </summary>
        /// <remarks>
        /// The owning connection client sets this when it connects to an IoT hub. The number of hub attempts after
        /// which re-provisioning is advised lives in the retry policy (for example
        /// <see cref="ExponentialBackoffRetryPolicy"/>), not here.
        /// </remarks>
        public bool CanReprovision { get; set; }

        /// <summary>
        /// Asks the owning connection client to switch to its other set of credentials (primary or secondary) for the
        /// current endpoint after the server rejected the current ones as an identity failure.
        /// </summary>
        /// <remarks>
        /// Returns false when there is nothing to switch to. Each set of credentials is tried at most once between
        /// successful connects, so an identity failure only ends connection maintenance once both have been rejected.
        /// The next CONNECT picks up the switched credentials when the owning client patches it.
        /// </remarks>
        public Func<CancellationToken, Task<bool>>? TrySwitchCredentialsAsync { get; set; }

        // Set once the credentials have been switched after an identity failure, and cleared by a successful connect.
        private volatile bool _credentialsSwitchedSinceLastConnect;

        /// <summary>
        /// The endpoint the current connection targets, surfaced to the retry policy so it can tell whether it is being
        /// asked to retry connecting to an IoT hub or to Device Provisioning Service.
        /// </summary>
        /// <remarks>
        /// The owning connection client changes this through <see cref="SwitchEndpointAsync"/>, to
        /// <see cref="ConnectionEndpoint.IotHub"/> for hub connections and
        /// <see cref="ConnectionEndpoint.DeviceProvisioningService"/> for provisioning connections, alongside
        /// <see cref="CanReprovision"/>. Every PUBLISH, SUBSCRIBE and UNSUBSCRIBE names the endpoint it is meant for and
        /// is only sent while this is that endpoint.
        /// </remarks>
        public ConnectionEndpoint ConnectionEndpoint
        {
            get
            {
                lock (_endpointLock)
                {
                    return _connectionEndpoint;
                }
            }
        }

        // Guards _connectionEndpoint, _inFlightOperationCounts and _staleOperationsDrained so that admitting an operation
        // for an endpoint and switching away from that endpoint are serialized with each other.
        private readonly object _endpointLock = new();
        private ConnectionEndpoint _connectionEndpoint = ConnectionEndpoint.None;

        // The number of PUBLISH/SUBSCRIBE/UNSUBSCRIBE operations currently handed to the underlying client, indexed by
        // the endpoint each one was admitted for.
        private readonly int[] _inFlightOperationCounts = new int[Enum.GetValues<ConnectionEndpoint>().Length];

        // Completed once no operation admitted for an endpoint other than the current one is still in flight. Only set
        // while an endpoint switch is waiting for that.
        private TaskCompletionSource? _staleOperationsDrained;

        /// <summary>
        /// Whether this layer is in a connection lifecycle that could still release an operation waiting for the
        /// connection to come back: it is maintaining or (re-)establishing a connection, or it is delivering a terminal
        /// fault that unblocks such waiters. This is <c>false</c> only when the layer is deliberately idle -- it has
        /// never connected, or it was deliberately disconnected (via <see cref="DisconnectAsync"/>) or disposed -- so no
        /// reconnection and no fault notification is coming.
        /// </summary>
        /// <remarks>
        /// The owning connection client uses this to decide whether a feature operation that finds the connection gone
        /// should keep waiting for a reconnection or fail fast. Unlike <see cref="_isDesiredConnected"/> alone, this
        /// stays true across the brief window in which a terminal fault has stopped the connection but has not yet been
        /// delivered to the owning client, so such a waiter is released by the real terminal fault rather than being
        /// failed with a bare not-connected error.
        /// </remarks>
        public bool IsConnectionLifecycleActive => _isDesiredConnected || (!_isClosing && _mostRecentConnect != null);

        internal CancellationToken ConnectionLifetimeCancellationToken
        {
            get
            {
                lock (_connectionLifetimeCancellationTokenLock)
                {
                    return _connectionLifetimeCancellationToken.Token;
                }
            }
        }

        private MqttConnect? _mostRecentConnect;
        private bool _isDisposed;

        private bool _isDesiredConnected;
        private bool _isClosing;

        // Cancels whatever connection maintenance is currently in flight, whether that is the initial connect's retry
        // loop or a reconnection's. DisconnectAsync(desireReconnection: false) cancels this to abandon retrying.
        private CancellationTokenSource? _reconnectionCancellationToken;

        // Cancels operations waiting for this connection to return when the user deliberately disconnects.
        private CancellationTokenSource _connectionLifetimeCancellationToken = new();
        private readonly object _connectionLifetimeCancellationTokenLock = new();

        private readonly SemaphoreSlim _disconnectedEventLock = new(1);

        // Incremented each time a connection is lost, so that an operation can tell whether the connection it was sent
        // over was lost before its acknowledgement arrived.
        private long _disconnectionCount;

        // Guards _isConnectionUp and _connectionLossNotification, so that each connection that came up is reported as
        // lost exactly once.
        private readonly object _connectionStateLock = new();

        // Whether a connection has been reported through ConnectedAsync and not yet through DisconnectedAsync.
        private bool _isConnectionUp;

        // Completes once the most recent connection loss has been reported through DisconnectedAsync. The next
        // connection is only reported through ConnectedAsync after that.
        private Task _connectionLossNotification = Task.CompletedTask;

        private readonly IMqttClient _underlyingMqttClient;

        public MqttConnectionManager(IMqttClient underlyingMqttClient, TimeSpan connectionAttemptTimeout, IRetryPolicy retryPolicy)
        {
            _underlyingMqttClient = underlyingMqttClient;
            _connectionAttemptTimeout = connectionAttemptTimeout;
            _connectionRetryPolicy = retryPolicy;

            _underlyingMqttClient.DisconnectedAsync += HandleUnderlyingDisconnectedAsync;
            _underlyingMqttClient.ConnectingAsync += DelegateConnectingAsync;
            _underlyingMqttClient.ConnectedAsync += HandleUnderlyingConnectedAsync;
            _underlyingMqttClient.PublishReceivedAsync += DelegatePublishReceivedAsync;
        }

        private async Task DelegatePublishReceivedAsync(MqttPublishReceivedEventArgs args)
        {
            try
            {
                await _publishReceived.InvokeAsync(args);
            }
            catch (Exception e)
            {
                Trace.TraceError("The publish handler threw while being notified of a publish. {0}", e);
            }
        }

        private async Task HandleUnderlyingConnectedAsync(MqttClientConnectedEventArgs args)
        {
            TaskCompletionSource? lateLossNotified = null;
            Task previousLossNotification;
            lock (_connectionStateLock)
            {
                previousLossNotification = _connectionLossNotification;

                if (_isConnectionUp)
                {
                    // The previous connection's disconnect callback has not arrived yet, so report its loss now, before
                    // this connection. The late callback will then find this connection up and be dropped.
                    lateLossNotified = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                    _connectionLossNotification = lateLossNotified.Task;
                }

                _isConnectionUp = true;
            }

            // Handlers of the previous connection's loss must finish before handlers of this connection start, so that
            // abandoning the previous connection's work cannot affect this one's.
            await previousLossNotification;

            if (lateLossNotified != null)
            {
                try
                {
                    await NotifyConnectionLostAsync(new MqttClientDisconnectedEventArgs()
                    {
                        Reason = MqttDisconnectReason.UnspecifiedError,
                        ReasonString = "The previous connection was lost before this connection was established.",
                    });
                }
                finally
                {
                    lateLossNotified.SetResult();
                }
            }

            await DelegateConnectedAsync(args);
        }

        /// <summary>
        /// Report a connection loss from the underlying client, unless no connection that came up was actually lost.
        /// </summary>
        /// <remarks>
        /// The underlying client may raise its disconnect callback for a connect attempt that never connected, more than
        /// once for one disconnection, or late, after a newer connection is already up. Only the first callback after a
        /// connection came up, and while the underlying client is not connected, is a connection loss.
        /// </remarks>
        private async Task HandleUnderlyingDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            var lossNotified = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            Task previousLossNotification;
            lock (_connectionStateLock)
            {
                if (!_isConnectionUp || _underlyingMqttClient.IsConnected())
                {
                    Trace.TraceInformation("Ignoring a disconnect callback that does not correspond to a lost connection. Disconnect reason: {0}", args.Reason);
                    return;
                }

                _isConnectionUp = false;
                previousLossNotification = _connectionLossNotification;
                _connectionLossNotification = lossNotified.Task;
            }

            try
            {
                await previousLossNotification;
                await NotifyConnectionLostAsync(args);
            }
            finally
            {
                lossNotified.SetResult();
            }

            // Only start reconnecting once handlers have abandoned the lost connection's work. Reconnection is not awaited
            // because it may retry for as long as the retry policy allows, and the underlying client's callback must not
            // be held up for that long.
            _ = InternalDisconnectedAsync(args);
        }

        private async Task NotifyConnectionLostAsync(MqttClientDisconnectedEventArgs args)
        {
            Interlocked.Increment(ref _disconnectionCount);
            await DelegateDisconnectedAsync(args);
        }

        private async Task DelegateConnectedAsync(MqttClientConnectedEventArgs args)
        {
            try
            {
                await _connected.InvokeAsync(args);
            }
            catch (Exception e)
            {
                Trace.TraceError("The connected handler threw while being notified of a connection. {0}", e);
            }
        }

        private Task<MqttConnect> DelegateConnectingAsync(MqttConnect connect, CancellationToken cancellationToken)
        {
            return _connecting.InvokeAsync(connect, cancellationToken);
        }

        private async Task DelegateDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            try
            {
                await _disconnected.InvokeAsync(args);
            }
            catch (Exception e)
            {
                Trace.TraceError("The disconnected handler threw while being notified of a disconnection. {0}", e);
            }
        }

        /// <summary>
        /// Establish the session and begin maintaining it.
        /// </summary>
        /// <remarks>
        /// Retryable failures are absorbed here and retried under the retry policy, so this call only returns once the
        /// session is established. It throws only when the failure is terminal, identity-terminal, the retry policy is
        /// exhausted, or the caller cancels.
        /// </remarks>
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

            // Hold the disconnected-event lock for the whole initial connect. Each failed attempt raises the
            // "Disconnected" callback, which would otherwise start a second retry loop alongside this one. Queued
            // callbacks re-check their guards once this method completes and stand down (or reconnect, if the
            // connection dropped right after being established).
            await _disconnectedEventLock.WaitAsync(cancellationToken);

            try
            {
                if (_isDesiredConnected)
                {
                    throw new InvalidOperationException("The client is already managing the connection.");
                }

                return await ConnectWhileHoldingEventLockAsync(connect, cancellationToken);
            }
            finally
            {
                try
                {
                    _disconnectedEventLock.Release();
                }
                catch (ObjectDisposedException)
                {
                    // Dispose raced with this connect; there is nothing left to release.
                }
            }
        }

        private async Task<MqttConnectAck> ConnectWhileHoldingEventLockAsync(MqttConnect connect, CancellationToken cancellationToken)
        {
            _mostRecentConnect = connect;
            _isClosing = false;
            _credentialsSwitchedSinceLastConnect = false;

            lock (_connectionLifetimeCancellationTokenLock)
            {
                CancellationTokenSource previousConnectionLifetimeCancellationToken = _connectionLifetimeCancellationToken;
                _connectionLifetimeCancellationToken = new CancellationTokenSource();
                previousConnectionLifetimeCancellationToken.Cancel();
                previousConnectionLifetimeCancellationToken.Dispose();
            }

            // Link the caller's token with this layer's own token so that DisconnectAsync(desireReconnection: false)
            // can abandon the initial connect's retry loop just as it abandons a reconnection's.
            _reconnectionCancellationToken?.Dispose();
            _reconnectionCancellationToken = new CancellationTokenSource();
            using CancellationTokenSource linkedCancellationToken =
                CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _reconnectionCancellationToken.Token);

            // Mark the connection as one this layer should keep alive before the first connect attempt begins. The
            // device presence flow runs fire-and-forget as soon as the broker accepts the CONNECT, so it can fail and
            // disconnect before this method returns. Setting this here ensures the "Disconnected" callback reconnects
            // in that case rather than standing down because the initial connect had not yet been marked as desired.
            _isDesiredConnected = true;

            MqttConnectAck? connectResult;
            try
            {
                connectResult = await MaintainConnectionAsync(connect, null, linkedCancellationToken.Token);
            }
            catch
            {
                // The initial connect ultimately failed, so this layer is no longer maintaining a connection. Terminal
                // failures already clear this flag via EndConnectionMaintenanceAsync, but resetting here also covers
                // cancellation and any other exception that bypasses that path.
                _isDesiredConnected = false;
                throw;
            }

            // By design, MaintainConnectionAsync should only return null when called during reconnection.
            // When called by this method, MaintainConnectionAsync should return a non-null value or throw.
            Debug.Assert(connectResult != null);

            return connectResult;
        }

        /// <summary>
        /// Send a DISCONNECT to the server.
        /// </summary>
        /// <param name="desireReconnection">
        /// When true, this is a deliberate session reset: the connection is torn down but this layer keeps maintaining
        /// it and will reconnect. When false, the caller is closing for good: any in-progress retry loop is cancelled
        /// and this layer stops maintaining the connection.
        /// </param>
        public async Task DisconnectAsync(bool desireReconnection, MqttDisconnect? options = null, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            options ??= new MqttDisconnect();
            options.SessionExpiryInterval = 0;

            if (!desireReconnection)
            {
                // Record the close intent before touching the transport so that the reconnection loop and the
                // "Disconnected" callback both observe it and stand down rather than racing this call.
                _isClosing = true;
                _isDesiredConnected = false;
                Interlocked.MemoryBarrier();
                _reconnectionCancellationToken?.Cancel();
                lock (_connectionLifetimeCancellationTokenLock)
                {
                    _connectionLifetimeCancellationToken.Cancel();
                }
            }

            await _underlyingMqttClient.DisconnectAsync(options, cancellationToken);
        }

        public void Dispose()
        {
            if (_isDisposed)
            {
                return;
            }

            _isDisposed = true;
            _isClosing = true;
            _isDesiredConnected = false;

            _underlyingMqttClient.DisconnectedAsync -= HandleUnderlyingDisconnectedAsync;
            _underlyingMqttClient.ConnectingAsync -= DelegateConnectingAsync;
            _underlyingMqttClient.ConnectedAsync -= HandleUnderlyingConnectedAsync;
            _underlyingMqttClient.PublishReceivedAsync -= DelegatePublishReceivedAsync;

            _reconnectionCancellationToken?.Cancel();
            _reconnectionCancellationToken?.Dispose();
            lock (_connectionLifetimeCancellationTokenLock)
            {
                _connectionLifetimeCancellationToken.Cancel();
                _connectionLifetimeCancellationToken.Dispose();
            }
            _disconnectedEventLock.Dispose();

            // The underlying client has an MQTT client as a managed resource that no other client has access to, so always dispose it
            // alongside all unmanaged resources.
            _underlyingMqttClient.Dispose();

            GC.SuppressFinalize(this);
        }

        private async Task InternalDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            // Serialized with the initial connect, which holds this lock while it retries, so that a connection lost
            // right after being established is reconnected by only one of them.
            await _disconnectedEventLock.WaitAsync();

            try
            {
                if (!_isDesiredConnected || _isClosing)
                {
                    // Either the user closed the connection deliberately or this layer has already faulted. Either way,
                    // reconnecting is not wanted.
                    return;
                }

                if (_underlyingMqttClient.IsConnected())
                {
                    return;
                }

                DeviceException? disconnectException = Classify(args.Reason);
                bool switchedCredentials = false;
                if (disconnectException is { Retryability: not ErrorRetryability.Retryable })
                {
                    switchedCredentials = await TrySwitchCredentialsAfterIdentityFaultAsync(disconnectException, ConnectionLifetimeCancellationToken);
                    if (_isClosing)
                    {
                        return;
                    }
                    if (!switchedCredentials)
                    {
                        // The server told us why it hung up, and the reason says that coming back is the wrong move.
                        await EndConnectionMaintenanceAsync(disconnectException, args);
                        return;
                    }
                }

                Trace.TraceInformation("Disconnect detected, starting reconnection. Disconnect reason: {0}", args.Reason);

                _reconnectionCancellationToken?.Dispose();
                _reconnectionCancellationToken = new CancellationTokenSource();

                // A deliberate close may have landed after the check above but before the new token existed, in which
                // case it cancelled the previous token instead. Re-check so that close still takes precedence.
                Interlocked.MemoryBarrier();
                if (_isClosing)
                {
                    return;
                }

                // The most recent connect cache should be set since at least one connect must happen before this "Disconnected" callback is triggered
                Debug.Assert(_mostRecentConnect != null);

                // start reconnection if the user didn't initiate this disconnect
                await MaintainConnectionAsync(_mostRecentConnect!, args, _reconnectionCancellationToken.Token, switchedCredentials);
            }
            catch (Exception e)
            {
                // This task is unmonitored, so nothing above may escape it.
                Trace.TraceError("Unexpected exception escaped connection maintenance. {0}", e);
            }
            finally
            {
                _disconnectedEventLock.Release();
            }
        }

        /// <summary>
        /// Attempt to establish the session, retrying indefinitely for as long as the failures are retryable and the
        /// retry policy allows.
        /// </summary>
        /// <param name="options">The CONNECT to send.</param>
        /// <param name="lastDisconnect">
        /// The disconnect that triggered a reconnection, or null when this is the caller's initial connect. This is
        /// what decides whether a terminal failure is thrown to a caller or only reported through the fault event.
        /// </param>
        /// <param name="cancellationToken">Cancels the retry loop.</param>
        /// <param name="switchedCredentialsBeforeFirstAttempt">
        /// True when the disconnect that triggered this reconnection rejected the credentials and they were already
        /// switched, so the first attempt is made at once with the other set.
        /// </param>
        /// <returns>The CONNACK, or null when reconnection was abandoned.</returns>
        private async Task<MqttConnectAck?> MaintainConnectionAsync(MqttConnect options, MqttClientDisconnectedEventArgs? lastDisconnect, CancellationToken cancellationToken, bool switchedCredentialsBeforeFirstAttempt = false)
        {
            // This function is either called when initially connecting the client or when reconnecting it. The behavior
            // of this function should change depending on the context it was called. For instance, thrown exceptions are the expected
            // behavior when called from the initial ConnectAsync thread, but any exceptions thrown in the reconnection thread will be
            // unhandled and may crash the client.
            bool isReconnection = lastDisconnect != null;
            uint attemptCount = 1;
            Exception? lastException = lastDisconnect?.Exception;
            TimeSpan retryDelay = TimeSpan.Zero;

            while (true)
            {
                // This flag signals that the user is trying to close the connection.
                if (_isClosing)
                {
                    if (isReconnection)
                    {
                        // Abandon reconnecting and let this unmonitored task end quietly.
                        Trace.TraceInformation("MQTT reconnection cancelled because the client is being closed.");
                        return null;
                    }

                    // If the user disconnects the client while they were trying to connect it,
                    // stop trying to connect it and just report the most recent error.
                    throw lastException
                        ?? new OperationCanceledException("This operation was canceled because the MQTT client was closed.");
                }

                // A hub that simply will not answer is otherwise retried under the policy forever, which for a device
                // provisioned through DPS would never fall back to asking DPS for a fresh assignment. The retry policy
                // (see ExponentialBackoffRetryPolicy) decides when enough consecutive hub attempts have failed and
                // returns RetryGuidance.Reprovision, which the consultation below turns into a
                // re-provisioning crossover.

                DeviceException? deviceException = Classify(lastException, cancellationToken.IsCancellationRequested);

                if (deviceException != null)
                {
                    lastException = deviceException;
                }

                // An identity failure only ends maintenance once both sets of credentials have been rejected. Until
                // then, retry at once with the other set rather than waiting out the retry policy.
                bool retryWithSwitchedCredentials = switchedCredentialsBeforeFirstAttempt
                    || await TrySwitchCredentialsAfterIdentityFaultAsync(deviceException, cancellationToken);
                switchedCredentialsBeforeFirstAttempt = false;

                if (!retryWithSwitchedCredentials && deviceException is { Retryability: not ErrorRetryability.Retryable })
                {
                    Trace.TraceError("Encountered a fatal exception while maintaining connection {0}", deviceException);
                    await EndConnectionMaintenanceAsync(deviceException, lastDisconnect);

                    if (isReconnection)
                    {
                        // This function was called to reconnect after an unexpected disconnect. Since the error is fatal,
                        // the owning client has been notified via callback; don't throw the exception since
                        // this task is unmonitored.
                        return null;
                    }

                    // This function was called directly by the user via ConnectAsync, so also throw the exception.
                    throw deviceException;
                }

                // Always consult the retry policy when reconnecting, but only consult it on attempt > 1 when
                // initially connecting
                if (retryWithSwitchedCredentials)
                {
                    retryDelay = TimeSpan.Zero;
                }
                else if (isReconnection || attemptCount > 1)
                {
                    RetryGuidance guidance = _connectionRetryPolicy.GetRetryGuidance(attemptCount, lastException, ConnectionEndpoint, out retryDelay);

                    // The policy wants this device to stop retrying the hub and re-provision through DPS instead. This is
                    // only meaningful for a hub connection on a device that holds provisioning inputs to re-provision
                    // from; otherwise (a DPS connection, or a device connected with directly supplied credentials) it is
                    // treated the same as Retry, per the RetryGuidance documentation. End maintenance with a fault the
                    // owning client turns into a re-provisioning attempt.
                    if (guidance == RetryGuidance.Reprovision
                        && ConnectionEndpoint == ConnectionEndpoint.IotHub
                        && CanReprovision)
                    {
                        Trace.TraceWarning("Retry policy asked to abandon hub reconnection and re-provision through DPS. {0}", lastException);

                        var policyReprovisionFault = new DeviceException(
                            "The retry policy asked this device to abandon reconnecting to the IoT hub and re-provision through Device Provisioning Service.",
                            lastException!)
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = false,
                        };

                        await EndConnectionMaintenanceAsync(policyReprovisionFault, lastDisconnect, reprovisionRequired: true);

                        if (isReconnection)
                        {
                            return null;
                        }

                        // Called directly from the initial ConnectAsync, which cannot use a null ack and would otherwise
                        // await a presence flow that never arrives. The reprovisionRequired fault raised above has
                        // already asked the owning client to re-provision; surface the crossover to the caller too, the
                        // same way the terminal branches below do on an initial connect.
                        throw policyReprovisionFault;
                    }

                    // The policy wants this device to stop retrying altogether, which puts it in a terminal state.
                    if (guidance == RetryGuidance.AbandonRetry)
                    {
                        // Should not occur with the default policy as it's indefinite retry
                        Trace.TraceError("Retry policy asked to abandon retrying while maintaining a connection {0}", lastException);
                        var retryException = new RetryExpiredException("Retry policy asked to abandon retrying. See inner exception for the latest exception encountered while retrying.", lastException!);

                        // Abandoning retries is terminal by construction: there are no attempts left to make.
                        var exhaustedException = new DeviceException("Retry policy asked to abandon retrying while maintaining the connection.", retryException)
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = false,
                        };

                        await EndConnectionMaintenanceAsync(exhaustedException, lastDisconnect);

                        if (isReconnection)
                        {
                            return null;
                        }

                        throw exhaustedException;
                    }

                    // Otherwise the policy allows another attempt (RetryGuidance.Retry, or RetryGuidance.Reprovision on a
                    // DPS connection or on a device that cannot re-provision), so fall through and retry after the delay
                    // the policy provided.
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
                    Trace.TraceInformation("Trying to connect. Attempt number {0}", attemptCount);

                    using CancellationTokenSource reconnectionTimeoutCancellationToken = new();
                    reconnectionTimeoutCancellationToken.CancelAfter(_connectionAttemptTimeout);
                    using CancellationTokenSource linkedCancellationToken = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, reconnectionTimeoutCancellationToken.Token);
                    MqttConnectAck? mostRecentConnectResult = await TryEstablishConnectionAsync(options, linkedCancellationToken.Token).ConfigureAwait(false);

                    // The server accepted the current credentials, so a later rejection may fall back to the other set again.
                    _credentialsSwitchedSinceLastConnect = false;

                    if (isReconnection && _isClosing)
                    {
                        // The owner deliberately closed the connection while this attempt was completing. The close
                        // takes precedence, so drop the connection this attempt just established.
                        Trace.TraceInformation("MQTT reconnection cancelled because the client is being closed.");
                        await DisconnectSupersededReconnectionAsync().ConfigureAwait(false);
                        return null;
                    }

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
                    // Every failed attempt lands here. The loop re-classifies it at the top: retryable failures are
                    // absorbed and retried, everything else ends maintenance.
                    lastException = e;
                    Trace.TraceWarning("Encountered an exception while connecting. May attempt to reconnect. {0}", e);
                }

                // Saturate rather than wrap: under the default indefinite policy this reconnection loop can run without
                // bound, and a uint that wrapped back to 0 would make the retry policy see this as a first attempt again
                // and reset its backoff. Pinning at the maximum keeps the policy seeing an ever-growing attempt count.
                if (attemptCount < uint.MaxValue)
                {
                    attemptCount++;
                }
            }
        }

        /// <summary>
        /// Switch to the other set of credentials after an identity failure, unless they were already switched since the
        /// last successful connect.
        /// </summary>
        /// <returns>True if the next attempt should be made with the switched credentials.</returns>
        private async Task<bool> TrySwitchCredentialsAfterIdentityFaultAsync(DeviceException? fault, CancellationToken cancellationToken)
        {
            Func<CancellationToken, Task<bool>>? trySwitchCredentials = TrySwitchCredentialsAsync;
            if (fault is not { Retryability: ErrorRetryability.IdentityTerminal }
                || _credentialsSwitchedSinceLastConnect
                || trySwitchCredentials == null)
            {
                return false;
            }

            try
            {
                using CancellationTokenSource credentialTimeoutCancellationToken = new();
                credentialTimeoutCancellationToken.CancelAfter(_connectionAttemptTimeout);
                using CancellationTokenSource linkedCancellationToken = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, credentialTimeoutCancellationToken.Token);
                if (!await trySwitchCredentials(linkedCancellationToken.Token).ConfigureAwait(false))
                {
                    return false;
                }
            }
            catch (Exception e)
            {
                Trace.TraceError("Failed to switch credentials after an identity failure. {0}", e);
                return false;
            }

            _credentialsSwitchedSinceLastConnect = true;
            Trace.TraceWarning("{0} rejected the device's credentials, retrying with its other credentials. {1}", ConnectionEndpoint, fault);
            return true;
        }

        private async Task DisconnectSupersededReconnectionAsync()
        {
            try
            {
                await _underlyingMqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception e)
            {
                // Best effort: the owner's own close is already tearing this connection down.
                Trace.TraceWarning("Failed to drop a reconnection that completed after the client was closed. {0}", e);
            }
        }

        private async Task<MqttConnectAck?> TryEstablishConnectionAsync(MqttConnect options, CancellationToken cancellationToken)
        {
            if (_underlyingMqttClient.IsConnected())
            {
                return null;
            }

            MqttConnectAck connectResult = await _underlyingMqttClient.ConnectAsync(options, cancellationToken).ConfigureAwait(false);

            if (connectResult.ResultCode != MqttConnectReasonCode.Success)
            {
                throw new MqttConnectingFailedException($"Client tried to connect but server denied connection with reason '{connectResult.ResultCode}'.", connectResult);
            }

            return connectResult;
        }

        /// <summary>
        /// Stop maintaining the connection and tell the owning connection client why.
        /// </summary>
        /// <remarks>
        /// Only ever called with a terminal or identity-terminal error: retryable errors are absorbed by the retry loop
        /// and never reach here.
        /// </remarks>
        private Task EndConnectionMaintenanceAsync(DeviceException fault, MqttClientDisconnectedEventArgs? lastDisconnect, bool reprovisionRequired = false)
        {
            Debug.Assert(fault.Retryability != ErrorRetryability.Retryable);

            _isDesiredConnected = false;

            if (!_connectionFaulted.HasHandlers)
            {
                Trace.TraceError("Connection maintenance ended with no fault handler attached. {0}", fault);
                return Task.CompletedTask;
            }

            var args = new MqttConnectionFaultedEventArgs()
            {
                Exception = fault,
                LastDisconnect = lastDisconnect,
                ReprovisionRequired = reprovisionRequired,
            };

            _ = ObserveConnectionFaultHandlerAsync(_connectionFaulted.InvokeAsync(args));

            return Task.CompletedTask;
        }

        private static async Task ObserveConnectionFaultHandlerAsync(Task handlerTask)
        {
            try
            {
                await handlerTask.ConfigureAwait(false);
            }
            catch (Exception e)
            {
                // This may run on an unmonitored reconnection task, so a misbehaving handler must not crash the process.
                Trace.TraceError("The connection fault handler threw while being notified of a fatal error. {0}", e);
            }
        }

        /// <summary>
        /// Classify an exception raised while connecting or while the session was up.
        /// </summary>
        /// <param name="e">The exception to classify, if any.</param>
        /// <param name="userCancellationRequested">
        /// Whether the cancellation token this layer is aware of actually requested cancellation.
        /// </param>
        /// <returns>Null if there is nothing to classify, otherwise an exception carrying the retryability.</returns>
        private static DeviceException? Classify(Exception? e, bool userCancellationRequested)
        {
            switch (e)
            {
                case null:
                    return null;

                // Already classified; don't re-wrap and lose the classification.
                case DeviceException deviceException:
                    return deviceException;

                // The server answered the CONNECT with a refusal, so the CONNACK code is the authority.
                case MqttConnectingFailedException connectingFailed:
                    {
                        DeviceException? connackException = Classify(connectingFailed.ResultCode);
                        if (connackException == null)
                        {
                            return null;
                        }

                        return new DeviceException(connackException.Message, connectingFailed)
                        {
                            Retryability = connackException.Retryability,
                            IsContained = false,
                        };
                    }

                // MQTTnet 5 returns a CONNACK refusal (including NotAuthorized) as a result rather than throwing it; the
                // adapter maps that result and TryEstablishConnectionAsync turns it into the SDK's own
                // MqttConnectingFailedException above. MQTTnet only throws this exception to wrap a failure while
                // exchanging CONNECT/CONNACK (for example the connection closing first), so classify what it wraps.
                case MQTTnet.Adapter.MqttConnectingFailedException connectingFailed:
                    {
                        DeviceException? innerException = Classify(connectingFailed.InnerException, userCancellationRequested);
                        if (innerException == null)
                        {
                            return Retryable(connectingFailed, "The connection attempt failed before the server answered it.");
                        }

                        return new DeviceException(innerException.Message, connectingFailed)
                        {
                            Retryability = innerException.Retryability,
                            IsContained = false,
                        };
                    }

                // connection.md section 9.4.1: name resolution, address selection, socket connect and connection reset
                // are all retryable, and none of them say anything about the device's identity.
                case SocketException:
                    return Retryable(e, "A network-level error was encountered while connecting.");

                // connection.md section 9.4.2: all but the least specific handshake failure are terminal, and this
                // exception cannot be told apart from those, so it takes the terminal branch rather than retrying
                // forever against a trust-store or certificate problem that no retry can fix.
                case AuthenticationException:
                    return new DeviceException("The TLS handshake failed.", e)
                    {
                        Retryability = ErrorRetryability.Terminal,
                        IsContained = false,
                    };

                // connection.md section 9.4.8: programming and configuration errors are deterministic, so retrying
                // them just reproduces them.
                case MQTTnet.Exceptions.MqttProtocolViolationException:
                case ArgumentException:
                case NotSupportedException:
                case ObjectDisposedException:
                    return new DeviceException("A non-recoverable error was encountered while connecting.", e)
                    {
                        Retryability = ErrorRetryability.Terminal,
                        IsContained = false,
                    };

                case OperationCanceledException:
                    // MQTTnet may throw an OperationCanceledException/TaskCanceledException even if
                    // neither the user nor the session client provides a cancellation token. Because
                    // of that, this exception is only fatal if the cancellation token this layer
                    // is aware of actually requested cancellation. Other cases signify that MQTTnet
                    // gave up on the operation, but the user still wants to retry.
                    if (userCancellationRequested)
                    {
                        return new DeviceException("The connection attempt was canceled.", e)
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = false,
                        };
                    }

                    return Retryable(e, "The connection attempt did not complete in time.");

                default:
                    // Unrecognised. Preserve the value and take the conservative branch.
                    return Retryable(e, "A recoverable error was encountered while connecting.");
            }

            static DeviceException Retryable(Exception inner, string message) => new(message, inner)
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = false,
            };
        }

        /// <summary>
        /// Classify a server DISCONNECT reason code per connection.md section 9.3.7.
        /// </summary>
        /// <param name="code">The reason code carried by the server's DISCONNECT packet.</param>
        /// <returns>Null if the code is benign, otherwise an exception whose retryability describes the code.</returns>
        private static DeviceException? Classify(MqttDisconnectReason code)
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
        private static DeviceException? Classify(MqttConnectReasonCode code)
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

        /// <summary>
        /// Change the endpoint that the connections this layer maintains target, and wait until no PUBLISH, SUBSCRIBE or
        /// UNSUBSCRIBE meant for the previous endpoint is still in flight.
        /// </summary>
        /// <remarks>
        /// Operations are admitted for an endpoint under the same lock that this switches the endpoint under, so once
        /// this returns no operation meant for the previous endpoint can reach the underlying client. The caller must
        /// await this before connecting to the new endpoint; otherwise an operation that was admitted while the previous
        /// endpoint was current could be sent over the new endpoint's connection (for example, feature traffic meant
        /// for IoT hub being sent to Device Provisioning Service while re-provisioning).
        /// </remarks>
        public async Task SwitchEndpointAsync(ConnectionEndpoint endpoint, CancellationToken cancellationToken = default)
        {
            Task staleOperationsDrained;
            lock (_endpointLock)
            {
                _connectionEndpoint = endpoint;

                if (!HasStaleOperationsInFlight())
                {
                    return;
                }

                _staleOperationsDrained ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                staleOperationsDrained = _staleOperationsDrained.Task;
            }

            Trace.TraceInformation("Waiting for operations meant for the previous endpoint to finish before targeting {0}.", endpoint);
            await staleOperationsDrained.WaitAsync(cancellationToken);
        }

        /// <summary>
        /// Admit an operation meant for the given endpoint, or throw <see cref="MqttClientNotConnectedException"/> if
        /// this layer is not targeting that endpoint.
        /// </summary>
        /// <remarks>
        /// Every admitted operation must be ended with exactly one call to <see cref="EndOperation"/>.
        /// </remarks>
        private void BeginOperation(ConnectionEndpoint endpoint)
        {
            lock (_endpointLock)
            {
                if (_connectionEndpoint != endpoint)
                {
                    // Reported as "not connected" because, from the caller's point of view, it is: there is no
                    // connection to the endpoint it wants. The layer above waits for one rather than failing.
                    throw new MqttClientNotConnectedException($"Not connected to {endpoint}; the current connection targets {_connectionEndpoint}.");
                }

                _inFlightOperationCounts[(int)endpoint]++;
            }
        }

        private void EndOperation(ConnectionEndpoint endpoint)
        {
            TaskCompletionSource? staleOperationsDrained = null;
            lock (_endpointLock)
            {
                _inFlightOperationCounts[(int)endpoint]--;

                if (_staleOperationsDrained != null && !HasStaleOperationsInFlight())
                {
                    staleOperationsDrained = _staleOperationsDrained;
                    _staleOperationsDrained = null;
                }
            }

            staleOperationsDrained?.TrySetResult();
        }

        // Must be called while holding _endpointLock.
        private bool HasStaleOperationsInFlight()
        {
            for (int endpoint = 0; endpoint < _inFlightOperationCounts.Length; endpoint++)
            {
                if (endpoint != (int)_connectionEndpoint && _inFlightOperationCounts[endpoint] > 0)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// Send a PUBLISH to the given endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// Failures here are contained to the one operation, so they are thrown to the caller rather than being
        /// retried or reported as connection faults. Already-classified <see cref="DeviceException"/>s are rethrown
        /// unchanged so their retryability survives, and <see cref="MqttClientNotConnectedException"/> is left alone
        /// because it reports connection state rather than an operation failure, and the layer above acts on it. That
        /// includes the case where this layer is not targeting <paramref name="endpoint"/>.
        /// </remarks>
        public Task<MqttPublishAck> PublishAsync(ConnectionEndpoint endpoint, MqttPublish publish, CancellationToken cancellationToken = default)
        {
            return BuildAndPublishAsync(endpoint, () => publish, cancellationToken);
        }

        /// <summary>
        /// Build a PUBLISH only once it has been admitted for the given endpoint, then send it to that endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// <paramref name="buildPublish"/> runs after the operation is admitted and before it is sent, while
        /// <see cref="SwitchEndpointAsync"/> waits for it, so state it reads (such as the device ID in the topic) cannot
        /// change to that of a different endpoint connection between being read and the operation being sent. It may
        /// throw <see cref="MqttClientNotConnectedException"/> to report that the operation cannot be sent yet.
        /// </remarks>
        public Task<MqttPublishAck> BuildAndPublishAsync(ConnectionEndpoint endpoint, Func<MqttPublish> buildPublish, CancellationToken cancellationToken = default)
        {
            return SendAndSettleAsync(
                endpoint,
                buildPublish,
                _underlyingMqttClient.PublishAsync,
                publish => publish.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtLeastOnce,
                ThrowIfPubackHasErrorCode,
                "publishing",
                cancellationToken);
        }

        /// <summary>
        /// Send a SUBSCRIBE to the given endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// Failures here are contained to the one operation, so they are thrown to the caller rather than being
        /// retried or reported as connection faults. Already-classified <see cref="DeviceException"/>s are rethrown
        /// unchanged so their retryability survives, and <see cref="MqttClientNotConnectedException"/> is left alone
        /// because it reports connection state rather than an operation failure, and the layer above acts on it. That
        /// includes the case where this layer is not targeting <paramref name="endpoint"/>.
        /// </remarks>
        public Task<MqttSubscribeAck> SubscribeAsync(ConnectionEndpoint endpoint, MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            return BuildAndSubscribeAsync(endpoint, () => subscribe, cancellationToken);
        }

        /// <summary>
        /// Build a SUBSCRIBE only once it has been admitted for the given endpoint, then send it to that endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// <paramref name="buildSubscribe"/> runs after the operation is admitted and before it is sent, while
        /// <see cref="SwitchEndpointAsync"/> waits for it, so state it reads (such as the device ID in the topic) cannot
        /// change to that of a different endpoint connection between being read and the operation being sent. It may
        /// throw <see cref="MqttClientNotConnectedException"/> to report that the operation cannot be sent yet.
        /// </remarks>
        public Task<MqttSubscribeAck> BuildAndSubscribeAsync(ConnectionEndpoint endpoint, Func<MqttSubscribe> buildSubscribe, CancellationToken cancellationToken = default)
        {
            return SendAndSettleAsync(
                endpoint,
                buildSubscribe,
                _underlyingMqttClient.SubscribeAsync,
                _ => true,
                ThrowIfSubackHasErrorCode,
                "subscribing",
                cancellationToken);
        }

        /// <summary>
        /// Send a UNSUBSCRIBE to the given endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// Failures here are contained to the one operation, so they are thrown to the caller rather than being
        /// retried or reported as connection faults. Already-classified <see cref="DeviceException"/>s are rethrown
        /// unchanged so their retryability survives, and <see cref="MqttClientNotConnectedException"/> is left alone
        /// because it reports connection state rather than an operation failure, and the layer above acts on it. That
        /// includes the case where this layer is not targeting <paramref name="endpoint"/>.
        /// </remarks>
        public Task<MqttUnsubscribeAck> UnsubscribeAsync(ConnectionEndpoint endpoint, MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            return BuildAndUnsubscribeAsync(endpoint, () => unsubscribe, cancellationToken);
        }

        /// <summary>
        /// Build a UNSUBSCRIBE only once it has been admitted for the given endpoint, then send it to that endpoint and settle it.
        /// </summary>
        /// <remarks>
        /// <paramref name="buildUnsubscribe"/> runs after the operation is admitted and before it is sent, while
        /// <see cref="SwitchEndpointAsync"/> waits for it, so state it reads (such as the device ID in the topic) cannot
        /// change to that of a different endpoint connection between being read and the operation being sent. It may
        /// throw <see cref="MqttClientNotConnectedException"/> to report that the operation cannot be sent yet.
        /// </remarks>
        public Task<MqttUnsubscribeAck> BuildAndUnsubscribeAsync(ConnectionEndpoint endpoint, Func<MqttUnsubscribe> buildUnsubscribe, CancellationToken cancellationToken = default)
        {
            return SendAndSettleAsync(
                endpoint,
                buildUnsubscribe,
                _underlyingMqttClient.UnsubscribeAsync,
                _ => true,
                ThrowIfUnsubackHasErrorCode,
                "unsubscribing",
                cancellationToken);
        }

        /// <summary>
        /// Admit an operation for the given endpoint, build its packet, send it and settle its acknowledgement.
        /// </summary>
        /// <param name="isAcknowledged">
        /// Whether the packet is one the server acknowledges (a QoS 1 PUBLISH, a SUBSCRIBE or an UNSUBSCRIBE). Only such
        /// packets sent to IoT hub are reported as <see cref="MqttClientNotConnectedException"/> when the connection is
        /// lost before their acknowledgement arrives, so that the caller sends them again once reconnected. DPS traffic
        /// is not, because the provisioning flow starts over from the beginning whenever its connection is lost.
        /// </param>
        private async Task<TAck> SendAndSettleAsync<TPacket, TAck>(
            ConnectionEndpoint endpoint,
            Func<TPacket> buildPacket,
            Func<TPacket, CancellationToken, Task<TAck>> send,
            Func<TPacket, bool> isAcknowledged,
            Action<TAck> throwIfAckHasErrorCode,
            string operationDescription,
            CancellationToken cancellationToken)
        {
            BeginOperation(endpoint);
            try
            {
                TPacket packet = buildPacket();
                long disconnectionCountBeforeSend = Interlocked.Read(ref _disconnectionCount);

                TAck ack;
                try
                {
                    ack = await send(packet, cancellationToken);
                }
                catch (Exception e) when (endpoint == ConnectionEndpoint.IotHub
                    && isAcknowledged(packet)
                    && WasConnectionLostDuring(e, disconnectionCountBeforeSend, cancellationToken))
                {
                    throw new MqttClientNotConnectedException(
                        $"The connection was lost while {operationDescription}, before the acknowledgement arrived.", e);
                }

                throwIfAckHasErrorCode(ack);
                return ack;
            }
            catch (Exception e) when (e is not DeviceException and not MqttClientNotConnectedException and not OperationCanceledException)
            {
                throw new DeviceException($"Encountered an exception while {operationDescription}", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = true,
                };
            }
            finally
            {
                EndOperation(endpoint);
            }
        }

        /// <summary>
        /// Whether an operation failed because the connection it was sent over was lost before it was acknowledged.
        /// </summary>
        /// <remarks>
        /// This holds for any <see cref="IMqttClient"/>, not just ones that report the loss as
        /// <see cref="MqttClientNotConnectedException"/>: if a disconnection was reported after the operation was sent,
        /// or the client is no longer connected, its acknowledgement can never arrive.
        /// </remarks>
        private bool WasConnectionLostDuring(Exception e, long disconnectionCountBeforeSend, CancellationToken cancellationToken)
        {
            if (e is MqttClientNotConnectedException or DeviceException
                || (e is OperationCanceledException && cancellationToken.IsCancellationRequested))
            {
                // Already reported as a lost connection, already classified, or cancelled by the caller.
                return false;
            }

            return Interlocked.Read(ref _disconnectionCount) != disconnectionCountBeforeSend
                || !_underlyingMqttClient.IsConnected();
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
