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
    /// affects only <see cref="Completed"/>. If the completion callback throws, <see cref="Completed"/> faults
    /// with the callback's exception. Cancellation cancels each task that has not already completed.
    /// </remarks>
    public class CertificateSigningOperation
    {
        private readonly TaskCompletionSource<CertificateSigningRequestAccepted> _accepted
            = new TaskCompletionSource<CertificateSigningRequestAccepted>(TaskCreationOptions.RunContinuationsAsynchronously);

        private readonly TaskCompletionSource<CertificateSigningResponse> _completed
            = new TaskCompletionSource<CertificateSigningResponse>(TaskCreationOptions.RunContinuationsAsynchronously);

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
        /// Thrown when the certificate issuance fails after acceptance, or when IoT hub's issued certificate response
        /// could not be read. It can also be thrown if the request is rejected before <see cref="Accepted"/> completes.
        /// If the <see cref="AbstractConnectionClient.HandleCertificateSigningCompleteAsync"/> callback throws, this
        /// task fails with that exception instead.
        /// </exception>
        public Task<CertificateSigningResponse> Completed => _completed.Task;

        internal void SetAccepted(CertificateSigningRequestAccepted response) => _accepted.TrySetResult(response);

        internal void SetCompleted(CertificateSigningResponse response) => _completed.TrySetResult(response);

        internal void SetFailed(Exception ex)
        {
            _accepted.TrySetException(ex);
            _completed.TrySetException(ex);
        }

        internal void SetCanceled(CancellationToken cancellationToken)
        {
            _accepted.TrySetCanceled(cancellationToken);
            _completed.TrySetCanceled(cancellationToken);
        }
    }
}
