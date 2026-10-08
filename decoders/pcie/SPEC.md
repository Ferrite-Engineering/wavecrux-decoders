# PCIe decoders: output specification (v0.1)

This is the contract between the C decoders in `src/` and the independent
fixture generator in `tools/`. Both are written from this document and the
PCI Express Base Specification (Gen1/Gen2, 8b/10b encoding); neither reads
the other's code. Where they disagree, the one that departs from this
document (or from the PCIe specification, if this document is silent) is
wrong.

Protocol facts and verified known answers (K-codes, ordered-set layout,
scrambler keys, DLLP CRC-16, TLP LCRC-32) are in `docs/protocol-notes.md`.

## 1. One plugin, eight decoders

The plugin library is `wcx_pcie`; plugin name "PCIe decoders (PIPE, Data
Link Layer)"; description "Open source, Apache-2.0. Ferrite Engineering,
github.com/Ferrite-Engineering/wavecrux-decoders".

WaveCrux packs each signal at its declared width and auto-binds only signals
of that width, so each decoder is registered once per PIPE data width:

| Id | Display name | `data` | `datak` |
|---|---|---|---|
| `ferrite.pcie_pipe_w8` | PCIe PIPE (8-bit) | 8 | 1 |
| `ferrite.pcie_pipe_w16` | PCIe PIPE (16-bit) | 16 | 2 |
| `ferrite.pcie_pipe_w32` | PCIe PIPE (32-bit) | 32 | 4 |
| `ferrite.pcie_pipe_w64` | PCIe PIPE (64-bit) | 64 | 8 |
| `ferrite.pcie_dll_w8` | PCIe Data Link Layer (8-bit) | 8 | 1 |
| `ferrite.pcie_dll_w16` | PCIe Data Link Layer (16-bit) | 16 | 2 |
| `ferrite.pcie_dll_w32` | PCIe Data Link Layer (32-bit) | 32 | 4 |
| `ferrite.pcie_dll_w64` | PCIe Data Link Layer (64-bit) | 64 | 8 |

N = symbols per clock = width / 8. Byte lane j (bits `8j+7:8j` of `data`,
bit j of `datak`) is the j-th symbol on the wire. x1 links only.

## 2. Signals (both decoders, in this manifest order)

| Name | Width | Required | Meaning |
|---|---|---|---|
| `pclk` | 1 | yes | PIPE parallel clock; sampled on rising edges |
| `data` | 8N | yes | PIPE TxData or RxData (one direction per decoder instance) |
| `datak` | N | yes | PIPE TxDataK or RxDataK; 1 = K symbol |
| `rxvalid` | 1 | no | When bound and 0 at a sampled edge, that edge's symbols are ignored. Unbound = always valid. |
| `txelecidle` | 1 | no | When bound and 1 at a sampled edge, that edge's symbols are ignored. Unbound = never idle. |
| `rxstatus` | 3 | no | PIPE RxStatus; PIPE decoder only (§4.6). The DLL decoder declares it and ignores it, so one binding set fits both. |

## 3. Parameters

| Name | Kind | Values | Default | Decoders |
|---|---|---|---|---|
| `scrambling` | enum | `auto`, `on`, `off` | `auto` | both |
| `sample_point` | enum | `before_edge`, `at_edge` | `before_edge` | both |
| `coalesce` | bool | | `true` | PIPE |
| `show_idle` | bool | | `false` | PIPE |

`enum_labels`: scrambling `{"auto": "Detect", "on": "On (descramble)", "off":
"Off"}`; sample_point `{"before_edge": "Before the clock edge", "at_edge": "At
the clock edge"}`.

## 4. Sampling and time

### 4.1 Edges

A rising edge of `pclk` is a sample where `pclk` goes from known 0 to known 1.
With `sample_point=before_edge`, every other signal is read from the host
sample **before** the one in which `pclk` rose (the values a flip-flop
captures in a zero-delay RTL dump). With `at_edge`, from the same sample.

### 4.2 Symbol times

Let `t_k` be the timestamp (fs) of the k-th rising edge and `P_k = t_k -
t_{k-1}` (P_1 = 0: the first edge has no measured period). The symbol in lane
j captured at edge k has time `s = t_k + floor(j * P_k / N)` and duration
`d = floor(P_k / N)`.

A transaction covering symbols `a..b` has `start_fs = s(a)` and `end_fs = s(b)
+ d(b)`. A coalesced run covers its first to its last symbol. All arithmetic
is unsigned 64-bit integer.

