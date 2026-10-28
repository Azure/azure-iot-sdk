// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.CertificateManagement
{
    /// <summary>
    /// Represents a two-phase certificate signing operation.
    /// Phase 1 (Accepted): IoT Hub acknowledges the CSR with a 202 response.
    /// Phase 2 (Completed): IoT Hub delivers the issued certificate with a 200 response.
    /// </summary>
    /// <remarks>
    /// If the operation fails before <see cref="Accepted"/> completes successfully, both tasks fault with the
    /// operation's exception. If <see cref="Accepted"/> has already completed successfully, a later failure
    /// affects only <see cref="Completed"/>. Cancellation cancels each task that has not already completed.
    /// </remarks>
    public class CertificateSigningOperation
    {
        private readonly TaskCompletionSource<CertificateSigningRequestAccepted> _accepted
            = new TaskCompletionSource<CertificateSigningRequestAccepted>(TaskCreationOptions.RunContinuationsAsynchronously);

        private readonly TaskCompletionSource<CertificateSigningResponse> _completed
            = new TaskCompletionSource<CertificateSigningResponse>(TaskCreationOptions.RunContinuationsAsynchronously);

        // Canceled when this operation is canceled, so that sending its request stops even if a send or one of its
        // retries is still in progress.
        private readonly CancellationTokenSource _abandoned = new();

        /// <summary>
        /// A task that completes when IoT Hub accepts the certificate signing request (202 Accepted).
        /// The result contains the correlation ID and operation expiration time.
        /// </summary>
        /// <exception cref="CertificateSigningRequestFailedException">
        /// Thrown when the CSR is rejected by IoT Hub. Inspect <see cref="CertificateSigningRequestFailedException.Error"/>
        /// for the specific failure reason (e.g., 400040 for CSR decode failure, 409005 for an active conflicting operation,
        /// 429002/429003 for throttling).
        /// </exception>
        public Task<CertificateSigningRequestAccepted> Accepted => _accepted.Task;

        /// <summary>
        /// A task that completes when IoT Hub delivers the issued certificate (200 OK).
        /// The result contains the certificate chain and correlation ID.
        /// </summary>
        /// <exception cref="CertificateSigningRequestFailedException">
        /// Thrown when IoT Hub rejects certificate issuance. It can also be thrown if the request is rejected before
        /// <see cref="Accepted"/> completes.
        /// </exception>
        /// <exception cref="DeviceException">
        /// Thrown when the certificate signing response cannot be handled.
        /// </exception>
        public Task<CertificateSigningResponse> Completed => _completed.Task;

        internal CancellationToken Abandoned => _abandoned.Token;

        internal void SetAccepted(CertificateSigningRequestAccepted response) => _accepted.TrySetResult(response);

        internal void SetCompleted(CertificateSigningResponse response)
        {
            _completed.TrySetResult(response);
        }

        internal void SetFailed(Exception ex)
        {
            _accepted.TrySetException(ex);
            _completed.TrySetException(ex);
        }

        internal void SetCanceled(CancellationToken cancellationToken)
        {
            _abandoned.Cancel();
            _accepted.TrySetCanceled(cancellationToken);
            _completed.TrySetCanceled(cancellationToken);
        }
    }
}
