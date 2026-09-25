// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using System.Text;
using Xunit;
using Microsoft.Azure.Iot.Device.MQTTv5.FileUpload;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Exceptions;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    public class FileUploadIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        public async Task TestFileUpload()
        {
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
            FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);
            //TODO

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        public async Task TestFileUpload_BadFormat()
        {
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, null, TestContext.Current.CancellationToken);
            FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);
            //TODO

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