(The host then converts to ticks: start rounded down, end rounded up.)

### 4.3 Gating

An edge is **gated** when `rxvalid` is bound and samples 0, or `txelecidle`
is bound and samples 1. Gated edges contribute no symbols. If a packet is open
when gating begins, it is reported as truncated (§6) and the framer resets.

### 4.4 Unknown values

If any bit of `data` or `datak` sampled at an edge is X or Z (and the edge is
not gated), the edge contributes no symbols, an open packet is dropped
silently, and one error transaction is emitted per **run** of such edges:
label `X/Z on data`, fields `type: "unknown_value"`, `edges: <run length>`,
spanning the run's symbol times.

## 5. Shared symbol processing

### 5.1 K-code classification

COM 0xBC, STP 0xFB, SDP 0x5C, END 0xFD, EDB 0xFE, PAD 0xF7, SKP 0x1C, FTS 0x3C,
IDL 0x7C, EIE 0xFC. Any other byte with K=1 is an **invalid K symbol** (§6).

### 5.2 Descrambler

As in `docs/protocol-notes.md` §3: LFSR x^16+x^5+x^4+x^3+1, seed 0xFFFF, reset
by COM, advanced by every symbol except SKP, applied to D symbols only, and not
applied to the 15 symbols that follow COM in a TS1/TS2 (they still advance
it). Before the first COM is seen the descrambler is unsynchronised: in
`on` mode, D symbols before the first COM are discarded without output.

### 5.3 Ordered sets

COM followed by:

- **PAD or a D symbol**: a TS. Collect symbols 1-15. Symbol 6 = 0x4A → TS1,
  0x45 → TS2, otherwise an `unknown_os` error. Fields from symbols 1-5 per
  protocol notes §2.
- **SKP** (1 to 5 of them; stop at the first non-SKP): SKP ordered set.
- **FTS ×3**: FTS ordered set. **IDL ×3**: EIOS. **EIE ×14 then 0x4A (D)**:
  EIEOS.
- Anything else: `unknown_os` error, resynchronise at the next symbol.

An ordered set cannot appear inside a packet; COM inside a packet truncates
the packet (§6) and starts the ordered set.

### 5.4 Packet framing

STP opens a TLP, SDP opens a DLLP. The packet closes at END (good) or EDB
(nullified TLP / PHY error). Bytes between the start and the end symbol are
the packet body (STP/SDP and END/EDB excluded).

Outside a packet, after descrambling, a D symbol 0x00 is **logical idle**.
Any other D symbol outside a packet and outside an ordered set is **stray
data** (§6).

### 5.5 Scrambling mode

- `off`: no descrambling. `on`: descramble per §5.2.
- `auto`: if a TS1/TS2 with Training Control bit 3 (Disable Scrambling) set
  is seen, lock to `off`. Otherwise run an `off` and an `on` pipeline side by
  side, each producing its own transactions into its own buffer (ordered
  sets are identical in both). The first pipeline to close a packet whose CRC
  is correct (a DLLP with valid CRC-16 or a TLP with valid LCRC, whichever
  comes first) wins; if both close a valid packet at the same symbol, `off`
  wins. On lock, emit the winner's buffered transactions, then an info
  transaction (label `Scrambling: on (detected)` or `Scrambling: off
  (detected)`, fields `type: "scrambling_detected"`, `mode: "on"|"off"`,
  spanning the winning packet), then continue with the winner only. If no
  pipeline has locked when 4096 transactions are buffered, or at flush, lock to
  `off` with label `Scrambling: off (undetermined)` and `mode: "off"`, spanning
  the last buffered transaction (or the last symbol, if none was buffered).

## 6. Errors common to both decoders

All have `is_error = true`, a `type`, and an `error` field (one sentence).

| Situation | Label | `type` | Other fields |
|---|---|---|---|
| Invalid K symbol | `Invalid K symbol 0x9C` | `invalid_k` | `symbol: "0x9C"` |
| STP or SDP while a packet is open | `Packet truncated by STP` / `by SDP` / `by COM` | `truncated` | `packet: "tlp"|"dllp"`, `symbols` (body length so far) |
| Gating while a packet is open | `Packet truncated by rxvalid` / `by txelecidle` | `truncated` | as above |
| END or EDB with no packet open | `END without start` / `EDB without start` | `unmatched_end` | |
| K symbol other than END/EDB inside a packet (COM, STP, SDP are covered above) | `Invalid K symbol in packet 0x1C` | `k_in_packet` | `symbol` |
| Stray data | `Data outside a packet` | `stray_data` | `symbols` (run length), `first: "0x12"` |
| Unknown ordered set | `Unknown ordered set` | `unknown_os` | `symbol: "0x.."` (the symbol that did not fit) |
| X/Z | `X/Z on data` | `unknown_value` | `edges` |

