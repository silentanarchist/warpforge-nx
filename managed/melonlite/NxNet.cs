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
    public sealed class NxTlsStream : Stream
    {
        [DllImport("warpforge")] static extern unsafe IntPtr wf_tls_connect(string host, int port, int timeoutMs, byte* err, int errLen);
        [DllImport("warpforge")] static extern unsafe int wf_tls_read(IntPtr h, byte* buf, int len);
        [DllImport("warpforge")] static extern unsafe int wf_tls_write(IntPtr h, byte* buf, int len);
        [DllImport("warpforge")] static extern void wf_tls_close(IntPtr h);
        [DllImport("warpforge")] static extern void wf_tls_abort(IntPtr h);

        IntPtr _h;
        readonly object _closeLock = new();

        NxTlsStream(IntPtr h) { _h = h; }

        public static unsafe NxTlsStream Connect(string host, int port, int timeoutMs = 20000)
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
        public override unsafe int Read(Span<byte> buffer)
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
        public override unsafe void Write(ReadOnlySpan<byte> buffer)
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
        /// <summary>
        /// Replaces WarpforgeRevival.PlayFabTransport.RewriteUrl. The Switch data prep already
        /// points the game's PlayFab host at "<server>/playfab" (so the stock client works), so
        /// the path the mod sees starts with "/playfab/" and the mod's own rewrite doubled it:
        /// "/playfab/playfab/Client/Login..." -- which the server answers with an empty success.
        /// </summary>
        public static string RewriteUrl(string fullUrl, string serverUrl)
        {
            var uri = new Uri(fullUrl);
            var pq = uri.PathAndQuery;
            if (pq.StartsWith("/playfab/", StringComparison.OrdinalIgnoreCase)) pq = pq.Substring("/playfab".Length);
            return serverUrl + "/playfab" + pq;
        }

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
                    // The pool cannot tell that the server has hung up on an idle TLS
                    // stream (it can poll a socket, not our stream), so a stale one is
                    // reused and the request dies with "response ended prematurely".
                    // Keep idle connections for less time than servers do (Node: 5 s).
                    PooledConnectionIdleTimeout = TimeSpan.FromSeconds(4),
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
                var t0 = Environment.TickCount64;
                try { return await _tls.SendAsync(request, ct).ConfigureAwait(false); }
                catch (Exception e)
                {
                    // one line per failure with what was sent: the post-match report failed four
                    // times in a row with "response ended prematurely" while small requests worked
                    long? len = request.Content?.Headers.ContentLength;
                    MelonLogger.Warning("[switch] [https] " + request.Method + " " + uri.AbsolutePath + " body=" + (len.HasValue ? len.Value.ToString() : "?")
                        + " bytes failed after " + (Environment.TickCount64 - t0) + " ms: " + e.GetType().Name + ": " + e.Message
                        + (e.InnerException != null ? " <- " + e.InnerException.GetType().Name + ": " + e.InnerException.Message : ""));
                    throw;
                }
                finally { request.RequestUri = uri; }
            }
        }
    }

    /// <summary>
    /// The mod's encrypted match tunnel, carried by the wrapper (warpforge_tunnel.c) instead of a
    /// TcpListener + SslStream: Mono NX has no SslStream backend, and its accept() path spun.
    /// Takes the place of MatchTunnel.Start(host, port); returns the local port to connect to.
    /// </summary>
    public static class NxTunnel
    {
        [DllImport("warpforge")] static extern int wf_tunnel_start([MarshalAs(UnmanagedType.LPStr)] string host, int port);

        public static int Start(string host, int port)
        {
            int local = wf_tunnel_start(host, port);
            if (local <= 0) throw new IOException("the wrapper could not open the match tunnel");
            MelonLogger.Msg("[switch] [match] matches are encrypted (through the wrapper's TLS to " + host + ":" + port + ")");
            return local;
        }
    }
}
