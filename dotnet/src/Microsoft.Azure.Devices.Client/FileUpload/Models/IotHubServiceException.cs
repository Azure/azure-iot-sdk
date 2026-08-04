using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.FileUpload.Models
{
    public class IotHubServiceException : Exception
    {
        public IotHubServiceException(string message) : base(message)
        { 
        
        }

        public string? ErrorMessage { get; set; }

        public IotHubNestedServiceException? ErrorDetails { get; set; }
    }
}
