using MQTTnet.Protocol;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The connection dropping machinery shared by <see cref="StubIotHubService"/> and
    /// <see cref="StubDeviceProvisioningService"/>: reason code selection, the random drop loop, and the drop history.
    /// </summary>
    /// <remarks>
    /// Both stubs are MQTT clients rather than brokers, both borrow the ability to close a device's session from an
    /// <see cref="IStubDeviceConnectionDropper"/>, and both want the same knobs over it, so the behavior lives here once
    /// and each stub exposes it in its own vocabulary.
    /// </remarks>
    internal sealed class StubConnectionDropEngine
    {
        private readonly Lock _randomLock = new();
        private readonly List<StubDeviceConnectionDroppedEventArgs> _history = new();

        private readonly StubConnectionDropOptions _options;
        private readonly IStubDeviceConnectionDropper? _dropper;
        private readonly Func<string, bool> _canDropDevice;
        private readonly Func<string, bool> _isRandomDropCandidate;
        private readonly Action<StubDeviceConnectionDroppedEventArgs> _onDropped;
        private readonly Action<string> _log;
        private readonly string _ownerDescription;
        private readonly string _dropperOptionName;

        private Random? _random;
        private CancellationTokenSource? _loopCancellation;
        private Task? _loop;

        /// <param name="options">The random drop configuration.</param>
        /// <param name="dropper">What actually closes a device's session, or null if the owner was not given one.</param>
        /// <param name="canDropDevice">Whether the owner is willing to drop the given device at all.</param>
        /// <param name="isRandomDropCandidate">Whether the given connected device may be chosen for a random drop.</param>
        /// <param name="onDropped">Invoked after a drop is recorded, so the owner can update its own state and raise its event.</param>
        /// <param name="log">The owner's diagnostic sink.</param>
        /// <param name="ownerDescription">How the owner refers to itself in diagnostics, for example "stub DPS".</param>
        /// <param name="dropperOptionName">The fully qualified name of the owner's connection dropper option, for error messages.</param>
        internal StubConnectionDropEngine(
            StubConnectionDropOptions options,
            IStubDeviceConnectionDropper? dropper,
            Func<string, bool> canDropDevice,
            Func<string, bool> isRandomDropCandidate,
            Action<StubDeviceConnectionDroppedEventArgs> onDropped,
            Action<string> log,
            string ownerDescription,
            string dropperOptionName)
        {
            _options = options;
            _dropper = dropper;
            _canDropDevice = canDropDevice;
            _isRandomDropCandidate = isRandomDropCandidate;
            _onDropped = onDropped;
            _log = log;
            _ownerDescription = ownerDescription;
            _dropperOptionName = dropperOptionName;
        }

        internal bool CanDropConnections => _dropper != null;

        internal IReadOnlyList<StubDeviceConnectionDroppedEventArgs> History
        {
            get
            {
                lock (_randomLock)
                {
                    return [.. _history];
                }
            }
        }

        internal MqttDisconnectReasonCode NextRandomReasonCode()
        {
            IReadOnlyList<MqttDisconnectReasonCode> reasonCodes = _options.ReasonCodes;

            return reasonCodes[NextRandomIndex(reasonCodes.Count)];
        }

        internal async Task<StubDeviceConnectionDroppedEventArgs?> DropAsync(
            string deviceId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString,
            bool wasRandom,
            CancellationToken cancellationToken)
        {
            IStubDeviceConnectionDropper dropper = RequireDropper();

            if (!_canDropDevice(deviceId))
            {
                throw new InvalidOperationException(
                    $"The {_ownerDescription} does not serve device '{deviceId}', so it will not drop its connection.");
            }

            string? effectiveReasonString = reasonString ?? _options.ReasonString;

            bool wasDropped = await dropper.DropDeviceConnectionAsync(deviceId, reasonCode, effectiveReasonString, cancellationToken);
            if (!wasDropped)
            {
                _log($"The {_ownerDescription} did not drop device '{deviceId}' because it was not connected.");
                return null;
            }

            var args = new StubDeviceConnectionDroppedEventArgs
            {
                DeviceId = deviceId,
                ReasonCode = reasonCode,
                ReasonString = effectiveReasonString,
                WasRandom = wasRandom,
                DroppedOnUtc = DateTimeOffset.UtcNow,
            };

            lock (_randomLock)
            {
                _history.Add(args);
            }

            _log($"The {_ownerDescription} dropped device '{deviceId}' with reason code {reasonCode}.");
            _onDropped(args);

            return args;
        }

        internal async Task<StubDeviceConnectionDroppedEventArgs?> DropRandomAsync(CancellationToken cancellationToken)
        {
            IReadOnlyList<string> candidates = await GetCandidateDeviceIdsAsync(cancellationToken);
            if (candidates.Count == 0)
            {
                return null;
            }

            string deviceId = candidates[NextRandomIndex(candidates.Count)];

            return await DropAsync(deviceId, NextRandomReasonCode(), reasonString: null, wasRandom: true, cancellationToken);
        }

        /// <summary>
        /// Start the random drop loop, if it is enabled. Throws if it is enabled without a dropper, or with contradictory
        /// options, since a test that asked for random drops and silently got none would be worse than useless.
        /// </summary>
        internal void StartRandomDrops()
        {
            if (!_options.Enabled)
            {
                return;
            }

            _options.Validate();
            RequireDropper();

            _loopCancellation = new CancellationTokenSource();
            _loop = Task.Run(() => RunRandomDropsAsync(_loopCancellation.Token));

            _log($"The {_ownerDescription} will randomly drop device connections every {_options.MinInterval} to "
                + $"{_options.MaxInterval} with probability {_options.Probability}.");
        }

        internal async Task StopRandomDropsAsync()
        {
            CancellationTokenSource? cancellation = _loopCancellation;
            Task? loop = _loop;

            _loopCancellation = null;
            _loop = null;

            if (cancellation == null)
            {
                return;
            }

            await cancellation.CancelAsync();

            if (loop != null)
            {
                try
                {
                    await loop;
                }
                catch (OperationCanceledException)
                {
                    // Expected: this is how the loop ends.
                }
            }

            cancellation.Dispose();
        }

        private IStubDeviceConnectionDropper RequireDropper()
        {
            return _dropper
                ?? throw new InvalidOperationException(
                    $"The {_ownerDescription} cannot drop a device's connection because no {_dropperOptionName} was configured. "
                    + $"The stub is an MQTT client rather than a broker, so it needs the broker's help to close another client's "
                    + $"session. {nameof(InProcessMqttBroker)} implements {nameof(IStubDeviceConnectionDropper)}.");
        }

        private async Task<IReadOnlyList<string>> GetCandidateDeviceIdsAsync(CancellationToken cancellationToken)
        {
            IStubDeviceConnectionDropper dropper = RequireDropper();
            IReadOnlyList<string> connected = await dropper.GetConnectedDeviceIdsAsync(cancellationToken);
            IReadOnlyList<string>? allowList = _options.DeviceIds;

            // The dropper reports every connection the broker holds, which includes the stubs themselves. Only ids the owner
            // recognizes as one of its devices are eligible.
            return
            [
                .. connected.Where(deviceId =>
                    _isRandomDropCandidate(deviceId)
                    && (allowList == null || allowList.Contains(deviceId, StringComparer.Ordinal)))
            ];
        }

        private async Task RunRandomDropsAsync(CancellationToken cancellationToken)
        {
            int dropCount = 0;

            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    await Task.Delay(NextRandomInterval(), cancellationToken);

                    if (NextRandomProbability() >= _options.Probability)
                    {
                        continue;
                    }

                    StubDeviceConnectionDroppedEventArgs? dropped = await DropRandomAsync(cancellationToken);
                    if (dropped == null)
                    {
                        // Nothing eligible was connected this time around. Wait for the next interval and try again.
                        continue;
                    }

                    if (++dropCount == _options.MaxDrops)
                    {
                        _log($"The {_ownerDescription} reached its configured limit of {_options.MaxDrops} random connection drops.");
                        return;
                    }
                }
            }
            catch (OperationCanceledException)
            {
                // Expected: the stub is stopping.
            }
            catch (Exception e)
            {
                // The random drop loop is a test aid, so a failure in it must not take the stub down with it.
                _log($"The {_ownerDescription}'s random connection drop loop stopped after an exception: {e}");
            }
        }

        private TimeSpan NextRandomInterval()
        {
            if (_options.MaxInterval <= _options.MinInterval)
            {
                return _options.MinInterval;
            }

            double spread = (_options.MaxInterval - _options.MinInterval).TotalMilliseconds;

            return _options.MinInterval + TimeSpan.FromMilliseconds(NextRandomProbability() * spread);
        }

        private double NextRandomProbability()
        {
            lock (_randomLock)
            {
                return GetRandom().NextDouble();
            }
        }

        private int NextRandomIndex(int exclusiveUpperBound)
        {
            lock (_randomLock)
            {
                return GetRandom().Next(exclusiveUpperBound);
            }
        }

        private Random GetRandom()
        {
            // Callers hold _randomLock, because Random is not thread safe and the seeded sequence has to stay reproducible.
            return _random ??= _options.RandomSeed is int seed
                ? new Random(seed)
                : new Random();
        }
    }
}
