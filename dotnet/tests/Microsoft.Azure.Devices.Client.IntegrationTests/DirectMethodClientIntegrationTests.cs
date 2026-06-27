using Microsoft.Azure.Devices.Client.DirectMethods;
using MQTTnet.Internal;
using System.Text.Json;
using System.Text.Json.Serialization;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class DirectMethodClientIntegrationTests
    {
        class SimpleDirectMethodPayload 
        {
            [JsonPropertyName("someInt")]
            public int SomeInt { get; set; }

            [JsonPropertyName("someString")]
            public string SomeString { get; set; }

            public static SimpleDirectMethodPayload FromJsonBytes(byte[] json)
            { 
                return JsonSerializer.Deserialize<SimpleDirectMethodPayload>(json);
            }

            public static SimpleDirectMethodPayload FromJson(string json)
            {
                return JsonSerializer.Deserialize<SimpleDirectMethodPayload>(json);
            }


            public byte[] ToJsonByteArray()
            {
                return JsonSerializer.SerializeToUtf8Bytes<SimpleDirectMethodPayload>(this);
            }

            public string ToJson()
            {
                return JsonSerializer.Serialize<SimpleDirectMethodPayload>(this);
            }

        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDirectMethods(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            string expectedDirectMethodName = "someDirectMethod-" + Guid.NewGuid().ToString();

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;
            DirectMethodClient directMethodClient = new DirectMethodClient(connectionClient);

            ServiceClient serviceClient = Setup.GetIotHubServiceClient();
            var directMethodInvocation = new CloudToDeviceMethod(expectedDirectMethodName);
            SimpleDirectMethodPayload expectedRequestPayload = new()
            {
                SomeInt = new Random().Next(0, 10000),
                SomeString = Guid.NewGuid().ToString(),
            };

            directMethodInvocation.SetPayloadJson(expectedRequestPayload.ToJson());


            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethodName))
                {
                    var requestPayload = SimpleDirectMethodPayload.FromJsonBytes(args.Payload);

                    if (requestPayload.SomeString.Equals(expectedRequestPayload.SomeString) && requestPayload.SomeInt == expectedRequestPayload.SomeInt)
                    {
                        DirectMethodResponse response = new()
                        {
                            Status = 200,
                            Payload = requestPayload.ToJsonByteArray(), // Echo back the request payload
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

            var directMethodResponse = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.ConnectionContext.DeviceId, directMethodInvocation);


            Assert.Equal(200, directMethodResponse.Status);

            Assert.NotNull(directMethodResponse.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethodResponse.GetPayloadAsJson());
            Assert.Equal(expectedRequestPayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedRequestPayload.SomeString, responsePayload.SomeString);
        }
    }
}
