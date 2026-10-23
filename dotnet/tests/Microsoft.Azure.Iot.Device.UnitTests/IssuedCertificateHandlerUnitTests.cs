// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class IssuedCertificateHandlerUnitTests
    {
        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Fact]
        public async Task CancellationDoesNotReleaseCallbackOrPreventCachingItsResult()
        {
            using RSA privateKey = RSA.Create();
            CertificateRequest request = new("CN=issued-certificate-test", privateKey, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-1), DateTimeOffset.UtcNow.AddMinutes(5));
            TaskCompletionSource callbackStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<X509Certificate2> completeCallback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int callbackCount = 0;
            IssuedCertificateHandler handler = new(_ =>
            {
                Interlocked.Increment(ref callbackCount);
                callbackStarted.TrySetResult();
                return completeCallback.Task;
            });
            string[] issuedChain = ["issued-certificate"];
            using CancellationTokenSource cancellationTokenSource = new();

            Task<X509Certificate2> canceledRequest = handler.GetCertificateAsync(issuedChain, cancellationTokenSource.Token);
            await callbackStarted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            cancellationTokenSource.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => canceledRequest);

            Task<X509Certificate2> subsequentRequest = handler.GetCertificateAsync(issuedChain, TestContext.Current.CancellationToken);
            Assert.Equal(1, callbackCount);

            completeCallback.SetResult(certificate);
            Assert.Same(certificate, await subsequentRequest.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Same(certificate, await handler.GetCertificateAsync(issuedChain, TestContext.Current.CancellationToken));
            Assert.Equal(1, callbackCount);
        }
    }
}
