using DirectMethodsClientSample;
using Microsoft.Azure.Devices;
using System.Text.Json;

// Wait for the device side sample to boot up first
await Task.Delay(TimeSpan.FromSeconds(5));

ServiceClient serviceClient = ServiceClient.CreateFromConnectionString(System.Environment.GetEnvironmentVariable("SAMPLE_IOTHUB_CONNECTION_STRING") ?? throw new Exception("TODO"));
string deviceId = System.Environment.GetEnvironmentVariable("SAMPLE_DEVICE_ID") ?? "iothubx509device1"; // This is the default device name when you run the GenerateTestCertificate.ps1 script

var directMethodRequest = new CloudToDeviceMethod("testMethod");
DirectMethodRequestPayloadObject requestPayload = new() { SomeIntegerField = 5, SomeStringField = "Hello!" };

directMethodRequest.SetPayloadJson(JsonSerializer.Serialize(requestPayload));

Console.WriteLine("Sending direct method request"); 
var directMethodResponse = await serviceClient.InvokeDeviceMethodAsync(deviceId, directMethodRequest);

DirectMethodResponsePayloadObject? responsePayload = JsonSerializer.Deserialize<DirectMethodResponsePayloadObject>(directMethodResponse.GetPayloadAsJson());
Console.WriteLine("Received response");

await Task.Delay(-1);