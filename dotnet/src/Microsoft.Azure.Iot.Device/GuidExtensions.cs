using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Text;

namespace Microsoft.Azure.Iot.Device
{
    public static class GuidExtensions
    {
        public static bool TryParseBytes(byte[]? bytes, [NotNullWhen(returnValue: true)] out Guid? result)
        {
            result = null!;
            if (bytes == null || bytes.Length != 16)
            {
                return false;
            }
            try
            {
                result = new Guid(bytes, bigEndian: true);
                return true;
            }
            catch (Exception ex)
            {
                Trace.TraceInformation(ex.Message);
                return false;
            }
        }
    }
}
