# PCIe protocol notes (Gen1/Gen2, 8b/10b)

The protocol facts both the decoders and the fixture generator rely on, with
known-answer vectors. Every CRC and scrambler vector here was verified by
three independent implementations: a bit-serial reference written from the
specification, a table-free reflected C version, and the parallel XOR
equations in openCologne-PCIE's DLL RTL (hard-coded InitFC CRCs and the live
LCRCs of five TLPs in its testbench). Source paths in parentheses refer to the
openCologne-PCIE and openPCIE repositories and to the pcieVHost model they
vendor; they are citations, not dependencies.

## 1. K-codes (byte = (y << 5) | x for Kx.y; computed and checked against pcieVHost `pci_express.h:36-48`)

| Name | Code | Byte | Use |
|---|---|---|---|
| COM | K28.5 | 0xBC | starts every ordered set; resets the scrambler |
| STP | K27.7 | 0xFB | start of TLP |
| SDP | K28.2 | 0x5C | start of DLLP |
| END | K29.7 | 0xFD | end of a good TLP/DLLP |
| EDB | K30.7 | 0xFE | end of a nullified TLP. A PIPE PHY also substitutes 0xFE for an 8b/10b decode error. |
| PAD | K23.7 | 0xF7 | link/lane number "not assigned" in TS1/TS2; framing pad on wide links |
| SKP | K28.0 | 0x1C | SKP OS; never advances the LFSR |
| FTS | K28.1 | 0x3C | FTS OS |
| IDL | K28.3 | 0x7C | EIOS |
| EIE | K28.7 | 0xFC | EIEOS (5.0 GT/s) |
| reserved | K28.4 0x9C, K28.6 0xDC | | flag as an error |
| TS1 identifier | D10.2 | 0x4A | inverted 0xB5 means RX polarity inversion |
| TS2 identifier | D5.2 | 0x45 | inverted 0xBA |

## 2. Ordered sets (8b/10b, per lane)

| OS | Length | Symbols |
|---|---|---|
| TS1 | 16 | 0 COM; 1 Link# (0-255 or PAD); 2 Lane# (0-31 or PAD); 3 N_FTS (0-255); 4 Data Rate Identifier; 5 Training Control; 6-15 0x4A |
| TS2 | 16 | the same, with 6-15 = 0x45 |
| SKP OS | 4 sent (receiver sees 2-6) | COM + 3 SKP. An elastic buffer adds or removes whole SKPs (RxStatus 001/010). The transmit interval is 1180-1538 symbol times. |
| FTS OS | 4 | COM + 3 FTS (N_FTS of them leaving L0s) |
| EIOS | 4 | COM + 3 IDL. A receiver may accept COM + 2 of 3 IDL. |
| EIEOS | 16 | COM + 14 EIE + D10.2 (Gen2 only, before TS1 sequences at 5 GT/s) |
| Compliance | 4, repeating | K28.5, D21.5, K28.5, D10.2. Not scrambled. |
| Logical Idle | n | D0.0 (0x00) data symbols, scrambled, between packets |

**Data Rate Identifier** (symbol 4): bit 0 reserved; bit 1 = 2.5 GT/s
supported (always 1); bit 2 = 5.0 GT/s supported; bits 5:3 reserved (bit 3 =
8 GT/s in 3.0+); bit 6 = Autonomous Change (US port) / Selectable
De-emphasis; bit 7 = speed_change. So Gen1-only = 0x02 and Gen2-capable =
0x06 (`pci_express.h:60-61`).
**Training Control** (symbol 5): bit 0 Hot Reset, bit 1 Disable Link, bit 2
Loopback, bit 3 Disable Scrambling, bit 4 Compliance Receive (2.0+), bits 7:5
reserved (`pci_express.h:63-67`).
A Gen3-capable device may put equalisation info in TS1/TS2 symbols 6-9 at
Gen1/2 rates. Decode it as "not 4A/45" plus a warning, not as a framing
error.

**Recognising TS1 vs TS2**: after COM, if the next symbol is PAD or data,
it is a TS; symbol 6 then classifies it. The other OSs are recognised by
`COM` followed by a K symbol: SKP, FTS, IDL or EIE.

## 3. Gen1/2 scrambler

- Polynomial G(x) = x^16 + x^5 + x^4 + x^3 + 1, Galois form (taps mask
  0x0039 on a left-shifting 16-bit register). **Seed 0xFFFF**.
- **COM resets the LFSR.** The symbol after COM uses the freshly seeded
  state, so its key is 0xFF.
- The LFSR advances 8 bits for **every symbol except SKP**: data, other K
  symbols, and TS symbols all advance it.
- **K symbols are never scrambled.** **TS1/TS2 data symbols (1-15) are not
  scrambled, but they do advance the LFSR.** The compliance pattern is not
  scrambled. Everything else that is D, including logical idle and packet
  bytes, is XORed with the key.