A run of `stray_data` is one transaction; it ends at the next non-stray symbol
or gated/unknown edge.

## 7. PIPE decoder transactions

| Unit | Label | Fields (in order, after `type`) |
|---|---|---|
| TS1 / TS2 | `TS1 ×4 link=PAD lane=PAD n_fts=24 gen1` (`×N` only when N > 1; `link=0`; `gen2` when Data Rate bit 2 is set) | `count`, `link` ("PAD" or decimal), `lane`, `n_fts`, `rate_id` ("0x02"), `training_control` ("0x00"), `hot_reset`, `disable_link`, `loopback`, `disable_scrambling`, `compliance_receive` (bools) |
| SKP OS | `SKP` (`SKP ×2` when it carries 2 SKP symbols, any count ≠ 3 is shown) | `skp_symbols` |
| FTS | `FTS ×N` | `count` |
| EIOS | `EIOS` | |
| EIEOS | `EIEOS ×N` | `count` |
| Logical idle (`show_idle` only) | `Idle ×N` | `symbols` |
| TLP frame | `TLP frame seq=5 (24 symbols)`; nullified: `TLP frame seq=5 (24 symbols, EDB)` | `seq` (from body bytes 0-1, 12 bits; omitted if body < 2), `symbols` (body length), `end` ("END"/"EDB") |
| DLLP frame | `DLLP frame (6 symbols)` | `symbols`, `end` |
| RxStatus | see §4.6 | |

`type` values: `ts1`, `ts2`, `skp`, `fts`, `eios`, `eieos`, `idle`,
`tlp_frame`, `dllp_frame`.

**Coalescing** (`coalesce=true`): consecutive TS1s with identical symbols 1-15
form one transaction with `count`; likewise TS2, FTS OS and EIEOS. Anything
else between two ordered sets (including a SKP) ends the run. With
`coalesce=false` every ordered set is its own transaction with `count: 1` and
no `×1` in the label.

The PIPE decoder does not check CRCs (the DLL decoder does), except inside
`auto` scrambling detection.

### 4.6 RxStatus (PIPE decoder, when bound)

At each ungated edge where `rxstatus` is non-zero and differs from the
previous edge's value, emit one transaction spanning that edge's symbols:

| Code | Label | `type` | `is_error` |
|---|---|---|---|
| 001 | `RxStatus: SKP added` | `rxstatus` | false |
| 010 | `RxStatus: SKP removed` | `rxstatus` | false |
| 011 | `RxStatus: receiver detected` | `rxstatus` | false |
| 100 | `RxStatus: 8b/10b decode error` | `rxstatus` | true |
| 101 | `RxStatus: elastic buffer overflow` | `rxstatus` | true |
| 110 | `RxStatus: elastic buffer underflow` | `rxstatus` | true |
| 111 | `RxStatus: disparity error` | `rxstatus` | true |

Fields: `code: "0b100"`.

## 8. DLL decoder transactions

The DLL decoder emits nothing for ordered sets or idle (it still uses them for
descrambling and framing).

### 8.1 DLLPs

A DLLP body must be exactly 6 bytes (4 + CRC-16); otherwise error
`DLLP length 5, expected 6` (`type: "dllp_length"`, `symbols`).

| Byte 0 | Label | `type` | Fields after `type` |
|---|---|---|---|
| 0x00 | `Ack seq=12` | `ack` | `seq` |
| 0x10 | `Nak seq=5` | `nak` | `seq` |
| 0x20 | `PM_Enter_L1` | `pm_enter_l1` | |
| 0x21 | `PM_Enter_L23` | `pm_enter_l23` | |
| 0x23 | `PM_Active_State_Request_L1` | `pm_as_request_l1` | |
| 0x24 | `PM_Request_Ack` | `pm_request_ack` | |
| 0x30 | `Vendor 0x123456` | `vendor` | `payload: "0x123456"` (bytes 1-3) |
| 0x40-0x47 | `InitFC1-P VC0 H=32 D=1008` | `initfc1_p` | `vc`, `hdr_fc`, `data_fc` |
| 0x50-0x57 | `InitFC1-NP …` | `initfc1_np` | same |
| 0x60-0x67 | `InitFC1-Cpl …` | `initfc1_cpl` | same |
| 0xC0-0xC7 | `InitFC2-P …` etc. | `initfc2_p` / `_np` / `_cpl` | same |
| 0x80-0x87 | `UpdateFC-P …` etc. | `updatefc_p` / `_np` / `_cpl` | same |
| other | `Reserved DLLP 0x01` | `reserved` | `byte0: "0x01"`; `is_error = true` |

