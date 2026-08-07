using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Net.Http.Headers;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private HttpClient? _httpClient; //TODO some documentation explaining why this override is only sometimes applicable since file upload is done over MQTT on Gen2 hub.
        private readonly IConnectionClient _connectionClient;
        private readonly X509Certificate2 _clientCertificate;
        private bool _isInitialized = false;
        private Gen2.FileUpload.FileUploadClient _aegFileUploadClient;

        public FileUploadClient(IConnectionClient connection, X509Certificate2 clientCertificate) //TODO passing in the cert for AEG case makes no sense. Clean this up later
        {
            _connectionClient = connection;
            _clientCertificate = clientCertificate;
            _httpClient = null;
            _aegFileUploadClient = new(new Gen2.Connection.ConnectionClient(_connectionClient));
        }

        public FileUploadClient(IConnectionClient connection, X509Certificate2 clientCertificate, HttpClient httpClient)
        {
            _connectionClient = connection;
            _httpClient = httpClient;
            _clientCertificate = clientCertificate;
            _aegFileUploadClient = new(new Gen2.Connection.ConnectionClient(_connectionClient));
        }

        private void InitializeIfUninitialized()
        {
            if (_isInitialized)
            {
                return;
            }

            var currentConnectionContext = _connectionClient.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            var handler = new HttpClientHandler();
            handler.ClientCertificates.Add(_clientCertificate); // TODO what about when this gets rotated by cert management APIs?
            handler.ServerCertificateCustomValidationCallback = (message, cert2, chain, errors) => true;
            _httpClient = new(handler)
            {
                BaseAddress = new Uri("https://" + currentConnectionContext.IotHubHostName)
            };

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

            var currentConnectionContext = _connectionClient.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (currentConnectionContext.IsAzureEventGrid)
            {
                return await _aegFileUploadClient.GetFileUploadSasUriAsync(request, cancellationToken);
            }

            string requestUri = $"devices/{currentConnectionContext.DeviceId}/files?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient!.SendAsync(requestMessage, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync())!;
            }
            else
            {
                string errorContent = await httpResponse.Content.ReadAsStringAsync();
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        var exception = new IotHubServiceException($"Failed to get the file upload Sas Uri: {nestedErrorPayload.Message}.")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                        throw exception;
                    }
                }

                throw new IotHubServiceException($"Received an error message from IoT hub with an unexpected format: {errorContent}");
            }
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

            var currentConnectionContext = _connectionClient.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (currentConnectionContext.IsAzureEventGrid)
            {
                await _aegFileUploadClient.CompleteFileUploadSasUriAsync(completion, cancellationToken);
                return;
            }

            string requestUri = $"devices/{currentConnectionContext.DeviceId}/files/notifications?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                string errorContent = await httpResponse.Content.ReadAsStringAsync();
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        var exception = new IotHubServiceException($"Failed to complete the file upload Sas Uri: {nestedErrorPayload.Message}")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                        throw exception;
                    }
                }

                throw new IotHubServiceException($"Received an error message from IoT hub with an unexpected format: {errorContent}");
            }
        }

        public void Dispose()
        {
            if (_httpClient != null)
            {
                _httpClient.Dispose();
            }
        }
    }
}
