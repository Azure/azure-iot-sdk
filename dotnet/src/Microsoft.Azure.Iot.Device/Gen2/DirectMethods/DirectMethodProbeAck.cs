// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Gen2.DirectMethods
{
    public class DirectMethodProbeAck
    {
        internal ProbeAck ProbeAck;

        public static DirectMethodProbeAck Accepted()
        {
            return new DirectMethodProbeAck
            {
                ProbeAck = new()
                {
                    Ready = new()
                    {
                        // Generate a ready Id for the user b/c there is no particular reason that they would care about what value this is
                        ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray())
                    }
                }
            };
        }

        public static DirectMethodProbeAck Rejected(RejectedReason reason)
        {
            return new DirectMethodProbeAck
            {
                ProbeAck = new()
                {
                    Rejected = new()
                    {
                        Reason = reason,
                    }
                }
            };
        }
    }
}
