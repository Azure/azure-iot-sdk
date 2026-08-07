using Microsoft.Azure.Devices.Client.Gen2.DirectMethods;
using Xunit;
using Microsoft.Azure.Devices.Client.IntegrationTests.Models;
using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Models.DirectMethods;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    public class DirectMethodClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDirectMethods()
        {
            string expectedDirectMethodName = "someDirectMethod-" + Guid.NewGuid().ToString();
            uint expectedResponseTimeout = 20;

            await using Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, TestContext.Current.CancellationToken);
            using DirectMethodClient directMethodClient = new DirectMethodClient(testDeviceContext.ConnectionClient);

            ServiceClient serviceClient = Setup.GetIotHubServiceClient();
            var directMethodInvocation = new CloudToDeviceMethod(expectedDirectMethodName, TimeSpan.FromSeconds(expectedResponseTimeout));
            SimpleDirectMethodPayload expectedRequestPayload = new()
            {
                SomeInt = 5,
                SomeString = Guid.NewGuid().ToString(),
            };

            SimpleDirectMethodPayload expectedResponsePayload = new()
            {
                SomeInt = 6,
                SomeString = Guid.NewGuid().ToString(),
            };

            directMethodInvocation.SetPayloadJson(expectedRequestPayload.ToJson());

            int actualDirectMethodRequestsReceivedCount = 0;

            TaskCompletionSource<DirectMethodRequestProbeReceivedEventArgs> ProbeReceivedTcs = new();
            directMethodClient.DirectMethodProbeReceivedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethodName) && args.ResponseTimeoutSeconds == expectedResponseTimeout)
                {
                    //TODO just generate this for the user
                    byte[] readyId = Guid.NewGuid().ToByteArray();

                    ProbeReceivedTcs.TrySetResult(args);

                    return Task.FromResult(new ProbeAck
                    {
                        Ready = new Ready { ReadyId = ByteString.CopyFrom(readyId) }
                    });
                }

                return Task.FromResult(new ProbeAck
                {
                    Rejected = new Rejected { Reason =  RejectedReason.MethodNotFound }
                });
            };

            TaskCompletionSource<DirectMethodRequestReceivedEventArgs> DirectMethodReceivedTcs = new();
            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethodName))
                {
                    if (args.Payload == null)
                    {
                        return Task.FromResult(new DirectMethodResponse() { Status = 400 }); // Unexpected request payload shape
                    }

                    var requestPayload = SimpleDirectMethodPayload.FromJsonBytes(args.Payload);

                    if (requestPayload.SomeString.Equals(expectedRequestPayload.SomeString) && requestPayload.SomeInt == expectedRequestPayload.SomeInt)
                    {
                        actualDirectMethodRequestsReceivedCount++;
                        DirectMethodResponse response = new()
                        {
                            Status = 200,
                            Payload = expectedResponsePayload.ToJsonByteArray(), // Echo back the request payload
                        };
                        
                        DirectMethodReceivedTcs.TrySetResult(args);

                        return Task.FromResult(response);
                    }
                    else
                    { 
                        return Task.FromResult(new DirectMethodResponse() { Status = 400 }); // Unexpected request payload shape
                    }
                }
                else
                {
                    return Task.FromResult(new DirectMethodResponse() { Status = 404 }); // unexpected direct method name
                }
            };

            var directMethodResponse = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.ConnectionContext.DeviceId, directMethodInvocation, TestContext.Current.CancellationToken);

            var receivedProbe = await ProbeReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);
            var receivedDirectMethod = await DirectMethodReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);

            // The direct method probe request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.
            // The direct method request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.

            // Validate the direct method response that was received by the service client
            Assert.Equal(200, directMethodResponse.Status);
            Assert.NotNull(directMethodResponse.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethodResponse.GetPayloadAsJson());
            Assert.Equal(expectedResponsePayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedResponsePayload.SomeString, responsePayload.SomeString);
            Assert.Equal(1, actualDirectMethodRequestsReceivedCount);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanRejectDirectMethodProbe() // Send one DM, reject it, send another DM, check that device's DM callback only sees the second (assumes ordering from service)
        {
        }
    }
}