Every DLLP also carries `crc` (the two CRC bytes in wire order, `"0x35BC"`)
as its last field. `seq` = `{byte2[3:0], byte3}`. FC fields per protocol
notes §5.

**CRC mismatch**: the DLLP is still decoded, `is_error = true`, the label
becomes `<normal label> CRC mismatch`, and the fields gain (after `crc`)
`expected_crc` and `error` ("DLLP CRC-16 does not match its contents").

### 8.2 TLPs

Body = 2 sequence bytes + TLP bytes + 4 LCRC bytes. A body shorter than 18
bytes, or with `(body - 6) % 4 != 0`, is error `TLP length 13 symbols is not
valid` (`type: "tlp_length"`, `symbols`).

Header decode (light): byte 0 = Fmt[2:0] (bits 7:5), Type[4:0]; Length = 10
bits from bytes 2-3 (0 means 1024); TD = byte 2 bit 7; EP = byte 2 bit 6;
TC = byte 1 bits 6:4. Names:

| Fmt | Type | Name |
|---|---|---|
| 000 / 001 | 0 0000 | `MRd32` / `MRd64` |
| 000 / 001 | 0 0001 | `MRdLk32` / `MRdLk64` |
| 010 / 011 | 0 0000 | `MWr32` / `MWr64` |
| 000 / 010 | 0 0010 | `IORd` / `IOWr` |
| 000 / 010 | 0 0100 | `CfgRd0` / `CfgWr0` |
| 000 / 010 | 0 0101 | `CfgRd1` / `CfgWr1` |
| 001 / 011 | 1 0rrr | `Msg` / `MsgD` |
| 000 / 010 | 0 1010 | `Cpl` / `CplD` |
| 000 / 010 | 0 1011 | `CplLk` / `CplDLk` |
| other | | `Type 0x<byte0>` |

Label: `TLP seq=5 MWr32 len=1` (len = Length field in DW).

Fields after `type: "tlp"`: `seq`, `tlp` (the name), `length_dw`, `tc`, `td`,
`ep` (bools for td/ep), `bytes` (TLP bytes, excluding seq and LCRC), `lcrc`
(wire order, `"0x9AE8F8C2"`), `replay` (bool).

**Length check**: header DWs = 3 if Fmt bit 0 is 0, else 4. If the format has
data (Fmt bit 1), payload bytes = `bytes - 4*hdr_dw - (td ? 4 : 0)` must equal
`4 * length_dw`; if it has no data, `bytes` must equal `4*hdr_dw + (td ? 4 :
0)`. Otherwise `is_error`, label suffix ` length mismatch`, fields gain
`error`.

**Nullified TLP** (ends in EDB and the LCRC is the bitwise inverse of the
correct one): label `Nullified TLP seq=5 MWr32`, `type: "tlp_nullified"`,
not an error, same fields minus `replay`.

**EDB with any other LCRC**: `is_error`, label `TLP ended by EDB seq=5`,
`type: "tlp_edb"`, fields `seq`, `bytes`, `error`.

**LCRC mismatch (END)**: `is_error`, label `TLP seq=5 MWr32 len=1 LCRC
mismatch`, fields gain `expected_lcrc` and `error`.

**Sequence tracking** (good-LCRC, END-terminated TLPs only): the first such
TLP sets `next = seq + 1 (mod 4096)`. Each later one:

- `seq == next`: normal; `next = seq + 1`.
- `(next - seq) mod 4096` in 1..2048: a replay; label suffix ` (replay)`,
  `replay: true`; `next` unchanged.
- otherwise: `is_error`, label suffix ` sequence error`, fields gain
  `expected_seq` and `error`; `next = seq + 1`.

When several suffixes apply, they appear in this order: ` LCRC mismatch`, `
length mismatch`, ` (replay)`, ` sequence error`.

