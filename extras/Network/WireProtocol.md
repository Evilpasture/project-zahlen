# Zahlen Wire Protocol v2

`extras/Network/` replaces the MessagePack-based replication protocol with a
compact, self-contained binary format. There are **no third-party
dependencies** on the client: serialization, framing, CRC32 and block
compression are implemented in `Wire.cppm` (module `ZHLN.Wire`) using C++26
static reflection and `std::expected`-based error reporting.

This document is the complete on-the-wire specification, sufficient to
implement the server side in any language (a Python reference follows).

---

## 1. Value encoding (ZHLN.Wire primitives)

All integers are **little-endian** where fixed-width, and all multi-byte
values are explicitly byte-ordered below — the format is host-independent.

| Type            | Encoding |
|-----------------|----------|
| `bool`          | 1 byte, value `0` or `1`. Anything else is rejected. |
| unsigned int   | LEB128 varint (7 bits per byte, LSB first, high bit = continuation). Max 10 bytes; over-long encodings with a trailing zero group are rejected. |
| signed int     | zigzag (`(n << 1) ^ (n >> 63)`) encoded as a varint. |
| `float` / `double` | 4 / 8 bytes IEEE-754, little-endian bit pattern. |
| enum           | its underlying integer (validated against the enumerator list on decode). |
| `optional<T>`  | presence byte (`0`/`1`) followed by `T` if present. |
| string         | varint byte length + UTF-8 bytes. `FixedString<C>` rejects lengths > `C-1`. |
| `vector<T>`    | varint element count + elements. `vector<uint8_t>` and spans of bytes are raw blobs (length + bytes). |
| `array<T, N>` / C array | `N` elements without a count; byte arrays are raw bytes. |
| aggregate      | fields in declaration order. Fields annotated `[[= ZHLN::Wire::Skip {}]]` are **not** encoded and keep their default on decode. |

### Quantization codecs (Jolt math types)

| Type        | Encoding |
|-------------|----------|
| `JPH::Vec3` | three zigzag varints of `int32`: `round(component * 256)` (1/256 m resolution, ±8,388,607 m). Non-finite components are rejected. |
| `JPH::Quat` | four zigzag varints of `int16`: `round(component * 32767)` of the **sign-canonical** representative (`w >= 0`). Decoders must renormalize. |

### Annotations that matter on the wire

* `[[= ZHLN::Wire::Range<Min, Max> {}]]` — value bounds enforced **on decode**;
  violations produce a `ValueOutOfRange` failure naming the field.
* `[[= ZHLN::Wire::Skip {}]]` — field excluded from the wire format.
* `[[= ZHLN::Wire::Version<N> {}]]` — schema version of the message type
  (informational; the envelope carries the actual version byte).
* `[[= ZHLN::Reflect::Description<"…"> {}]]` — human description attached to
  decode failures for that field.

---

## 2. Message envelope

Every message payload is wrapped:

```
offset  size  field
0       1     magic 'Z' (0x5A)
1       1     magic 'W' (0x57)
2       1     protocol version (2)
3       1     message type
4..     …     Wire-encoded message body
```

Message types:

| Value | Name            | Direction        |
|------:|-----------------|------------------|
| 1     | `ClientHello`   | client → server  |
| 2     | `ServerWelcome` | server → client  |
| 3     | `InitialSnapshot` | server → client |
| 4     | `PhysicsBatch`  | server → client (UDP) |
| 5     | `ClientInput`   | client → server (UDP) |

### Message bodies (fields in declaration order)

```
ClientHello:
    u32  protocolVersion     (varint)
    u64  userId              (varint)
    str token                (length + bytes)

ServerWelcome:
    u32  serverTick
    u16  realtimePort
    u8   tickRateHz          (Range<1, 240>)

InitialSnapshot:
    u32  serverTick
    u32  objectCount         (varint)
    objects… each:
        u64  uid
        Vec3 position
        Vec3 size

PhysicsBatch:
    u32  serverTick
    u32  bodyCount           (varint)
    bodies… each:
        u64  uid
        Vec3 position
        Quat rotation
        Vec3 velocity

ClientInput:
    u64  userId
    u32  sequence
    u8   moveFlags           (bit0 forward, bit1 backward, bit2 left,
                              bit3 right, bit4 jump; Range<0, 31>)
    f32  yaw                 (degrees; Range<-1000, 1000>)
```

---

## 3. Transport framing

### TCP stream frame

```
offset  size  field
0       4     frame length, big-endian (counts every byte after this field)
4       1     flags (bit0 = compressed; other bits must be 0)
[5      4     raw payload length, little-endian — only if compressed]
…       …     payload (Wire-encoded envelope) — compressed or raw
last    4     CRC32 (IEEE, reflected) of the *uncompressed* payload, little-endian
```

Frames are self-delimiting: read 4 bytes, then exactly `length` more. A frame
whose CRC32 does not match, whose flags are unknown, whose decompressed size
does not match the announced raw length, or whose length exceeds 128 MiB is
rejected. On rejection the client treats the stream as unrecoverable and
disconnects.

