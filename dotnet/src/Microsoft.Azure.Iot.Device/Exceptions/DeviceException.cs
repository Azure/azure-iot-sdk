// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Runtime.Serialization;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Exceptions
{
    public class DeviceException : Exception
    {
        public DeviceException()
        {
        }

        public DeviceException(string? message) : base(message)
        {
        }

        public DeviceException(string? message, Exception? innerException) : base(message, innerException)
        {
        }

        public required ErrorRetryability Retryability { get; set; }

        public required bool IsContained { get; set; }



    }
}
