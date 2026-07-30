using Microsoft.Azure.Devices.Client.IotHub;
using System.Net.Http.Headers;
using System.Runtime.ConstrainedExecution;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private HttpClient? _httpClient;
        private readonly IConnectionClient _connectionClient;
        private readonly X509Certificate2 _clientCertificate;
        private readonly bool _userProvidedHttpClient;
        private bool _isInitialized = false;

        public FileUploadClient(IConnectionClient connection, X509Certificate2 clientCertificate) //TODO passing in the cert for AEG case makes no sense. Clean this up later
        {
            _connectionClient = connection;
            _clientCertificate = clientCertificate;
            _httpClient = null;
            _userProvidedHttpClient = false;
        }

        public FileUploadClient(IConnectionClient connection, X509Certificate2 clientCertificate, HttpClient httpClient)
        {
            _connectionClient = connection;
            _httpClient = httpClient;
            _clientCertificate = clientCertificate;
            _userProvidedHttpClient = true;
        }

        private void InitializeIfUninitialized()
        {
            if (_isInitialized)
            {
                return;
            }

            ConnectionContext? connectionContext = _connectionClient.GetCurrentConnectionContext();
            if (connectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (connectionContext.IsAzureEventGrid && _userProvidedHttpClient)
            {
                throw new NotSupportedException("File upload APIs are done over MQTT when connected to AEG Hub");
            }

            if (!_userProvidedHttpClient)
            {
                // Only build a client when the caller did not supply one; replacing a
                // caller-provided HttpClient would silently discard its handler, and with
                // it any proxy, connection pooling or retry policy it was configured with.
                var handler = new HttpClientHandler();
                handler.ClientCertificates.Add(_clientCertificate); // TODO what about when this gets rotated by cert management APIs?

                // The hub's server certificate is validated against the platform's default
                // trust chain. Do NOT install an accept-everything callback here: it would
                // silently opt every application out of server authentication.
                _httpClient = new(handler);
            }

            // A caller-provided client may already be pointed somewhere; only fill in the
            // hub address when it has none (re-assigning after a request would throw).
            _httpClient!.BaseAddress ??= new Uri("https://" + connectionContext.IotHubHostName);

            _isInitialized = true;
        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public async Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            InitializeIfUninitialized();

            string requestUri = $"devices/{_connectionClient.GetCurrentConnectionContext().DeviceId}/files?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync())!;
            }

            throw BuildServiceException(
                "get the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
        }

        /// <summary>
        /// Turns an IoT hub error response into an <see cref="IotHubServiceException"/>.
        /// </summary>
        /// <remarks>
        /// The hub's own errors carry a nested JSON payload with a service error code, which
        /// callers switch on. Anything else -- a gateway error page, a truncated body, a
        /// payload missing fields -- must still surface as this SDK's exception type instead
        /// of a raw <see cref="JsonException"/> that no caller is looking for.
        /// </remarks>
        private static IotHubServiceException BuildServiceException(string operation, string errorContent)
        {
            try
            {
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        return new IotHubServiceException($"Failed to {operation}: {nestedErrorPayload.Message}.")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                    }
                }
            }
            catch (JsonException)
            {
                // Not an error shape this SDK recognizes; fall through to the generic report.
            }

            return new IotHubServiceException(
                $"Received an error message from IoT hub with an unexpected format: {errorContent}");
        }

        /// <summary>
        /// Signal to IoT hub that the SAS URI retrieved with <see cref="GetFileUploadSasUri(FileUploadSasUriRequest, CancellationToken)"/> is no longer needed 
        /// and should be released.
        /// </summary>
        /// <param name="completion">The notification that includes the SAS URI from <see cref="FileUploadSasUriResponse"/>.</param>
        /// <param name="cancellationToken">the cancellation token</param>
        public async Task CompleteFileUploadSasUriAsync(FileUploadCompletionNotification completion, CancellationToken cancellationToken = default)
        {
            InitializeIfUninitialized();

            string requestUri = $"devices/{_connectionClient.GetCurrentConnectionContext().DeviceId}/files/notifications?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                throw BuildServiceException(
                    "complete the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
            }
        }

        public void Dispose()
        {
            // Only dispose the client this instance created: a caller-provided HttpClient
            // is typically shared and outlives this client.
            if (!_userProvidedHttpClient)
            {
                _httpClient?.Dispose();
            }
        }
    }
}