### UDP datagram

Same body as a TCP frame **without** the 4-byte length prefix
(`flags` … `crc32`), since datagrams are self-delimiting. Corrupt datagrams
are dropped individually without affecting the stream.

Compression uses the block format in §4 and is applied automatically when it
shrinks the payload by more than 36 bytes (implementation detail; the flag bit
is authoritative).

---

## 4. Block compression ("mini-LZ4")

Token stream, modeled on the LZ4 block format:

* **token byte**: high nibble = literal length (15 = extended), low nibble =
  match length − 4 (15 = extended).
* literal bytes follow the token.
* unless this is the last sequence: 2-byte **little-endian back-reference
  offset** (1..65535, must not exceed the bytes produced so far), then, if
  extended, match-length extension bytes.
* **length extensions**: while a byte equals 255, add 255 and read another
  byte; the final non-255 byte is added too. (Encoder emits remainder < 255.)
* the final sequence of a block contains literals only.
* matches may overlap the current output (run replication); minimum match
  length is 4, window 64 KiB.
* worst-case (all-literal) size = `n + n/255 + 16` bytes; the decoder rejects
  any expansion beyond the announced raw length.

---

## 5. CRC32

IEEE 802.3 reflected CRC-32 (poly `0xEDB88320`, init `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`). Test vector: `"123456789"` → `0xCBF43926`.

---

## 6. Session flow

1. Client opens TCP to the server, sends `ClientHello` as a stream frame.
2. Server validates and answers `ServerWelcome` (announcing the realtime UDP
   port) as a stream frame.
3. Server sends `InitialSnapshot` as one (possibly compressed) stream frame.
4. Server streams `PhysicsBatch` **datagrams** over UDP at its tick rate.
5. Client sends `ClientInput` datagrams every frame.

---

## 7. Python reference (server side, stdlib only)

```python
import struct, zlib  # zlib only for crc32; the LZ codec below is dependency-free

# ---------- ZHLN.Wire primitives ----------
def w_varint(n: int) -> bytes:
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)

def r_varint(buf: memoryview, pos: int) -> tuple[int, int]:
    value, shift = 0, 0
    count = 0
    while True:
        b = buf[pos]; pos += 1; count += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            if count > 1 and b == 0:
                raise ValueError("non-canonical varint")
            return value, pos
        shift += 7
        if count == 10:
            raise ValueError("varint overflow")

def w_zigzag(n: int) -> bytes:  return w_varint((n << 1) ^ (n >> 63) if n >= 0 else ((n << 1) ^ (-1)) & ((1 << 64) - 1))
def r_zigzag(buf, pos):
    raw, pos = r_varint(buf, pos)
    return (raw >> 1) ^ -(raw & 1), pos

def w_str(s: str) -> bytes:
    raw = s.encode()
    return w_varint(len(raw)) + raw

def w_vec3(xyz) -> bytes:
    return b"".join(w_zigzag(int(round(v * 256))) for v in xyz)

def w_quat(wxyz):
    w, x, y, z = wxyz
    if w < 0: w, x, y, z = -w, -x, -y, -z
    return b"".join(w_zigzag(int(round(v * 32767))) for v in (x, y, z, w))

# ---------- envelope & framing ----------
PROTOCOL_VERSION = 2

def envelope(msg_type: int, body: bytes) -> bytes:
    return bytes((0x5A, 0x57, PROTOCOL_VERSION, msg_type)) + body

def frame(payload: bytes) -> bytes:
    """TCP stream frame (compression omitted — set flag 0)."""
    body = bytes((0,)) + payload + struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)
    return struct.pack(">I", len(body)) + body

def datagram(payload: bytes) -> bytes:
    return bytes((0,)) + payload + struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)

# ---------- server-side session ----------
def server_welcome(tick: int, udp_port: int, tick_hz: int) -> bytes:
    body = w_varint(tick) + w_varint(udp_port) + w_varint(tick_hz)
    return frame(envelope(2, body))

def initial_snapshot(tick: int, objects) -> bytes:
    """objects: iterable of (uid, (x, y, z), (sx, sy, sz))"""
    body = w_varint(tick) + w_varint(len(objects))
    for uid, pos, size in objects:
        body += w_varint(uid) + w_vec3(pos) + w_vec3(size)
    return frame(envelope(3, body))

def physics_batch(tick: int, bodies) -> bytes:
    """bodies: iterable of (uid, (x, y, z), (qx, qy, qz, qw), (vx, vy, vz))"""
    body = w_varint(tick) + w_varint(len(bodies))
    for uid, pos, rot, vel in bodies:
        body += w_varint(uid) + w_vec3(pos) + w_quat(rot) + w_vec3(vel)
    return datagram(envelope(4, body))
```

To support compressed frames, a server additionally needs the ~30-line
decoder for the block format in §4; the flag bit announces it, and the
4-byte little-endian raw length precedes the compressed payload.
