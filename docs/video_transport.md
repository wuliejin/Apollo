# Video pacing and TCP video transport (experimental)

This fork adds two ways to get more video bitrate through links that tail-drop bursts or
police UDP, plus logging to see what the link is doing. All of it is off by default.

## Pacing

By default, each frame goes out at close to line rate. On a link with a ~60 Mbps bottleneck
and a shallow buffer, those per-frame bursts get tail-dropped even when the average bitrate
fits.

| Key | Default | Meaning |
|---|---|---|
| `video_pacing_mbps` | 0 | Releases video packets through a token bucket at this rate in Mbps, counting IP/UDP headers. 0 keeps the stock pacing. Set it well above the stream bitrate (about 1.5-2x) so frames do not queue. |
| `video_pacing_burst_kb` | 8 | Bucket size, which is the largest burst sent back to back. |
| `network_stats_interval` | 10 | Seconds between `Network stats` log lines while streaming. 0 disables them. |

## Video over TCP

Some links police UDP well below what TCP gets. With `video_tcp_port` set, the host
advertises `a=x-ss-video.tcpPort:<port>` in RTSP DESCRIBE. A client that opts in sends
`x-ss-video.tcpConnections:N` in ANNOUNCE and opens N TCP connections to that port.

- Hello (24 bytes): `SSVT`, version `2`, connection index, 2 reserved bytes, then the
  16-byte A/V ping payload from RTSP SETUP. The host answers `SSVK`.
- Each packet is framed as a 16-bit length and a 32-bit sequence number, both big-endian,
  followed by the usual encryption prefix and shard. Each packet goes to the connection with
  the smallest backlog.
- The client puts packets back in sequence order before its RTP queue sees them. That queue
  treats any packet of a newer frame as the end of the current frame.
- When every connection has more than `video_tcp_queue_kb` queued, the packet is dropped. This
  bounds the latency a stalled path can add. For 3 s after a connection attaches, the limit
  is raised to 1 MB.
- If every TCP connection dies, video falls back to UDP.

| Key | Default | Meaning |
|---|---|---|
| `video_tcp_port` | 0 | TCP port for video. 0 disables the transport. Open or forward it like the other ports. |
| `video_tcp_queue_kb` | 32 | Per-connection backlog above which packets are dropped. |
| `video_tcp_sndbuf_kb` | 64 | `SO_SNDBUF` for the video connections. 0 keeps the OS default. |
| `video_tcp_fec_percentage` | -1 | FEC percentage used while video is on TCP. -1 keeps `fec_percentage`. TCP losses come as whole stalls that parity cannot repair, so 0 is a reasonable choice. |

On Windows, each video connection also logs `Video TCP stats` lines from the kernel's
extended TCP statistics. A line covers each connection's queue, cwnd, ssthresh, smoothed
RTT, RTO, fast retransmits, timeouts and send-limit times. They are logged every 100 ms for
5 s after a connection attaches and while packets are dropped, otherwise once a second.

The matching client changes are in the `video-tcp-transport` branches of
[moonlight-common-c](https://github.com/wuliejin/moonlight-common-c) and
[moonlight-qt](https://github.com/wuliejin/moonlight-qt).

## Measurements

These numbers come from one 2560x1440@60 HEVC path whose UDP was policed at ~61 Mbps:

| Transport | Bitrate | Network frame drop |
|---|---|---|
| UDP, stock pacing | 50 Mbps | 1.7-5.9% |
| UDP, `video_pacing_mbps=100` | 50 Mbps | 0.06-0.5% |
| UDP, `video_pacing_mbps=120` | 60 Mbps | 23% (collapse) |
| TCP x4, `video_tcp_fec_percentage=0` | 65-75 Mbps | 0-0.3% between path stalls |
