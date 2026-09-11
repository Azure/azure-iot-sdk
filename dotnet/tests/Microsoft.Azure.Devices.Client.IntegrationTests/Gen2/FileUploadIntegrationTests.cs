using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using System.Text;
using Xunit;
using Microsoft.Azure.Devices.Client.Gen2.FileUpload;
using Microsoft.Azure.Devices.Client.Models.FileUpload;
using Microsoft.Azure.Devices.Client.Exceptions;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    public class FileUploadIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        public async Task TestFileUpload()
        {
            Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
            using FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);
            //TODO

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        public async Task TestFileUpload_BadFormat()
        {
            Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
            using FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);
            //TODO

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
