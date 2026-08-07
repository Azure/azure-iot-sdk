using Microsoft.Azure.Devices.Client.DirectMethods.Unified;
using System.Text.Json;
using System.Text.Json.Serialization;
using Xunit;
using Microsoft.Azure.Devices.Client.IntegrationTests.Models;
using Microsoft.Azure.Devices.Client.Models;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class DirectMethodClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDirectMethods(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            string expectedDirectMethodName = "someDirectMethod-" + Guid.NewGuid().ToString();

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            using DirectMethodClient directMethodClient = new DirectMethodClient(testDeviceContext.ConnectionClient);

            ServiceClient serviceClient = Setup.GetIotHubServiceClient();
            var directMethodInvocation = new CloudToDeviceMethod(expectedDirectMethodName);
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

            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethodName))
                {
                    var requestPayload = SimpleDirectMethodPayload.FromJsonBytes(args.Payload);

                    if (requestPayload.SomeString.Equals(expectedRequestPayload.SomeString) && requestPayload.SomeInt == expectedRequestPayload.SomeInt)
                    {
                        actualDirectMethodRequestsReceivedCount++;
                        DirectMethodResponse response = new()
                        {
                            Status = 200,
                            Payload = expectedResponsePayload.ToJsonByteArray(), // Echo back the request payload
                        };

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

            var directMethodResponse = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.ConnectionContext.DeviceId, directMethodInvocation, cts.Token);


            Assert.Equal(200, directMethodResponse.Status);

            Assert.NotNull(directMethodResponse.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethodResponse.GetPayloadAsJson());
            Assert.Equal(expectedResponsePayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedResponsePayload.SomeString, responsePayload.SomeString);
            Assert.Equal(1, actualDirectMethodRequestsReceivedCount);
        }
    }
}