## 9. Field encoding

`fields_json` is a JSON object, `type` first, then the fields in the order
above. Integers are JSON numbers, booleans JSON booleans, everything shown in
quotes above is a string. Hex strings are uppercase with `0x` and the natural
width (`0x5C`, `0x35BC`, `0x9AE8F8C2`, `0x123456`). The host renders every
value as a string, so the expected files (`*.expected.json`) hold strings
(`"seq":"5"`, `"td":"false"`).

## 10. Readings settled after the first comparison

The independent generator and the decoders disagreed, or had to guess, in the
places below. These rules are now part of the specification and override
anything earlier in this document that reads differently.

### 10.1 The `error` field

Every transaction with `is_error = true` carries an `error` field, and it is
always the **last** field. Its text is fixed per type:

| `type` / case | `error` |
|---|---|
| `invalid_k` | `Not a PCIe control symbol.` |
| `unexpected_k` | `A control symbol appeared outside an ordered set or packet.` |
| `truncated` | `The packet ended without END or EDB.` |
| `unmatched_end` | `END or EDB arrived with no packet open.` |
| `k_in_packet` | `A control symbol other than END or EDB appeared inside a packet.` |
| `stray_data` | `Data outside a packet or ordered set; logical idle is 0x00.` |
| `unknown_os` | `COM was not followed by a recognised ordered set.` |
| `unknown_value` | `The data or datak bus carries X or Z.` |
| `dllp_length` | `A DLLP is 6 symbols between SDP and END.` |
| `dllp_edb` | `A DLLP cannot be ended by EDB.` |
| `reserved` | `This DLLP type is reserved in PCIe Gen1 and Gen2.` |
| DLLP CRC mismatch | `DLLP CRC-16 does not match its contents.` |
| `tlp_length` | `A TLP body is at least 18 symbols, and a multiple of 4 between the sequence number and the LCRC.` |
| `tlp_edb` | `A TLP ended by EDB must carry the inverted LCRC of a nullified TLP.` |
| TLP LCRC mismatch | `TLP LCRC does not match its contents.` |
| TLP length mismatch | `The header Length field does not match the framed payload.` |
| TLP sequence error | `The sequence number is neither the next expected one nor a replay.` |
| `rxstatus` 100 | `8b/10b decode error reported by the PHY.` |
| `rxstatus` 101 | `Elastic buffer overflow reported by the PHY.` |
| `rxstatus` 110 | `Elastic buffer underflow reported by the PHY.` |
| `rxstatus` 111 | `Disparity error reported by the PHY.` |

A TLP with several problems carries one `error` field whose text is the
applicable sentences joined by a single space, in the suffix order of §8.2
(LCRC, length, sequence). The fields it gains come before `error` in this
order: `expected_lcrc`, `expected_seq`.

### 10.2 Framing

- A packet transaction spans its start symbol (STP/SDP) through its end
  symbol (END/EDB) inclusive. A truncated packet spans its start symbol to its
  last body symbol (or the start symbol alone when it has no body).
- **K symbol inside a packet**: the error spans the K symbol, the packet is
  **dropped** (no truncation error for it), and framing restarts at the next
  symbol. Data after it, outside any packet, is `stray_data`.
- **Invalid K inside a packet** (a byte that is not a PCIe K-code, §5.1): the
  `invalid_k` error only; the packet is dropped as above.
- **EDB ending a DLLP**: one error transaction, `type: "dllp_edb"`, label
  `DLLP ended by EDB`, fields `symbols`, `error`; the DLLP is not decoded.
- **PAD, SKP, FTS, IDL or EIE outside an ordered set and outside a packet**:
  `type: "unexpected_k"`, label `Unexpected K symbol 0x1C`, fields `symbol`,
  `error`. A sixth consecutive SKP in a SKP ordered set is such a symbol.

### 10.3 Ordered sets

