using Microsoft.Azure.Iot.Device.Gen2.DirectMethods;
using Xunit;
using Microsoft.Azure.Iot.Device.IntegrationTests.Models;
using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Gen2
{
    public class DirectMethodClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDirectMethods()
        {
            string expectedDirectMethodName = "someDirectMethod-" + Guid.NewGuid().ToString();
            uint expectedResponseTimeout = 20;

            Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
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

            TaskCompletionSource<DirectMethodRequestProbeReceivedEventArgs> ProbeReceivedTcs = new();
            directMethodClient.DirectMethodProbeReceivedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethodName) && args.ResponseTimeoutSeconds == expectedResponseTimeout)
                {
                    //TODO just generate this for the user
                    byte[] readyId = Guid.NewGuid().ToByteArray();

                    ProbeReceivedTcs.TrySetResult(args);

                    return Task.FromResult(DirectMethodProbeAck.Accepted());
                }

                return Task.FromResult(DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound));
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

            var directMethodResponse = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.DeviceId, directMethodInvocation, TestContext.Current.CancellationToken);

            // The direct method probe request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.
            // The direct method request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.
            var receivedProbe = await ProbeReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);
            var receivedDirectMethod = await DirectMethodReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);

            // Validate the direct method response that was received by the service client
            Assert.Equal(200, directMethodResponse.Status);
            Assert.NotNull(directMethodResponse.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethodResponse.GetPayloadAsJson());
            Assert.Equal(expectedResponsePayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedResponsePayload.SomeString, responsePayload.SomeString);
            Assert.Equal(1, actualDirectMethodRequestsReceivedCount);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanRejectDirectMethodProbe() // Send one DM, reject it, send another DM, check that device's DM callback only sees the second (assumes ordering from service)
        {
            string expectedDirectMethod1Name = "someDirectMethod-" + Guid.NewGuid().ToString();
            string expectedDirectMethod2Name = "someDirectMethod-" + Guid.NewGuid().ToString();
            uint expectedResponseTimeout = 20;

            Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
            using DirectMethodClient directMethodClient = new DirectMethodClient(testDeviceContext.ConnectionClient);

            ServiceClient serviceClient = Setup.GetGen1IotHubServiceClient();
            var directMethod1Invocation = new CloudToDeviceMethod(expectedDirectMethod1Name, TimeSpan.FromSeconds(expectedResponseTimeout));
            var directMethod2Invocation = new CloudToDeviceMethod(expectedDirectMethod2Name, TimeSpan.FromSeconds(expectedResponseTimeout));
            SimpleDirectMethodPayload expectedRequest1Payload = new()
            {
                SomeInt = 5,
                SomeString = Guid.NewGuid().ToString(),
            };

            SimpleDirectMethodPayload expectedRequest2Payload = new()
            {
                SomeInt = 7,
                SomeString = Guid.NewGuid().ToString(),
            };

            SimpleDirectMethodPayload expectedResponsePayload = new()
            {
                SomeInt = 6,
                SomeString = Guid.NewGuid().ToString(),
            };

            directMethod1Invocation.SetPayloadJson(expectedRequest1Payload.ToJson());
            directMethod2Invocation.SetPayloadJson(expectedRequest1Payload.ToJson());

            int actualDirectMethodRequestsReceivedCount = 0;

            TaskCompletionSource<DirectMethodRequestProbeReceivedEventArgs> Probe1ReceivedTcs = new();
            TaskCompletionSource<DirectMethodRequestProbeReceivedEventArgs> Probe2ReceivedTcs = new();
            directMethodClient.DirectMethodProbeReceivedAsync += (args) =>
            {
                // Deliberately reject the first direct method invocation to test that flow
                if (args.MethodName.Equals(expectedDirectMethod1Name))
                {
                    Probe1ReceivedTcs.TrySetResult(args);

                    return Task.FromResult(DirectMethodProbeAck.Rejected(RejectedReason.DeviceBusy));
                }
                else if (args.MethodName.Equals(expectedDirectMethod2Name) && args.ResponseTimeoutSeconds == expectedResponseTimeout)
                {
                    byte[] readyId = Guid.NewGuid().ToByteArray();

                    Probe2ReceivedTcs.TrySetResult(args);
                    
                    return Task.FromResult(DirectMethodProbeAck.Accepted());
                }

                return Task.FromResult(DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound));
            };

            TaskCompletionSource<DirectMethodRequestReceivedEventArgs> DirectMethodReceivedTcs = new();
            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                if (args.MethodName.Equals(expectedDirectMethod1Name))
                {
                    return Task.FromResult(new DirectMethodResponse() { Status = 429 }); // This shouldn't be reached during the test since the probe for this direct method was rejected
                }
                else if (args.MethodName.Equals(expectedDirectMethod2Name))
                {
                    if (args.Payload == null)
                    {
                        return Task.FromResult(new DirectMethodResponse() { Status = 400 }); // Unexpected request payload shape
                    }

                    var requestPayload = SimpleDirectMethodPayload.FromJsonBytes(args.Payload);

                    if (requestPayload.SomeString.Equals(expectedRequest2Payload.SomeString) && requestPayload.SomeInt == expectedRequest2Payload.SomeInt)
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

            try
            {
                var directMethod1Response = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.DeviceId, directMethod1Invocation, TestContext.Current.CancellationToken);
                Assert.Fail("Expected the first direct method invocation to fail since the device rejects that invocation's probe message");
            }
            catch (Exception)
            { 
                //TODO what kind of exception will the service client see when a probe message is rejected?
            }
            
            var directMethod2Response = await serviceClient.InvokeDeviceMethodAsync(testDeviceContext.DeviceId, directMethod2Invocation, TestContext.Current.CancellationToken);

            // The direct method probe request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.
            // The direct method request received by the device is validated in the callback itself, so no assertions needed here other than that the TCS completed.
            var receivedProbe1 = await Probe1ReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);
            var receivedProbe2 = await Probe2ReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);
            var receivedDirectMethod = await DirectMethodReceivedTcs.Task.WaitAsync(TestContext.Current.CancellationToken);

            // Validate the direct method response that was received by the service client
            Assert.Equal(200, directMethod2Response.Status);
            Assert.NotNull(directMethod2Response.GetPayloadAsJson());
            SimpleDirectMethodPayload responsePayload = SimpleDirectMethodPayload.FromJson(directMethod2Response.GetPayloadAsJson());
            Assert.Equal(expectedResponsePayload.SomeInt, responsePayload.SomeInt);
            Assert.Equal(expectedResponsePayload.SomeString, responsePayload.SomeString);
            Assert.Equal(1, actualDirectMethodRequestsReceivedCount);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
