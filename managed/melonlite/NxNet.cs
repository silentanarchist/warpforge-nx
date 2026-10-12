// https for mods on the Switch, where the class library has no TLS (Mono NX ships
// no native crypto). The wrapper provides a TLS client on mbedTLS
// (source/warpforge_tls.c); this turns it into a Stream and an HttpClient.
//
// The mod's own code is not changed: NxModShims (at load time) points the mod's
// single HttpClient factory, WarpforgeRevival.Net.Client(TimeSpan, bool), here.
using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace MelonLoader.Switch
{
    /// <summary>One TLS connection made by the wrapper (blocking reads and writes).</summary>
    public sealed unsafe class NxTlsStream : Stream
    {
        [DllImport("warpforge")] static extern IntPtr wf_tls_connect(string host, int port, int timeoutMs, byte* err, int errLen);
        [DllImport("warpforge")] static extern int wf_tls_read(IntPtr h, byte* buf, int len);
        [DllImport("warpforge")] static extern int wf_tls_write(IntPtr h, byte* buf, int len);
        [DllImport("warpforge")] static extern void wf_tls_close(IntPtr h);
        [DllImport("warpforge")] static extern void wf_tls_abort(IntPtr h);

        IntPtr _h;
        readonly object _closeLock = new();

        NxTlsStream(IntPtr h) { _h = h; }

        public static NxTlsStream Connect(string host, int port, int timeoutMs = 20000)
        {
            byte* err = stackalloc byte[512];
            err[0] = 0;
            var h = wf_tls_connect(host, port, timeoutMs, err, 512);
            if (h == IntPtr.Zero)
                throw new IOException("TLS connection to " + host + ":" + port + " failed: " + Marshal.PtrToStringAnsi((IntPtr)err));
            return new NxTlsStream(h);
        }

        public static Task<Stream> ConnectAsync(string host, int port, CancellationToken ct)
            => Task.Run<Stream>(() => Connect(host, port), ct);

        public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));
        public override int Read(Span<byte> buffer)
        {
            if (buffer.Length == 0) return 0;
            var h = _h;
            if (h == IntPtr.Zero) throw new ObjectDisposedException(nameof(NxTlsStream));
            int r;
            fixed (byte* p = buffer) r = wf_tls_read(h, p, buffer.Length);
            if (r < 0) throw new IOException($"TLS read failed (-0x{-r:x})");
            return r;
        }

        public override void Write(byte[] buffer, int offset, int count) => Write(buffer.AsSpan(offset, count));
        public override void Write(ReadOnlySpan<byte> buffer)
        {
            if (buffer.Length == 0) return;
            var h = _h;
            if (h == IntPtr.Zero) throw new ObjectDisposedException(nameof(NxTlsStream));
            int r;
            fixed (byte* p = buffer) r = wf_tls_write(h, p, buffer.Length);
            if (r < 0) throw new IOException($"TLS write failed (-0x{-r:x})");
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default)
        {
            using (ct.Register(() => { var h = _h; if (h != IntPtr.Zero) wf_tls_abort(h); }))
                return await Task.Run(() => Read(buffer.Span), CancellationToken.None).ConfigureAwait(false);
        }
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct)
            => ReadAsync(buffer.AsMemory(offset, count), ct).AsTask();

        public override async ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken ct = default)
        {
            using (ct.Register(() => { var h = _h; if (h != IntPtr.Zero) wf_tls_abort(h); }))
                await Task.Run(() => Write(buffer.Span), CancellationToken.None).ConfigureAwait(false);
        }
        public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken ct)
            => WriteAsync(buffer.AsMemory(offset, count), ct).AsTask();

        public override void Flush() { }
        public override Task FlushAsync(CancellationToken ct) => Task.CompletedTask;

        protected override void Dispose(bool disposing)
        {
            lock (_closeLock)
            {
                var h = _h; _h = IntPtr.Zero;
                if (h != IntPtr.Zero) { wf_tls_abort(h); wf_tls_close(h); }
            }
            base.Dispose(disposing);
        }

        public override bool CanRead => true;
        public override bool CanWrite => true;
        public override bool CanSeek => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }

    /// <summary>HttpClient whose https requests go over the wrapper's TLS.</summary>
    public static class NxHttp
    {
        const int MaxConnections = 8;   // as the mod's own Net.cs
        static NxHttpsHandler s_plain, s_zipped;
        static readonly object s_gate = new();

        public static HttpClient Client(TimeSpan timeout, bool gzip)
        {
            NxHttpsHandler h;
            lock (s_gate) h = gzip ? (s_zipped ??= new NxHttpsHandler(true)) : (s_plain ??= new NxHttpsHandler(false));
            return new HttpClient(h, disposeHandler: false) { Timeout = timeout };
        }

        sealed class NxHttpsHandler : HttpMessageHandler
        {
            // Separate pools: an https request travels as http over a TLS stream, so it
            // must never share a connection with a real plain-http request.
            readonly HttpMessageInvoker _tls, _plain;

            public NxHttpsHandler(bool gzip)
            {
                var tls = new SocketsHttpHandler
                {
                    PooledConnectionIdleTimeout = TimeSpan.FromSeconds(15),
                    MaxConnectionsPerServer = MaxConnections,
                    ConnectCallback = async (ctx, ct) => await NxTlsStream.ConnectAsync(ctx.DnsEndPoint.Host, ctx.DnsEndPoint.Port, ct).ConfigureAwait(false),
                };
                var plain = new SocketsHttpHandler
                {
                    PooledConnectionIdleTimeout = TimeSpan.FromSeconds(15),
                    MaxConnectionsPerServer = MaxConnections,
                };
                if (gzip) { tls.AutomaticDecompression = DecompressionMethods.GZip; plain.AutomaticDecompression = DecompressionMethods.GZip; }
                _tls = new HttpMessageInvoker(tls, false);
                _plain = new HttpMessageInvoker(plain, false);
            }

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
            {
                var uri = request.RequestUri;
                if (uri == null || !uri.Scheme.Equals("https", StringComparison.OrdinalIgnoreCase))
                    return await _plain.SendAsync(request, ct).ConfigureAwait(false);
                var b = new UriBuilder(uri) { Scheme = "http", Port = uri.Port };   // keep the https port (443 or the server's)
                request.RequestUri = b.Uri;
                try { return await _tls.SendAsync(request, ct).ConfigureAwait(false); }
                finally { request.RequestUri = uri; }
            }
        }
    }
}
