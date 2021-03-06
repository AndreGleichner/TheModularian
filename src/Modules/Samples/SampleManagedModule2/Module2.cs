using System;
using ManagedModuleContract;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;

namespace SampleManagedModule1
{
    public class Module2 : IModule, IDisposable
    {
        private readonly IConfigurationRoot _config;
        private readonly ILogger _logger;
        private bool _disposed;
        private IModuleHost _moduleHost;

        public Module2(IConfigurationRoot config, ILogger logger)
        {
            _config = config;
            _logger = logger;
        }

        #region IModule
        public bool Initialize(IModuleHost moduleHost)
        {
            _moduleHost = moduleHost;
            return true;
        }

        public bool OnMessageFromHost(string msg, Guid service, int session)
        {
            _logger.LogInformation($"{nameof(Module2)}: Received event {msg}");
            return true;
        }
        #endregion

        #region Dispose
        protected virtual void Dispose(bool disposing)
        {
            if (!_disposed)
            {
                if (disposing)
                {
                    // dispose managed state (managed objects)
                    _moduleHost = null;
                }

                // free unmanaged resources (unmanaged objects) and override finalizer
                // set large fields to null
                _disposed = true;
            }
        }

        // // override finalizer only if 'Dispose(bool disposing)' has code to free unmanaged resources
        // ~Module2()
        // {
        //     // Do not change this code. Put cleanup code in 'Dispose(bool disposing)' method
        //     Dispose(disposing: false);
        // }

        public void Dispose()
        {
            // Do not change this code. Put cleanup code in 'Dispose(bool disposing)' method
            Dispose(disposing: true);
            GC.SuppressFinalize(this);
        }
        #endregion
    }
}
