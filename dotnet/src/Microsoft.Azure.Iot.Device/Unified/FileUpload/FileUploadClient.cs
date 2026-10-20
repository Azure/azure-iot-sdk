// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Net.Http.Headers;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Iot.Device.Unified.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private bool _isDisposed = false;
        private bool _isUserSuppliedHttpClientInitialized = false;
        private readonly object _httpClientLock = new();
        private HttpClientLease? _httpClient;
        private readonly IConnectionClient _connection;
        private readonly Func<X509Certificate2?, HttpMessageHandler> _createHttpMessageHandler;

        public FileUploadClient(IConnectionClient connection)
            : this(connection, CreateHttpMessageHandler)
        {
        }

        // Lets tests observe the HTTP clients this client creates for each client certificate.
        internal FileUploadClient(IConnectionClient connection, Func<X509Certificate2?, HttpMessageHandler> createHttpMessageHandler)
        {
            _connection = connection;
            _httpClient = null;
            _createHttpMessageHandler = createHttpMessageHandler;
        }

        /// <summary>
        /// Create a file upload client that uses a user-provided http client when applicable.
        /// </summary>
        /// <param name="connection"></param>
        /// <param name="httpClient">The user-supplied HTTP client for this client to use when applicable</param>
        /// <remarks>
        /// <para>
        /// File upload operations are done over HTTP only when connected to a Gen 1 IoT hub. File upload operations use MQTT when connected to Gen 2 IoT Hubs. Since devices using this "unified" flavor of the file upload 
        /// client may be provisioned to either a Gen 1 IoT Hub or a Gen 2 IoT Hub, the user-supplied HTTP client may not always be used.
        /// </para>
        /// <para>
        /// When the provided <see cref="IConnectionClient"/> authenticates with a client certificate, you must configure the custom HTTP client to present that same certificate.
        /// When it authenticates with a SAS token, this client adds the token to each request's Authorization header. In either case, this client will set the base address of the http client
        /// to target the IoT Hub endpoint correctly, and requests always target the IoT hub the device is currently assigned to. Unlike the HTTP client this client creates for itself,
        /// a user-supplied HTTP client is not replaced when the device's client certificate changes or the device is re-provisioned to a different IoT hub.
        /// </para>
        /// </remarks>
        public FileUploadClient(IConnectionClient connection, HttpClient httpClient)
        {
            _connection = connection;
            _httpClient = new HttpClientLease(httpClient, clientCertificate: null, iotHubHostName: null, ownsClient: false);
            _createHttpMessageHandler = CreateHttpMessageHandler;
        }

        private async Task<HttpResponseMessage> SendAsync(HttpRequestMessage requestMessage, ConnectionContext currentConnectionContext, CancellationToken cancellationToken)
        {
            // Fetched per request so SAS tokens are renewed before they expire and newly issued client certificates are picked up.
            AuthenticationCredentials credentials = await currentConnectionContext.AuthenticationProvider.GetOperationalAuthenticationCredentialsAsync(
                currentConnectionContext.IotHubHostName,
                currentConnectionContext.DeviceId,
                currentConnectionContext.IssuedClientCertificates).ConfigureAwait(false);

            // Providers that authenticate with a SAS token return it as the password; otherwise the client certificate authenticates the request.
            if (credentials.Password.Length > 0)
            {
                requestMessage.Headers.TryAddWithoutValidation("Authorization", Encoding.UTF8.GetString(credentials.Password));
            }

            HttpClientLease lease = AcquireHttpClient(currentConnectionContext, credentials);
            try
            {
                // The default completion option buffers the response content, so it stays readable after the lease is released.
                return await lease.Client.SendAsync(requestMessage, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                lock (_httpClientLock)
                {
                    lease.Release();
                }
            }
        }

        private HttpClientLease AcquireHttpClient(ConnectionContext currentConnectionContext, AuthenticationCredentials credentials)
        {
            lock (_httpClientLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, this);

                if (_httpClient == null)
                {
                    _httpClient = CreateHttpClient(currentConnectionContext, credentials.ClientCertificate);
                }
                else if (_httpClient.OwnsClient
                    && (!IsSameCertificate(_httpClient.ClientCertificate, credentials.ClientCertificate)
                        || !string.Equals(_httpClient.IotHubHostName, currentConnectionContext.IotHubHostName, StringComparison.OrdinalIgnoreCase)))
                {
                    // The client certificate is bound to the handler when the HTTP client is created, so present a changed certificate
                    // (for example, one issued by certificate management) through a new HTTP client. Likewise, an HTTP client's base address
                    // can't change once it has sent a request, so a device re-provisioned to a different IoT hub also needs a new HTTP client.
                    // The old one is disposed once its in-flight requests complete since disposing an HTTP client cancels its pending requests.
                    _httpClient.Release();
                    _httpClient = CreateHttpClient(currentConnectionContext, credentials.ClientCertificate);
                }
                else if (!_httpClient.OwnsClient && !_isUserSuppliedHttpClientInitialized)
                {
                    // If user supplied an HTTP client, just target its base address appropriately. We cannot load the client certificates into this client due to how the .NET HTTP client works, 
                    // so it is the user's responsibility to set that up when creating the HTTP client that they passed in. The base address can't be updated after the first request,
                    // so requests use absolute URIs to follow the device if it is re-provisioned to a different IoT hub.
                    _httpClient.Client.BaseAddress = new Uri("https://" + currentConnectionContext.IotHubHostName);
                    _isUserSuppliedHttpClientInitialized = true;
                }

                _httpClient.AddReference();
                return _httpClient;
            }
        }

        private HttpClientLease CreateHttpClient(ConnectionContext currentConnectionContext, X509Certificate2? clientCertificate)
        {
            HttpClient httpClient = new(_createHttpMessageHandler(clientCertificate))
            {
                BaseAddress = new Uri("https://" + currentConnectionContext.IotHubHostName)
            };

            return new HttpClientLease(httpClient, clientCertificate, currentConnectionContext.IotHubHostName, ownsClient: true);
        }

        private static HttpMessageHandler CreateHttpMessageHandler(X509Certificate2? clientCertificate)
        {
            var handler = new HttpClientHandler();
            if (clientCertificate != null)
            {
                handler.ClientCertificates.Add(clientCertificate);
            }

            return handler;
        }

        private static bool IsSameCertificate(X509Certificate2? first, X509Certificate2? second)
        {
            if (ReferenceEquals(first, second))
            {
                return true;
            }

            if (first == null || second == null)
            {
                return false;
            }

            return first.RawDataMemory.Span.SequenceEqual(second.RawDataMemory.Span);
        }

        /// <summary>
        /// Reference counts an HTTP client so that a replaced client is only disposed after its in-flight requests complete.
        /// Callers must hold <see cref="_httpClientLock"/>.
        /// </summary>
        private sealed class HttpClientLease
        {
            // Starts with the reference held by the file upload client while this is its current HTTP client.
            private int _references = 1;

            public HttpClientLease(HttpClient client, X509Certificate2? clientCertificate, string? iotHubHostName, bool ownsClient)
            {
                Client = client;
                ClientCertificate = clientCertificate;
                IotHubHostName = iotHubHostName;
                OwnsClient = ownsClient;
            }

            public HttpClient Client { get; }

            public X509Certificate2? ClientCertificate { get; }

            /// <summary>
            /// The IoT hub that the base address of an HTTP client created by this file upload client targets.
            /// </summary>
            public string? IotHubHostName { get; }

            /// <summary>
            /// False for a user-supplied HTTP client, which is never replaced or disposed when released.
            /// </summary>
            public bool OwnsClient { get; }

            public void AddReference() => _references++;

            public void Release()
            {
                _references--;
                if (_references == 0 && OwnsClient)
                {
                    Client.Dispose();
                }
            }
        }

        private ConnectionContext GetRequiredConnectionContext()
        {
            return _connection.GetCurrentConnectionContext()
                ?? throw new NotSupportedException("Must connect device prior to using this method");
        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public async Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            ConnectionContext currentConnectionContext = GetRequiredConnectionContext();

            string requestUri = $"https://{currentConnectionContext.IotHubHostName}/devices/{currentConnectionContext.DeviceId}/files?api-version={Unified.Connection.ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await SendAsync(requestMessage, currentConnectionContext, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync())!;
            }
            else
            {
                throw BuildServiceException(
                    "Failed to get the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
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
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            ConnectionContext currentConnectionContext = GetRequiredConnectionContext();

            string requestUri = $"https://{currentConnectionContext.IotHubHostName}/devices/{currentConnectionContext.DeviceId}/files/notifications?api-version={ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await SendAsync(requestMessage, currentConnectionContext, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                throw BuildServiceException(
                    "Failed to complete the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
            }
        }

        private static IotHubServiceException BuildServiceException(string errorMessage, string errorContent)
        {
            try
            {
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        return new IotHubServiceException($"{errorMessage}: {nestedErrorPayload.Message}.")
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
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            // Dispose the underlying HTTP client depending on if it was user-supplied and if the disposing flag is set
            DisposeHttpClient(disposeUserSuppliedHttpClient: disposing);

            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            DisposeHttpClient(disposeUserSuppliedHttpClient: true);

            _connection.Dispose();
        }

        private void DisposeHttpClient(bool disposeUserSuppliedHttpClient)
        {
            lock (_httpClientLock)
            {
                if (_httpClient != null && !_isDisposed)
                {
                    if (_httpClient.OwnsClient)
                    {
                        // Disposed once any in-flight requests complete
                        _httpClient.Release();
                    }
                    else if (disposeUserSuppliedHttpClient)
                    {
                        _httpClient.Client.Dispose();
                    }
                }

                _isDisposed = true;
            }
        }
    }
}
