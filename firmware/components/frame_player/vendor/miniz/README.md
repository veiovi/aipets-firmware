# Pinned independent DEFLATE decoder

Decoder/header and license are vendored from miniz **3.1.2**,
commit `77d0dce8627735138c51770d1799a1ef48f2117d`:
https://github.com/richgel999/miniz/tree/77d0dce8627735138c51770d1799a1ef48f2117d

The source has four marked hardening guards: reject reserved DEFLATE length codes
286/287 and distance codes 30/31 before table lookup, and dynamic trees exceeding
286 literal/length or 30 distance codes, and reject distances beyond the
zlib-advertised window even for non-wrapping output. These reject streams the
upstream decoder could incorrectly accept but the independent zlib reference
rejects. Regression bitstreams must cover each code. Header is unmodified;
license line endings are normalized to LF. Original source SHA-256:
`2296ebd21ef9af5ebbefd5b454d4bb67d8fb4af2d24945e7fb32114374f75a76`;
header: `da4850920fdf09f8877d9affabe1ebba1852ce4b1ebbd121f2ba7a7ea8e57e5d`.

The local `miniz.h`/`miniz_common.h` adapters expose only freestanding types and
bytewise memory helpers. They disable allocation, unaligned access and 64-bit
bit buffers for identical firmware/WASM paths. The high-level upstream helpers
are not used. Only `tinfl_decompress`, with caller-owned state and exact bounded
output, is used by `frame_codec`. No shared static state or task-stack frame
buffers. Upstream 3.1.2 includes the zero-code-length infinite-loop guard.

Release validation additionally requires zlib method 8, CINFO <=7, no FDICT,
correct FCHECK/Adler-32, exact decoded length, DONE and complete input consumption.
No raw/gzip/concatenated streams or preset dictionaries are accepted.
Compressed input is bounded to 65,828 bytes per resource as well as the
dimension-derived output bound. The compiler selects compressed resources only
when smaller than raw/RLE. All states/buffers are caller-owned and symbols are
prefixed so they cannot interpose ESP-IDF's independent ROM/miniz implementation.