- Key bit 0 (the first transmitted bit) is the LFSR MSB before each shift.
  Equivalently, in pcieVHost's byte-parallel form, `key = bitrev8(lfsr[15:8])`
  (`pcievhost/src/codec.c:166-181,198`).
- Multi-lane: one LFSR per lane, all identical. Out of scope.
- Disabled when TS Training Control bit 3 is set. Simulation models do not
  always set it when they run unscrambled (pcieVHost never does), which is
  why the decoders can detect the mode from CRC results instead.

**Known answer** (keys for the first 32 symbols after a COM, i.e. what 32
scrambled 0x00 idles look like on the wire). Computed by a bit-serial
reference written from the rules above and cross-checked over 20,000 random
symbols, with COM and SKP mixed in, against a re-expression of pcieVHost's
byte-parallel update (0 mismatches). The fixture generator
(`../tools/`) reproduces these keys as its first self-check:

```
FF 17 C0 14 B2 E7 02 82 72 6E 28 A6 BE 6D BF 8D
BE 40 A7 E6 2C D3 E2 B2 07 02 77 2A CD 34 BE E0
```

## 4. Packet framing

| Packet | Wire symbols | Minimum |
|---|---|---|
| TLP | STP, `{0000b, seq[11:8]}`, `seq[7:0]`, TLP bytes (3 or 4 DW header + payload + optional ECRC DW), LCRC x4, END | 20 symbols (3-DW header, no data) |
| Nullified TLP | same, but the **LCRC is bitwise inverted** and the packet ends with **EDB** | |
| DLLP | SDP, 4 DLLP bytes, CRC x2, END | exactly 8 symbols |

- The sequence number is 12 bits, modulo 4096. The 4 reserved bits must be 0
  and are included in the LCRC.
- On x1 there is no alignment rule. STP or SDP may follow END in the very
  next symbol, and packets may start at any byte of a multi-byte PIPE word.
  The fixture packs DLLPs back to back.
- The TLP byte count between seq and LCRC must be a multiple of 4. The
  header Length field gives the payload DWs. TD (byte 2 bit 7) means a
  4-byte ECRC precedes the LCRC.
- ECRC (out of scope, but note it if the decoder ever checks it): same CRC-32
  over the TLP header and data, with variant bits forced to 1 (Type bit 0 and
  EP); pcieVHost `CalcEcrc` (`pcie_utils.c:1238-1263`).

## 5. DLLP types and fields (Gen1/2; no scaled flow control)

| Byte 0 | Type | Bytes 1-3 |
|---|---|---|
| `0000 0000` | Ack | b1 reserved; b2[3:0] = AckNak_Seq_Num[11:8]; b3 = seq[7:0] |
| `0001 0000` | Nak | same. Nak carries the last good seq (NEXT_RCV_SEQ - 1). |
| `0010 0000` | PM_Enter_L1 | reserved |
| `0010 0001` | PM_Enter_L23 | reserved |
| `0010 0011` | PM_Active_State_Request_L1 | reserved |
| `0010 0100` | PM_Request_Ack | reserved |
| `0011 0000` | Vendor-specific | vendor defined |
| `0100 0vvv` | InitFC1-P | FC layout below |
| `0101 0vvv` | InitFC1-NP | |
| `0110 0vvv` | InitFC1-Cpl | |
| `1100 0vvv` | InitFC2-P | |
| `1101 0vvv` | InitFC2-NP | |
| `1110 0vvv` | InitFC2-Cpl | |
| `1000 0vvv` | UpdateFC-P | |
| `1001 0vvv` | UpdateFC-NP | |
| `1010 0vvv` | UpdateFC-Cpl | |
| anything else | reserved for Gen1/2 (later specs: 0x01 MR_Init, 0x02 Data_Link_Feature, 0x31 NOP, 0x22 which pcieVHost calls PM_REQ_L0S at `pci_express.h:105`) | flag as reserved |

FC layout: b0 = `type[7:4] | 0 | VC[2:0]`; b1 = `rsvd[7:6] | HdrFC[7:2]`;
b2 = `HdrFC[1:0] | rsvd[5:4] | DataFC[11:8]`; b3 = `DataFC[7:0]`. HdrFC is 8
bits and DataFC 12 bits; a value of 0 in InitFC means infinite credits. This
matches `send_DLLP.v:135-139` and `4.dll/README.md:102-121`. InitFC1 and
InitFC2 sets are resent at least every 34 us until their exit condition
(README:80-87).

## 6. DLLP CRC-16 (exact)

- Polynomial **0x100B** (x^16 + x^12 + x^3 + x + 1). **Init 0xFFFF.**
- It covers the 4 DLLP bytes in wire order (type first). Each byte is fed
  **bit 0 first** into an MSB-first register: `fb = C[15] ^ d; C <<= 1; if
  fb: C ^= 0x100B`.
