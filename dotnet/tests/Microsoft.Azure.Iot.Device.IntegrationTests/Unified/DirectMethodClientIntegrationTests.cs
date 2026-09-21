using Microsoft.Azure.Iot.Device.Unified.DirectMethods;
using Microsoft.Azure.Iot.Device.IntegrationTests.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class DirectMethodClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDirectMethods(bool testAgainstClassicHub)
        {
            string expectedDirectMethodName = "someDirectMethod-" + Guid.NewGuid().ToString();
            uint expectedResponseTimeout = 20;

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);
            using DirectMethodClient directMethodClient = new DirectMethodClient(testDeviceContext.ConnectionClient);

            ServiceClient serviceClient = Setup.GetGen1IotHubServiceClient();
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

            var receivedDirectMethod = await DirectMethodReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);

            // The direct method request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.

            // Validate the direct method response that was received by the service client
            Assert.Equal(200, directMethodResponse.Status);
            Assert.NotNull(directMethodResponse.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethodResponse.GetPayloadAsJson());
            Assert.Equal(expectedResponsePayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedResponsePayload.SomeString, responsePayload.SomeString);
            Assert.Equal(1, actualDirectMethodRequestsReceivedCount);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
