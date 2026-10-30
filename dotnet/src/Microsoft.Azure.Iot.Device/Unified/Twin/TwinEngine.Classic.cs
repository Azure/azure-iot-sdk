// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    // How IoT hub's (MQTTv3) twin protocol shapes this engine:
    //
    // - IoT hub neither acknowledges a device's presence with its twin versions nor pushes the twin upon connecting. Instead,
    //   once the device presence flow completes on each connection, this client GETs the full twin (the "connect sync"),
    //   raises TwinPushReceived with the sections that changed, and only then sends reported properties patches. The connect
    //   sync is a desired properties resync, so desired patches received meanwhile are buffered and applied after it.
    // - Reported properties patches can't be conditional (there is no if-match), so a patch whose response is lost is applied
    //   again when it is resent. Patches are therefore applied at least once, rather than at most once.
    // - Requests and responses are QoS 0, so either may be lost. Each request is sent again if its response doesn't arrive
    //   within a defensive timeout.
    // - There is no connection nonce on the wire, so the connection client identifies each connection with a local one.
    // - GET requests can't select sections or be conditional, so this client filters the full twin that IoT hub returns.
    // - Responses carry HTTP-like status codes, which this client maps onto the twin results (see Result).
    internal sealed partial class TwinEngine
    {
        private const string ClassicTwinTopicPrefix = "$iothub/twin/";
        private const string ClassicTwinResponseTopic = "$iothub/twin/res/";
        private const string ClassicTwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";
        private const string ClassicTwinGetTopicFormat = "$iothub/twin/GET/?$rid={0}";
        private const string ClassicTwinReportedPropertiesPatchTopicFormat = "$iothub/twin/PATCH/properties/reported/?$rid={0}";
        private const string ClassicRequestIdKey = "$rid";
        private const string ClassicVersionKey = "$version";

        // For example, "$iothub/twin/res/204/?$rid=efc34c73-79ce-4054-9985-0cdf40a3c794&$version=2".
        private static readonly Regex s_classicResponseTopicRegex = new(@"^\$iothub/twin/res/(\d+)/\?(.*)$", RegexOptions.Compiled);

        // Guarded by _desiredStateLock. The connection on which this client last started a connect sync.
        private Guid? _classicConnectSyncConnectionNonce;

        private void HandleClassicDevicePresenceEstablished()
        {
            if (_isDisposed)
            {
                return;
            }

            Guid? connectionNonce = _connection.ConnectionNonce;
            ulong reportedVersionAtStart;
            lock (_reportedStateLock)
            {
                reportedVersionAtStart = _localReportedVersion;
            }

            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                if (_isDisposed || (connectionNonce != null && _classicConnectSyncConnectionNonce == connectionNonce))
                {
                    // This client already started the connect sync for this connection.
                    return;
                }

                _classicConnectSyncConnectionNonce = connectionNonce;
                DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                if (_desiredResync == null)
                {
                    StartDesiredResync(connectionNonce, null, outcome);
                }
                else
                {
                    // A resync that a desired patch started on this connection may have sent its GET before the device
                    // subscribed to the responses, so send a new one.
                    SendNewDesiredResyncGet(_desiredResync, outcome);
                }

                _desiredResync!.IsConnectSync = true;
                _desiredResync.DesiredVersionAtStart = _localDesiredVersion;
                _desiredResync.ReportedVersionAtStart = reportedVersionAtStart;
            }

            Trace.TraceInformation("Connected to IoT hub, so getting the twin before sending any reported properties patches.");

            // The connection client raises this while it holds the lock that reconnecting needs, so the GET must not be sent
            // (which may wait for a reconnection if the connection is lost meanwhile) on this thread.
            _ = Task.Run(() => CompleteAsync(outcome));
        }

        // Must be called while holding _desiredStateLock. IoT hub's versions never decrease, so if this device held a newer version
        // of the desired properties than IoT hub when connecting, the version it holds is not valid (for example, the application
        // restored the wrong version, or the twin was recreated). This device then adopts IoT hub's desired properties instead. If
        // this device's version changed since connecting, it came from IoT hub on this connection, so it is kept.
        private void ResetLocalDesiredVersionIfNewerThanHub(DesiredResync connectSync, TwinGetResponse response)
        {
            if (response.HasDesiredPayload
                && response.DesiredVersion < _localDesiredVersion
                && _localDesiredVersion == connectSync.DesiredVersionAtStart)
            {
                Trace.TraceWarning("This device held version {0} of the desired properties, but IoT hub only has version {1}. Adopting version {1}.", _localDesiredVersion, response.DesiredVersion);
                lock (_twinStateLock)
                {
                    _localDesiredVersion = 0;
                    _isTwinStateChanged = true;
                }
            }
        }

        // Adopts the reported section of the connect sync's response, if it differs from this device's copy. The connect sync's
        // twin push (if raised) carries the adopted section. Reported properties patches are sent once its events are raised.
        private void CompleteClassicConnectSync(DesiredResync connectSync, TwinGetResponse response, TwinPushReceivedEventArgs push, DesiredStateOutcome outcome)
        {
            if (response.HasReportedPayload)
            {
                JsonObject reportedProperties = ParseSectionPayload(response.ReportedPayload);
                bool isAdopted = false;
                lock (_reportedStateLock)
                {
                    if (response.ReportedVersion > _localReportedVersion)
                    {
                        AdvanceLocalReportedVersion(response.ReportedVersion, reportedProperties);
                        isAdopted = true;
                    }
                    else if (response.ReportedVersion < _localReportedVersion && _localReportedVersion == connectSync.ReportedVersionAtStart)
                    {
                        Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub only has version {1}. Adopting version {1}.", _localReportedVersion, response.ReportedVersion);
                        SetLocalReportedState(response.ReportedVersion, reportedProperties);
                        isAdopted = true;
                    }
                }

                if (isAdopted)
                {
                    push.Reported = new TwinPushSection()
                    {
                        Properties = reportedProperties,
                        PropertiesVersion = response.ReportedVersion,
                    };

                    if (push.Desired == null)
                    {
                        outcome.Deliveries.Insert(0, push);
                    }
                }
            }

            outcome.CompletedConnectSync = connectSync;
        }

        // IoT hub's version of the reported properties is now known, and this device's copy is up to date, so patches can be sent.
        private void StartSendingReportedPatchesAfterConnectSync(DesiredResync connectSync)
        {
            ReportedPatchAttempt? nextAttempt = null;
            lock (_reportedPatchLock)
            {
                if (connectSync.ConnectionNonce == _connection.ConnectionNonce)
                {
                    _birthAckConnectionNonce = connectSync.ConnectionNonce;
                    nextAttempt = TryStartReportedPatchAttempt();
                }
            }

            SendReportedPatchAttempt(nextAttempt);
        }

        private static MqttPublish CreateClassicGetTwinPublish(Guid getId)
        {
            return new MqttPublish()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinGetTopicFormat, getId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
        }

        private static MqttPublish CreateClassicReportedPatchPublish(ReportedPatchAttempt attempt)
        {
            // IoT hub doesn't support if-match, so this patch is unconditional.
            return new MqttPublish()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinReportedPropertiesPatchTopicFormat, attempt.PatchId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                Payload = attempt.Payload,
            };
        }

        private async Task HandleReceivedClassicPublishAsync(MqttPublish publish)
        {
            if (publish.Topic.StartsWith(ClassicTwinResponseTopic, StringComparison.Ordinal))
            {
                await HandleReceivedClassicResponseAsync(publish);
            }
            else if (publish.Topic.StartsWith(ClassicTwinDesiredPropertiesPatchTopic, StringComparison.Ordinal))
            {
                await HandleReceivedClassicDesiredPatchAsync(publish);
            }
        }

        private async Task HandleReceivedClassicResponseAsync(MqttPublish publish)
        {
            Match match = s_classicResponseTopicRegex.Match(publish.Topic);
            if (!match.Success
                || !int.TryParse(match.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out int status))
            {
                Trace.TraceWarning("Dropping a twin response with a malformed topic: {0}", publish.Topic);
                return;
            }

            NameValueCollection query = HttpUtility.ParseQueryString(match.Groups[2].Value);
            if (!Guid.TryParse(query.Get(ClassicRequestIdKey), out Guid requestId))
            {
                Trace.TraceWarning("Dropping a twin response without a valid request ID: {0}", publish.Topic);
                return;
            }

            GetTwinExchange? exchange = TryTakeGetTwinExchange(requestId);
            if (exchange != null)
            {
                HandleClassicGetTwinExchangeResponse(exchange, status, publish);
                return;
            }

            bool isReportedPatchResponse;
            lock (_reportedPatchLock)
            {
                isReportedPatchResponse = _reportedPatchAttempt?.PatchId == requestId;
            }

            if (isReportedPatchResponse)
            {
                _ = ulong.TryParse(query.Get(ClassicVersionKey), NumberStyles.None, CultureInfo.InvariantCulture, out ulong version);
                HandleReportedPatchResponse(requestId, new ReportedPatchResponse()
                {
                    Result = MapClassicReportedPatchStatus(status),
                    Version = version,
                });
                return;
            }

            if (!IsClassicSuccessStatus(status))
            {
                // If this was the response to the resync GET, it is sent again once it times out.
                Trace.TraceWarning("IoT hub rejected a get twin request with status {0}.", status);
                return;
            }

            if (publish.Payload == null || publish.Payload.Length == 0)
            {
                // For example, the response to an earlier attempt of a reported properties patch.
                Trace.TraceInformation("Dropping a twin response that does not match any outstanding twin request.");
                return;
            }

            if (!TryParseClassicTwin(publish, out TwinGetResponse? response))
            {
                return;
            }

            await HandleDesiredResyncGetResponseAsync(requestId, response);
        }

        private void HandleClassicGetTwinExchangeResponse(GetTwinExchange exchange, int status, MqttPublish publish)
        {
            if (!IsClassicSuccessStatus(status))
            {
                exchange.Response.TrySetException(new DeviceException($"IoT hub rejected the get twin request with status {status}.")
                {
                    Retryability = IsClassicRetryableStatus(status) ? ErrorRetryability.Retryable : ErrorRetryability.Terminal,
                    IsContained = true,
                });
                return;
            }

            if (!TryParseClassicTwin(publish, out TwinGetResponse? response))
            {
                exchange.Response.TrySetException(new DeviceException("IoT hub returned a malformed twin.")
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = true,
                });
                return;
            }

            // IoT hub always returns the full twin, so drop the sections that the request's if-not-match versions exclude.
            GetTwinRequest request = exchange.Request;
            if (request.IfNotMatchDesired != 0 && request.IfNotMatchDesired >= response.DesiredVersion)
            {
                response.ClearDesiredPayload();
            }

            if (request.IfNotMatchReported != 0 && request.IfNotMatchReported >= response.ReportedVersion)
            {
                response.ClearReportedPayload();
            }

            HandleGetTwinExchangeResponse(exchange, response);
        }

        private async Task HandleReceivedClassicDesiredPatchAsync(MqttPublish publish)
        {
            JsonObject patch;
            ulong version;
            try
            {
                patch = JsonNode.Parse(publish.Payload ?? Array.Empty<byte>())!.AsObject();
                version = TakeClassicVersion(patch);
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException or NullReferenceException)
            {
                // A desired patch that is never processed leaves a gap, so the next patch starts a resync.
                Trace.TraceWarning("Dropping a malformed desired properties patch: {0}", e);
                return;
            }

            if (version == 0)
            {
                Trace.TraceWarning("Dropping a desired properties patch without a version.");
                return;
            }

            DesiredPatch desiredPatch = new()
            {
                Version = version,
                Payload = ByteString.CopyFrom(JsonSerializer.SerializeToUtf8Bytes(patch)),
            };

            Guid? connectionNonce = _connection.ConnectionNonce;

            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                ProcessDesiredPatch(desiredPatch, connectionNonce, outcome);
            }

            await CompleteAsync(outcome);
        }

        // Converts the full twin that IoT hub returns, such as {"desired":{"$version":1},"reported":{"$version":1}}, into a
        // response carrying both sections.
        private static bool TryParseClassicTwin(MqttPublish publish, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out TwinGetResponse? response)
        {
            try
            {
                JsonObject twin = JsonNode.Parse(publish.Payload ?? Array.Empty<byte>())!.AsObject();
                JsonObject desired = twin["desired"]?.AsObject() ?? new JsonObject();
                JsonObject reported = twin["reported"]?.AsObject() ?? new JsonObject();
                response = new TwinGetResponse()
                {
                    DesiredVersion = TakeClassicVersion(desired),
                    ReportedVersion = TakeClassicVersion(reported),
                    DesiredPayload = ByteString.CopyFrom(JsonSerializer.SerializeToUtf8Bytes(desired)),
                    ReportedPayload = ByteString.CopyFrom(JsonSerializer.SerializeToUtf8Bytes(reported)),
                };
                return true;
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException or NullReferenceException)
            {
                // If this was the response to an outstanding resync, the request is sent again once it times out.
                Trace.TraceWarning("Received a malformed get twin response. Ignoring it: {0}", e);
                response = null;
                return false;
            }
        }

        // Removes the "$version" property that IoT hub adds to each twin section, and returns it (or 0 if it is absent).
        private static ulong TakeClassicVersion(JsonObject section)
        {
            ulong version = section[ClassicVersionKey]?.GetValue<ulong>() ?? 0;
            section.Remove(ClassicVersionKey);
            return version;
        }

        private static bool IsClassicSuccessStatus(int status)
        {
            return status >= 200 && status < 300;
        }

        private static bool IsClassicRetryableStatus(int status)
        {
            return status == 429 || status >= 500;
        }

        private static Result MapClassicReportedPatchStatus(int status)
        {
            return status switch
            {
                _ when IsClassicSuccessStatus(status) => Result.Ok,
                400 => Result.PayloadInvalid,
                413 => Result.PayloadTooLarge,
                _ => Result.InternalError,
            };
        }
    }
}
