using Microsoft.Azure.Devices.Client.CertificateManagement;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    public class FileUploadClient
    {
        public FileUploadClient(ConnectionClient connection)
        { 
        
        }

        public FileUploadClient(HttpClient httpClient, ConnectionContext connectionContext)
        {

        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            throw new NotImplementedException();
        }

        /// <summary>
        /// Signal to IoT hub that the SAS URI retrieved with <see cref="GetFileUploadSasUri(FileUploadSasUriRequest, CancellationToken)"/> is no longer needed 
        /// and should be released.
        /// </summary>
        /// <param name="completion">The notification that includes the SAS URI from <see cref="FileUploadSasUriResponse"/>.</param>
        /// <param name="cancellationToken">the cancellation token</param>
        public Task CompleteFileUploadSasUriAsync(FileUploadCompletionNotification completion, CancellationToken cancellationToken = default)
        {
            throw new NotImplementedException();
        }
    }
}