- Wire output: CRC byte 1 (symbol after the 4 bytes) = `bitrev8(~C[15:8])`,
  CRC byte 2 = `bitrev8(~C[7:0])`. Sources: pcieVHost `CalcDllpCrc`
  (`pcie_utils.c:1214-1228`), openCologne `crc_gen.v:44-53`.
- **The same thing in reflected form, which is how a C decoder should write
  it:** poly 0xD008, init 0xFFFF, LSB-first, xorout 0xFFFF; the result's
  **low byte goes first on the wire**.
- Receiver check: run the reflected CRC over all 6 bytes and expect the
  constant residue **0xAA90** (verified for Ack, Nak and InitFC2-Cpl).

| DLLP (wire bytes) | CRC on wire | Source |
|---|---|---|
| 40 08 03 F0 (InitFC1-P VC0 Hdr 32 Data 1008) | **35 BC** | RTL `send_DLLP.v:142` |
| 50 08 00 01 (InitFC1-NP Hdr 32 Data 1) | **B1 F6** | RTL `:143` |
| 60 00 00 00 (InitFC1-Cpl 0/0) | **D8 92** | RTL `:144` |
| C0 08 03 F0 (InitFC2-P) | **4F C3** | RTL `:146` |
| D0 08 00 01 (InitFC2-NP) | **CB 89** | RTL `:147` |
| E0 00 00 00 (InitFC2-Cpl) | **A2 ED** | RTL `:148` |
| 00 00 00 00 (Ack seq 0) | **B3 62** | computed |
| 00 00 0F FF (Ack seq 4095) | **25 A8** | computed |
| 10 00 00 05 (Nak seq 5) | **7D 70** | computed |
| 80 02 80 C8 (UpdateFC-P Hdr 10 Data 200) | **EC F8** | computed; also seen in the captured openCologne fixture |

## 7. TLP LCRC-32 (exact)

- Polynomial **0x04C11DB7**, **init 0xFFFFFFFF**. It covers the **2
  sequence-number bytes** (`{0000, seq[11:8]}`, `seq[7:0]`) and then every
  TLP byte. STP is excluded. Bits are fed bit 0 first. Wire bytes, in order:
  `bitrev8(~C[31:24]), bitrev8(~C[23:16]), bitrev8(~C[15:8]), bitrev8(~C[7:0])`
  (`pcie_utils.c:1273-1290`, `retry_buffer.v:106-126`).
- **This is bit-for-bit IEEE 802.3 CRC-32** (zlib `crc32`: reflected
  0xEDB88320, init and xorout 0xFFFFFFFF), **sent least-significant byte
  first**. Verified against `zlib.crc32` on three inputs, including
  "123456789" giving CBF43926 on the wire as 26 39 F4 CB.
- Good-TLP residue: CRC-32 over seq + TLP + the 4 received LCRC bytes is
  **0x2144DF1C** (that is, ~0xDEBB20E3).
- **Nullified**: the LCRC bytes are the bitwise NOT of the correct ones, and
  the packet ends with EDB.

| Input (seq bytes + TLP) | LCRC on wire |
|---|---|
| ASCII "123456789" (CRC catalogue check) | 26 39 F4 CB |
| `00 00` + MRd32 `00 00 00 01 01 00 00 0F 00 00 10 00` (seq 0, 1 DW, ReqID 0100, tag 0, addr 0x1000) | **9A E8 F8 C2** (nullified: 65 17 07 3D + EDB) |
| the five TLPs in the captured openCologne fixture (`../fixtures/captured/`) | all verify |

**How this was verified.** Three independent implementations agree on every
vector in this section:

1. a bit-serial reference written from the rules above;
2. a table-free reflected C implementation;
3. the parallel XOR equations in openCologne-PCIE's DLL RTL, both its
   hard-coded InitFC CRCs and the live LCRCs of the five TLPs its testbench
   produces.

pcieVHost's CRC code (`pcicrc32.c`) uses the same algorithm as (1), and the
TLP LCRC equals `zlib.crc32` on every input tried, including the catalogue
check string. The fixture generator in `../tools/` recomputes all of them
before it writes a fixture, and the decoders' unit tests assert them.

Pitfalls this rules out: feeding bytes MSB first, forgetting the final
inversion, putting CRC[7:0] first for the DLLP, excluding the seq bytes from
the LCRC, and including STP/SDP in either CRC.

## 8. Lanes

v1 is x1 only. A multi-lane link stripes symbols round-robin across lanes,
and each lane carries its own ordered sets. It needs N separate lane buses
plus lane-deskew, so it is not trivial and is **out of scope**. On a wider
link, the PIPE decoder bound to lane 0 still decodes ordered sets correctly.
Packets will appear broken, so label them "multi-lane? framing unreliable"
once TS1/TS2 show Lane# != 0 or when the link width is greater than 1.

---
