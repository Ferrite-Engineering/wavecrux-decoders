# pcie-tlp output contract (0.1.0)

What the `openpcie.pcie_tlp_down` and `openpcie.pcie_tlp_up` decoders emit.
The two ids are the same decoder, registered twice so a link's two rows get
different names. Golden fixtures and unit tests derive their expected values
from this file and the PCI Express Base Specification, never from the
decoder's output. It follows [`docs/decoder-conventions.md`](../../docs/decoder-conventions.md).

## 1. Input

One TLP DW per clock. Signals, in manifest order:

| Signal | Width | Required | |
|---|---|---|---|
| `clk` | 1 | yes | rising edge (0 -> 1) samples the stream |
| `tlp_data` | 32 | yes | one DW; byte 0 of the TLP (Fmt/Type) in bits [31:24] |
| `tlp_sop` | 1 | yes | first DW of a TLP |
| `tlp_eop` | 1 | yes | last DW of a TLP |
| `tlp_valid` | 1 | yes | `tlp_data` carries a DW at this edge |
| `peer_data`, `peer_sop`, `peer_eop`, `peer_valid` | 32, 1, 1, 1 | no | the opposite link direction; read only when all four are bound |

Values are taken from **just before** each rising edge (the last change
before the edge's timestamp), as a flip-flop captures them. An edge is a
`clk` change from 0 to 1; an X/Z clock is no edge. At each edge the peer
stream is read first, then the main stream.

A **beat** is an edge with `valid` = 1. A beat where `valid` is X/Z, or
where `valid` is 1 and `data`, `sop` or `eop` has an X/Z bit, is an
**unknown beat** (§4). The peer stream only feeds pairing (§5.3) and emits
nothing: an unknown beat drops the peer TLP being collected, a beat
without SOP is skipped, and a SOP restarts collection.

## 2. Parameters

| Name | Values | Default | |
|---|---|---|---|
| `labels` | `compact`, `full` | `compact` | label style (§5) |
| `span` | `packet`, `until_next` | `packet` | transaction end time (§3) |

## 3. Timing

- A TLP starts at its SOP beat's edge and ends at its EOP beat's edge.
- An error starts and ends as §4 says.
- With `span = until_next`, every transaction is held until the next one is
  emitted; the held one's end becomes the next one's start if that is later.
  The last one is released at flush unchanged.

## 4. Errors

An error transaction has `isError` true, label `TLP error: <what>`, and
fields `{"type":"error","error":"<what>", ...}`, with the listed extra
fields after `error`. `<what>` is the exact sentence below.

| `<what>` | When | Start / end | Extra fields |
|---|---|---|---|
| `<signal> is X or Z` | a run of consecutive unknown beats on the main stream. `<signal>` is the first of `tlp_valid`, `tlp_data`, `tlp_sop`, `tlp_eop` that is X/Z in the run's first beat. An unknown beat drops the TLP being collected (no other error for it) | first / last edge of the run | `occurrences` |
| `data beat without SOP` | a run of consecutive beats with `sop` 0 and no TLP open | first / last edge of the run | `occurrences` |
| `SOP before EOP` | a run of consecutive beats with `sop` 1 while a TLP is open. Each drops the open TLP and starts a new one | the first dropped TLP's SOP edge / the last dropped TLP's last beat edge | `dw_count` (of the first dropped TLP), `occurrences` |
| `TLP shorter than its header` | a run of consecutive beats that each end a TLP of fewer DWs than its header (§5: 3, 4 when DW0 Fmt bit 0 is set, 1 for a prefix) | the first such TLP's SOP edge / the last one's EOP edge | `expected`, `actual` (DW counts, of the first), `occurrences` |
| `missing EOP at end of capture` | flush with a TLP open | its SOP edge / its last beat's edge | `dw_count` |

Every error but the last is a **run**: one transaction for consecutive
occurrences of the same error, so a stuck or misbound signal (`tlp_sop`
bound to `valid`, say) gives one error, not one per clock. A run continues
while each next edge has a beat that produces the same error again; it
ends at the first edge that does not (any other beat, a beat with another
error, or an edge without a beat), and at flush. Its transaction is emitted
then, before anything that edge produces. `occurrences` is the number of
beats (X/Z, without SOP) or of TLPs (SOP before EOP, shorter than its
header) in the run, and is always the last field. When it is more than 1,
the label gets ` (<n> beats)` or ` (<n> times)` appended, respectively.

A single beat can give both framing errors: `sop` and `eop` 1 while a TLP is
open drops the open TLP (`SOP before EOP`) and ends a 1-DW TLP (`TLP
shorter than its header`); the first ends its run when the second starts.

## 5. Decoded TLPs

`Fmt` = DW0[31:29], `Type` = DW0[28:24], `Length` = DW0[9:0]. The header is
3 DW, or 4 when Fmt bit 0 is set; a TLP prefix (Fmt bit 2 set, PCIe Base
Spec §2.2.1) is 1 DW. A TLP carries data when Fmt is 010 or 011; `data` is
the first DW after the header. `N` = Length, or 1024 for 0. The ECRC DW of
a TLP with TD set is counted (§5.6), never decoded.

The **tag** is 10 bits (§2.2.6.2): T9 = DW0 bit 23, T8 = DW0 bit 19, and the
low 8 bits from DW1[15:8] (requests) or DW2[15:8] (completions). `<TT>` is
the tag in hex, at least 2 digits (3 for a tag above 0xFF). These fields
are those of non-Flit Mode TLPs: a Flit Mode header (PCIe 6.0 and later)
has another layout, with a 14-bit tag, and is not decoded correctly.

Kinds, by Type and Fmt; any other combination, and every prefix, is §5.5:

| Kind | Type | Fmt |
|---|---|---|
| configuration (§5.1) | 0x04, 0x05 | 000, 010 (3 DW) |
| memory (§5.2) | 0x00 | any of 000..011 |
| locked memory read (§5.2) | 0x01 | 000, 001 (no data: there is no locked write) |
| I/O (§5.2) | 0x02 | 000, 010 (3 DW) |
| completion (§5.3) | 0x0A, 0x0B | 000, 010 (3 DW) |
| message (§5.4) | 1_0rrr | 001, 011 (4 DW) |

Hex in labels and fields is upper case. Field values written `0x<k>` are
JSON strings with exactly k hex digits; numbers are JSON numbers. A BDF
(`<BDF>`) is a 16-bit ID written `BB:DD.F`: bus [15:8], device [7:3],
function [2:0], bus and device as 2 hex digits, function as 1 decimal digit.

Fields of every decoded TLP, in this order, then the per-kind fields, then
the error fields (§5.6):

| Key | Value |
|---|---|
| `type` | the TLP name (below) |
| `fmt` | Fmt, number |
| `type_code` | Type, `0x<2>` |
| `length` | Length, number |
| `dw_count` | DWs received, number |
| `header` | the header DWs as 8 hex digits each, space-separated (string, no `0x`); 3 DWs, 4 for a 4 DW header, 1 for a prefix |

### 5.1 Configuration requests (Type 0x04, 0x05)

Name: `CfgRd<T>` or `CfgWr<T>` (with data), `T` = Type bit 0. From DW1:
requester = [31:16], first BE = [3:0]; the tag as above. From DW2: bus = [31:24],
device = [23:19], function = [18:16], register = DW2 & 0xFFC. `value` = the
byte-swapped `data` (the register value as the spec prints it).

Register names: 0x00 `ID`, 0x04 `Command`, 0x08 `Class/Rev`, 0x0C
`Hdr/Lat/CLS`, 0x10..0x24 `BAR0`..`BAR5`, 0x28 `CardBus`, 0x2C `Subsystem`,
0x30 `ExpROM`, 0x34 `CapPtr`, 0x3C `Int`; any other is `reg 0x<3>`. The
names are those of a Type 0 header: the decoder does not learn a function's
header type, so on a Type 1 header (a bridge or switch port, where 0x18 to
0x24 hold bus numbers and windows) `BAR2` to `BAR5` are only the offsets.

| | Read | Write |
|---|---|---|
| full label | name + ` <BB>:<DD>.<F> <reg>  tag <TT>` | name + ` <BB>:<DD>.<F> <reg> = 0x<value, 8>  tag <TT>` |
| compact | `CfgRd[1][ <BDF>] <reg>` | `CfgWr[1][ <BDF>] <reg> = 0x<value, 8>` |

In compact, `1` appears for Type 1 only,
and ` <BB>:<DD>.<F>` unless it is 01:00.0 and Type 0. Fields: `bus`,
`device`, `function` (numbers), `register` (`0x<3>`), `register_name`,
`requester` (BDF), `tag` (number), `first_be` (`0x<1>`); a write adds
`value` and `data` (both `0x<8>`).

### 5.2 Memory and I/O requests (Type 0x00, 0x01, 0x02)

Name: `M` (or `IO` for Type 0x02) + `Rd`/`Wr` + `Lk` (Type 0x01 only) +
`32`/`64` by header size (nothing for IO). From DW1: requester, first
BE = [3:0], last BE = [7:4]; the tag as in §5. Address: with a 4 DW header, high = DW2 and
low = DW3; else high = 0 and low = DW2; low has bits [1:0] cleared. `<A>` =
`0x<low, 8>`, or `0x<high, 8>_<low, 8>` when high is not 0.

| | Read | Write |
|---|---|---|
| full label | name + ` <A>  <N> DW  tag <TT>` | name + ` <A> = 0x<data, 8>`, + ` +<N-1> DW` when N > 1 |
| compact | name + ` <A>`, + ` x<N> DW` when N is not 1 | name + ` <A> = 0x<data, 8>`, + ` ...` when N > 1 |

Fields: `address` (`0x<8>` from a 3 DW header, `0x<16>` = high and low from
a 4 DW header), `length_dw` (`N`), `requester` (BDF), `tag`, `first_be`,
`last_be` (`0x<1>`); a write adds `data` (`0x<8>`).

### 5.3 Completions (Type 0x0A, 0x0B)

Name: `Cpl`, `CplD` (with data), `CplLk`, `CplDLk` (Type 0x0B). From DW1:
completer = [31:16], status = [15:13], byte count = [11:0], where 0
means 4096 (PCIe Base Spec §2.2.9). From DW2:
requester = [31:16], lower address = [6:0]; the tag as in §5. Status names: 0
`SC`, 1 `UR`, 2 `CRS`, 4 `CA`, else `rsvd`.

**Pairing.** Each request that ends on either stream is remembered under
its (requester, tag) pair: configuration requests (register, read or
write), memory reads (low address; memory writes are not remembered), and
I/O requests (DW2 with bits [1:0] cleared). A later request with the same
requester and tag replaces the entry, which keeps its place in the order
below. Up to 4 requesters are remembered per
tag; a fifth replaces the oldest entry of that tag. A completion **matches**
the entry with its requester and tag. A match is removed, unless the
completion has data and byte count + (lower address & 3) > 4 x N: more
split completions follow (§2.3.1.1).

`<what>`, for a match: the register name for a configuration read, `for
write <reg>` for a configuration write, `MRd 0x<addr, 8>` for a memory read
(locked or not), `IORd 0x<addr, 8>` or `IOWr 0x<addr, 8>` for I/O. `shown` = byte-swapped `data` for a configuration match, else `data`.

| | With data | Without |
|---|---|---|
| full label | name + ` <status>[ <what>] = 0x<shown, 8>  tag <TT>` | name + ` <status>[ <what>]  tag <TT>` |
| compact | `CplD[ <status>][ <c>] = 0x<shown, 8>` | `Cpl[ <status>][ <c>]` |

In compact, `<status>` appears unless it is SC, `<c>` is the register name
for a configuration match and `0x<addr, 8>` for memory or I/O, and there is
no `Lk`. Fields: `status` (name), `completer`, `requester` (BDF), `tag`,
`byte_count` (number, 4096 for a field of 0), `lower_address` (`0x<2>`); with data, `data`
(`0x<8>`); for a configuration match with data, `value` (`0x<8>`, = shown);
for a match, `answers` (`<what>`).

Status UR or CA makes the transaction an error (§5.6) with `error`
`Unsupported Request` or `Completer Abort`.

### 5.4 Messages (Type 1_0rrr)

Name: `Msg` or `MsgD`. `code` = DW1[7:0], `route` = Type[2:0]. Both label
styles: name + ` code 0x<code, 2>  route <route>`. Fields: `message_code`
(`0x<2>`), `routing` (number).

### 5.5 Anything else

TLP prefixes and every Type/Fmt combination the table in §5 does not list
(a locked memory write, a 4 DW configuration, I/O or completion header, a
message with a 3 DW header, reserved Types). Name: `TLP`. Both label
styles: `TLP Fmt <Fmt> Type 0x<Type, 2>`. No per-kind fields; the length
check of §5.6 still applies, except to a prefix. A frame that starts with
a prefix is shown this way as a whole (its `header` is the prefix DW, its
length is not checked); the TLP after the prefix is not decoded.

### 5.6 Errors on a decoded TLP

The TLP is still decoded, `isError` is true, and these fields come last:

- **Length mismatch.** Unless Fmt bit 2 is set (a TLP prefix), the expected
  DW count is header + (`N` when the TLP has data) + (1 when TD, DW0 bit 15,
  is set: the ECRC DW, PCIe Base Spec §2.2.3). A different count adds
  `"error":"length does not match the Length field","expected":<m>,
  "actual":<n>`, and both label styles get `  [<n> DW, expected <m>]`.
- **UR / CA completion** (§5.3): `"error":"Unsupported Request"` or
  `"Completer Abort"`. A completion that also has a length mismatch reports
  both: `error` is the status sentence, `; `, then the length sentence
  (`"Unsupported Request; length does not match the Length field"`),
  followed by `expected` and `actual`; the label is as for the length
  mismatch.
