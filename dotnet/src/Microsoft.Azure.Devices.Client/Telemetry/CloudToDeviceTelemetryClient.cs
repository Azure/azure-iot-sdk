using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Data.Common;
using System.Globalization;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    public class CloudToDeviceTelemetryClient
    {
        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        private readonly IConnectionClient _connection;

        /// <summary>
        /// An event that executes every time this device receives cloud-to-device telemetry. Once received, the application must decide how to complete that telemetry.
        /// </summary>
        public event Func<CloudToDeviceTelemetry, Task>? CloudToDeviceTelemetryReceivedAsync;

        public CloudToDeviceTelemetryClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedMqttPublish;
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                throw new NotSupportedException("c2d is not supported when connected to AEG Hub currently");
            }
            else
            {
                //TODO it is a bit awkward to build this now instead of in the constructor, but the deviceId isn't present until after hub connection :/
                var expectedDeviceBoundMessagesTopic = string.Format(CultureInfo.InvariantCulture, DeviceBoundMessagesTopicFormat, _connection.GetCurrentConnectionContext()!.DeviceId);

                if (args.Publish.Topic.StartsWith(expectedDeviceBoundMessagesTopic))
                {
                    if (CloudToDeviceTelemetryReceivedAsync != null)
                    {
                        var receivedCloudToDeviceMessage = new CloudToDeviceTelemetry(args.Publish.Payload);

                        // devices/{device-id}/messages/devicebound/{property-bag}
                        string[] topicSegments = args.Publish.Topic.Split("/", StringSplitOptions.RemoveEmptyEntries);

                        //TODO is there a case where there is no property bag b/c no correlation id + no message id + no user properties?

                        // for example: "%24.mid=febd6d71-df05-474f-8a9b-fe90318c7eb8&%24.to=%2Fdevices%2Fb98a1b81-6b6b-4762-ae77-610fb8d6d3cd%2Fmessages%2FdeviceBound"
                        string propertyBag = topicSegments[4];

                        string[] keyValuePairs = propertyBag.Split('&');
                        foreach (var keyValuePair in keyValuePairs)
                        {
                            string key = Uri.UnescapeDataString(keyValuePair.Split("=")[0]);
                            string value = Uri.UnescapeDataString(keyValuePair.Split("=")[1]);

                            if (key.Equals(TelemetryClient.MessagePropertyMessageId))
                            {
                                receivedCloudToDeviceMessage.MessageId = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyCorrelationId))
                            {
                                receivedCloudToDeviceMessage.CorrelationId = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyContentType))
                            {
                                receivedCloudToDeviceMessage.ContentType = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyContentEncoding))
                            {
                                receivedCloudToDeviceMessage.ContentEncoding = value;
                            }
                            else
                            {
                                receivedCloudToDeviceMessage.UserProperties.Add(key, value);
                            }
                        }

                        await CloudToDeviceTelemetryReceivedAsync.Invoke(receivedCloudToDeviceMessage);
                            
                        await args.AcknowledgeAsync(CancellationToken.None);
                    }
                }
            }
        }
    }
}
