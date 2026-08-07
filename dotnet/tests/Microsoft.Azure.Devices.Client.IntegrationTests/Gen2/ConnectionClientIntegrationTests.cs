using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    public class ConnectionClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanConnectDirectlyToIotHub()
        {
            Assert.Skip("No AEG hub to test against yet");
        }
    }
}