- **Every symbol of an ordered set bypasses the scrambler** (TS1/TS2 symbols
  1-15 and the EIEOS's final D10.2 alike) and still advances the LFSR, except
  SKP, which never advances it.
- `×N` appears in a label only when N > 1, for every coalesced unit (TS1,
  TS2, FTS, EIEOS, Idle). A lone FTS ordered set is `FTS` with `count: 1`;
  a single idle symbol is `Idle` with `symbols: 1`.
- **Disable Scrambling lock** (§5.5): emits an info transaction, label
  `Scrambling: off (training sets)`, fields `type: "scrambling_detected"`,
  `mode: "off"`, spanning the first TS1/TS2 that carried the bit. It is
  emitted where that TS's transaction is.

### 10.4 Time and gating

- `t_k` and `P_k` count every rising edge, including gated and X/Z edges.
- RxStatus "previous edge" is the previous rising edge, gated or not.

### 10.5 Scrambling detection

- "The last buffered transaction" (§5.5, undetermined) is the last transaction
  the `off` pipeline buffered.
- The tie rule in §5.5 cannot occur for valid Gen1/Gen2 packets (an
  exhaustive search over the scrambler's period found no DLLP, and no TLP of
  18, 22 or 26 body bytes, whose CRC is valid both raw and descrambled). It
  stays as a determinism rule.

### 10.6 End of trace (flush)

At flush, in this order:

1. A complete ordered set still being assembled is emitted: a SKP OS with at
   least one SKP, an FTS OS or EIOS with all three symbols, a TS or EIEOS
   with all 16. An incomplete one is `unknown_os`, spanning its symbols.
2. A coalesced run in progress (TS1, TS2, FTS, EIEOS, Idle) is emitted.
3. An open packet is a `truncated` error, label `Packet truncated by end of
   trace`, spanning start symbol to last body symbol.
4. A stray-data or X/Z run in progress is emitted.
5. In `auto` mode, the undetermined rule of §5.5 applies last.

### 10.7 Remaining readings

- `dllp_edb` is a framing rule shared by both decoders: the PIPE decoder
  reports `DLLP ended by EDB` and no `DLLP frame` for it.
- Sequence tracking covers only good-LCRC, END-terminated TLPs (§8.2), so an
  LCRC mismatch never also carries a sequence error. The reachable pairs are
  LCRC + length, length + sequence and length + replay.
- The Disable Scrambling notice spans the first TS1/TS2 that carried the bit
  (its 16 symbols), not the coalesced run it belongs to.
- **Reserved DLLP with a CRC mismatch**: label `Reserved DLLP 0x01 CRC
  mismatch`; fields `type: "reserved"`, `byte0`, `crc`, `expected_crc`,
  `error`; `error` is the reserved sentence and the CRC sentence joined by one
  space, in that order. In general, any unit with several problems joins its
  sentences in the order its label suffixes appear.
- **Incomplete ordered set at end of trace**: `unknown_os` with `symbol` set
  to its last symbol received, spanning its symbols.

### 10.8 Readings confirmed from the first implementation

Chosen by the decoder implementation where this document was silent, and
adopted as written:

1. **An ordered set in progress when gating or X/Z begins**: a SKP OS with at
   least one SKP is complete and is emitted; otherwise, on gating it is
   `unknown_os` with `symbol` = its last symbol, and on X/Z it is dropped
   silently (as an open packet is, §4.4).
2. **X/Z on sideband signals**: an X/Z `rxvalid` or `txelecidle` does not
   gate; an X/Z `rxstatus` counts as code 000 (no transaction) and becomes
   the previous value.
3. **`on` mode before the first COM**: D symbols are discarded, K symbols are
   still framed, so STP…END there has an empty body.
4. **`auto` buffering of edge-level transactions**: X/Z and RxStatus
   transactions before the lock go into both pipelines' buffers, count
   toward the 4096 ceiling and keep their time order.
5. **The 4096 ceiling** is checked after each symbol, break and edge-level
   transaction; a single symbol that completes two units may take the buffer
   to 4097 before the undetermined notice.
6. **The Disable Scrambling notice** is emitted when the TS that carries the
   bit completes, before the coalesced run it belongs to.
7. **A DLLP ended by EDB** never counts as a valid packet for the lock, and
   `dllp_edb` takes precedence over the length check.
8. **Nullified TLPs** get no length check and no sequence tracking.
9. **An unexpected K between idle symbols** ends the idle run; with
   `show_idle` that shows as two `Idle` transactions.
10. **`symbol` of an `unknown_os`** is the raw wire byte for a misfit inside
    FTS, EIOS or EIEOS assembly (ordered-set symbols bypass the descrambler);
    the symbol that ends a SKP OS is not part of it and is descrambled
    normally.
11. **The tie rule** is implemented (`off` first) although §10.5 shows it is
    unreachable.
12. **The first edge** has P = 0, so its symbols have zero duration (start
    equals end).
