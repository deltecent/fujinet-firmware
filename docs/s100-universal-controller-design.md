# Altair / S100 Universal Disk & Serial Controller — Design

This document designs a FujiNet peripheral for the **S100 (Altair 8800) bus**: a
single board that can *become* any of the classic S100 floppy-disk controllers
and serial cards, serving disk images and network/serial streams from a FujiNet.
It builds directly on the model described in
[`firmware-architecture.md`](firmware-architecture.md) — read that first,
especially **§2.5 (fixed vs SLIP framing)** and **§9 (the `pico/` companion
microcontroller firmware)**. This design is a specialization of the `pico/`
front-end pattern.

> **Scope note.** The board only has to *look like the board it is emulating* —
> the right I/O ports, register semantics, control latch, data-bus polarity,
> DRQ/INTRQ/DMA behavior, and timing. **What operating system the host runs over
> that is out of scope.** If the emulation is cycle- and byte-faithful, CP/M,
> CDOS, North Star DOS, Altair BASIC, or a hand-rolled monitor neither know nor
> care. Nothing in this design is written "for CP/M."

> **Status.** Design + templates only. No code in this document has been built or
> run on hardware. Register/port facts are cited to the manuals under
> `~/src/altairsim/reference/` (see [Appendix C](#appendix-c--reference-sources));
> anything a manual does not state is flagged rather than guessed.

> **Not the existing `s100spi`.** The tree already has an SPI-based `s100spi`
> bus/device/media scaffold. This is a *different* hardware approach (an S100 card
> with a Raspberry Pi Pico front-end talking FujiBus/SLIP to the ESP32) and is
> designed from scratch. It does not reuse or depend on `s100spi`.

---

## 1. Goals and non-goals

**Goals**

- One S100 card that emulates **many** disk controllers and serial cards, chosen
  at runtime — a "universal" controller.
- Faithful register/port/timing emulation of each target board.
- Disk images, filesystems, WiFi, and the config UI live on the **ESP32**
  (a FujiNet); the card is a front-end to it.
- Support **mixed densities and sector sizes** (per-track geometry), and both
  **soft-sectored** (WD-style IBM) and **hard-sectored** (North Star, Altair)
  formats.
- Emulate **various UARTs** for the serial side.
- The ESP32 can **reconfigure the front-end on the fly** — switch the emulated
  board or add a serial card from the web UI without reflashing the Pico.

**Non-goals**

- Guest operating system behavior (out of scope, as above).
- Analog flux-level fidelity (copy-protection, weak bits). We serve *logical*
  sectors/tracks; see [§9](#9-disk-geometry--image-formats).
- Being an S100 CPU/memory board. This is an I/O peripheral only.

---

## 2. Prior art: the FarmTek FDC+

The single most relevant existing device is the **FDC+** (`FDC+ Manual.md`): a
modern, single-board, drop-in replacement for the two-board MITS **88-DCDD** (8″)
and **88-MDS** (Minidisk) controllers that is register- and timing-compatible with
the originals, synthesizes hard-sector pulses from soft-sectored media, and — most
tellingly — has a **high-speed serial port** over which it serves disk images from
a PC running "Altair Server.exe" (drive types 6/7, at 230.4K–460.8K baud).
[FDC+]

That is essentially this project, minus FujiNet: *emulate a period controller on
the bus, fetch the sector data over a serial link from a smarter host.* Two useful
lessons carry over:

1. **Serving sectors over a serial link to a bus-faithful front-end is a proven
   approach.** Our smarter host is the ESP32; our link is FujiBus/SLIP.
2. The FDC+ manual **does not document its serial wire protocol** [FDC+]. We
   don't have to reverse-engineer it — we define our own (FujiBus, already used
   across FujiNet; see firmware-architecture.md §2.5).

The FDC+ only covers the two Altair controllers. Our target set is wider (see
[§5](#5-the-reality-of-universal)).

---

## 3. Two-MCU architecture

```
                     S100 backplane
  ┌───────────────┐  (I/O + DMA cycles) ┌──────────────────────┐  FujiBus/SLIP   ┌────────────────────────┐
  │ S100 host CPU │◄───────────────────►│  RP2350B (Pico)       │◄───(USB-CDC or──►│  ESP32 fujinet-firmware │
  │ (8080/Z80/…)  │  ports, /PRDY, DMA, │  PIO bus emulation +  │    UART)         │  disk images, FS, WiFi, │
  └───────────────┘  VI/PINT interrupts │  FDC/UART cores       │                  │  config UI, N: network  │
                                        └──────────────────────┘                  └────────────────────────┘
        real-time bus fidelity ───────────────┘                 └─── storage / networking / brains ───┘
```

Exactly the split from firmware-architecture.md §9: an S100 I/O or DMA cycle must
be answered in tens of nanoseconds, which only the RP2350's **PIO** state
machines can do. So responsibilities divide cleanly:

| Concern | Where |
|---|---|
| S100 address decode, data drive, /PRDY wait, DMA, VI/PINT interrupts, DRQ/INTRQ, hard-sector pulse synthesis, on-disk framing (CRC/sync/checksum), byte-level timing | **Pico** (PIO + cores) |
| Disk image storage & format decode, filesystems (SD/LittleFS/TNFS/host), WiFi, config, web UI, the personality library, the N: network stack | **ESP32** (existing FujiNet firmware) |

The ESP32 keeps being a FujiNet. The Pico is a new, S100-shaped front-end for it.

**Why the RP2350B specifically, not an RP2040.** The Platform Bring-Up Guide's
interface-choice rule (FEP-004, Ch. 1) is "count your signal lines": a wide
parallel bus needs the high-pin-count part. S100 is exactly that — a universal
controller must see A0–A15 (16 lines, for North Star's memory-mapped registers and
boot-ROM windows), the **separate** DI0–DI7 and DO0–DO7 data buses (16 more), plus
`sINP`/`sOUT`/`pDBIN`/`pWR*`/`/PRDY`/status/VI interrupt/`PHOLD`/`PHLDA` control —
comfortably 40+ GPIO (a concrete starting map is
[§7.1](#71-a-possible-gpio--s100-signal-map)). An RP2040 (~30 usable GPIO, 2 PIO blocks / 8 state machines)
cannot fit that; the **RP2350B** (up to 48 GPIO, 3 PIO blocks / 12 state machines)
is the intended part, and — per the guide — it also interfaces to 5 V bus levels
directly (see [§14](#14-open-questions-and-risks) risk 3 for the backplane-drive
caveat). Using GPIO above 31 imposes PIO constraints called out in
[§7](#7-s100-bus-interface-layer-pio).

---

## 4. The Pico is the FujiBus *client*

Note the request direction. On the Atari SIO or RS232/FujiBus path, the *host
computer* asks and the ESP32 *answers* (the ESP32 is the peripheral). Here the
S100 host asks the **Pico** (it thinks it is talking to a WD1793 or a 6850), and
the Pico — which has no disk images — must in turn ask the **ESP32**. So the Pico
is a FujiBus **client**, the ESP32 a FujiBus **server**. This is exactly the
`pico/intellivision` direction (RP2040 issues requests to the ESP32-S3; see
firmware-architecture.md §9 and `pico/intellivision/firmware/src/fujibus.c`).

### 4.1 Latency and the sector cache

A WD179x that has accepted a Read Sector command must begin producing bytes at the
drive's data rate (≈**23.5 µs/byte** MFM worst-case DRQ service on WD177x [WD177X];
**32 µs/byte** on 8″ FM, 64 µs on 5¼″ [VersaFloppy]). A FujiBus round-trip to the
ESP32 cannot happen *per byte*. Therefore:

- The Pico maintains a **local sector/track cache** (SRAM). On a Read Sector, it
  serves bytes from cache and issues the FujiBus fetch either **ahead of time**
  (read-ahead of the next logical sector) or **during the command's own head-load
  / settle delay window**, which the WD command timing already allows (E flag =
  15–30 ms settle [WD177X]).
- If the data is not yet present when the host demands the first byte, the Pico
  uses the target board's **native stall mechanism** to buy time — see the five
  models in [§5](#5-the-reality-of-universal). Every classic board already has a
  way to make the CPU wait (that is how real drives with rotational latency
  worked); we reuse it. Only if the stall budget is exceeded do we surface a
  Lost Data / timeout status, exactly as a real drive would on a bad transfer.
- Writes buffer into the Pico, then flush to the ESP32 as one
  `DISK_WRITE_SECTOR` on sector/command completion.

This is the single most important timing decision in the design and is called out
again in [§14 risks](#14-open-questions-and-risks).

---

## 5. The reality of "universal"

The user's framing was "mostly emulating disk controllers with the Western Digital
FD17xx." The manuals show that is only part of the picture — the S100 disk world
spans **at least five different flow-control models, four+ controller chip
families, and both I/O-mapped and memory-mapped interfaces.** A universal board
must implement all of them. This table is the design's backbone (every cell cited):

| Board | FDC chip | Interface | Flow-control / transfer model | Density | Sectoring |
|---|---|---|---|---|---|
| **Cromemco 4FDC** | WD **FD1771** | I/O ports 30–34h (fixed) | PIO, poll DRQ (port 30/34 bit7); optional **Auto-Wait** (port 34 D7) [Cromemco] | FM only | soft (IBM 3740) |
| **Cromemco 16FDC/64FDC** | WD **FD1793** | I/O ports 30–34h | PIO + Auto-Wait; 64FDC needs ≥4 MHz, no DMA [Cromemco] | FM/MFM | soft |
| **Tarbell** | WD **FD1771** | I/O ports F8–FFh | **Hardware WAIT port** — `IN FC` stalls CPU on DRQ/INTRQ via /PRDY or /XRDY [Tarbell] | FM (DD on #2022) | soft |
| **SD Systems VersaFloppy I/II** | FD1771 / **FD1791** | I/O ports 60–67h | **CPU wait-state (/PRDY)** on data port 67h only [VersaFloppy] | I=FM, II=FM/MFM | soft |
| **CompuPro Disk 1 / 1A** | NEC **µPD765A / i8272** | I/O ports C0–C3h | **DMA only** — DMA address stack, no PIO data port [CompuPro] | FM/MFM | soft |
| **North Star MDS-A / A-D** | **discrete/custom** | **memory-mapped** 1K block @ E800h | **CPU wait-state (/PRDY)** [NorthStar] | SD/DD, own format | hard (10 sec) |
| **Altair 88-DCDD** | **discrete** (not WD) | I/O ports 08–0Ah (octal 010–012) | PIO paced by **ENWD/NRDA** status flags (1 byte/32 µs) [88-DCDD] | — | **hard (32 sec)** |
| **Altair 88-MDS** (Minidisk) | **discrete** | I/O ports 08–0Ah | PIO paced by ENWD/NRDA (1 byte/64 µs) [88-MDS] | — | **hard (16 sec)** |
| **FDC+** | emulates 88-DCDD/88-MDS | I/O ports 08–0Bh (or 80–83h) | ENWD/NRDA, synthesizes hard-sector pulses [FDC+] | — | hard 32/16 |

Design consequences:

1. **The "generic WD179x core" is necessary but not sufficient.** It covers
   Cromemco/Tarbell/VersaFloppy (with per-board wrappers). CompuPro needs a
   **µPD765/8272** core; North Star and the Altair boards need **discrete-logic
   personalities** with their own command/status models. All present the same
   *internal* interface to the rest of the firmware (mount, read sector, write
   sector, geometry) — see [§8](#8-fdc-emulation).
2. **The bus interface layer must implement five stall/transfer mechanisms**, one
   per model above. This is a *set of PIO programs* the personality selects
   ([§7](#7-s100-bus-interface-layer-pio)).
3. **Interface addressing itself varies**: North Star is memory-mapped. The bus
   layer therefore matches on either an **I/O cycle** (sINP/sOUT) *or* a
   **memory cycle** in a decoded window, per personality.

The serial side is analogously varied (see [§10](#10-uart-emulation)): MC6850
(Altair 2SIO), Intel 8251 (IMSAI SIO-2), Signetics 2651, and discrete
COM2502/1602-family UARTs (MITS 88-SIO, SSM IO-4) — with the notorious trap that
on the discrete boards **the status-bit positions are defined by the board's
wiring, not by the chip** [com2502][88-SIO][SSM IO-4].

---

## 6. "Universal" = data-driven personalities, configured by the ESP32 on the fly

The board becomes a specific controller by loading a **personality**: a *data
descriptor*, not compiled-in code. Two layers:

- **Generic device cores** (Pico) — `fdc_wd179x`, `fdc_upd765`, `fdc_discrete`,
  and `uart` — each implements a *chip/behavior family* once.
- **Personality descriptor** — the S100 port/memory map, register offsets,
  control-latch bit assignments, data-bus polarity, flow-control model,
  interrupt wiring, density/side-select capability, and the PIO program +
  parameters needed to decode *that* board's cycles.

**The ESP32 owns the personality library and pushes descriptors to the Pico at
runtime.** The Pico ships generic and boots **blank** — it decodes no cycles until
the ESP32 sends a `CFG_LOAD_PERSONALITY` frame. On `CFG_APPLY`, the Pico tears
down and **re-initializes its PIO state machines and register maps live**
(`s100_apply_personality()`), becoming that board. Rationale:

- The ESP32 has the config UI, the filesystem, and room for the whole personality
  library; the Pico firmware stays small and generic.
- A user can switch the emulated controller (or hot-add a serial card) from the
  web UI without reflashing the Pico.
- Where two boards need genuinely different PIO *logic* (hard-sector vs
  soft-sector, memory-mapped vs I/O), the Pico carries a small **set** of PIO
  programs and the descriptor selects which to load and with what parameters.

Boot handshake:

```
Pico  --HELLO(fw_ver, caps)------------------------►  ESP32
Pico  ◄--LOAD_PERSONALITY(descriptor: FDC)---------   ESP32   (from config / web UI)
Pico  ◄--LOAD_PERSONALITY(descriptor: UART ch A)---   ESP32
Pico  ◄--APPLY-------------------------------------   ESP32
Pico  --ACK (now decoding S100 cycles as that board)►ESP32
```

The descriptor wire format is in [§12](#12-personality-descriptor-wire-format).

---

## 7. S100 bus interface layer (PIO)

This is the timing-critical heart and the most board-specific part. It lives in
Pico PIO + tight core-1 code.

> **RP2350 PIO GPIO-bank constraints (design-shaping, easy to miss).** Because the
> S100 signal set spans more than 32 GPIO, three RP2350 rules from the Bring-Up
> Guide (Ch. 9) drive the pin assignment and cannot be treated as afterthoughts:
> (1) any routing that uses GPIO above 31 requires the firmware to be built with
> `PICO_PIO_USE_GPIO_BASE=1` so the PIO can reach the upper bank; (2) a single
> state machine's contiguous pin span **cannot straddle the GPIO 16↔32 boundary**,
> so related signal groups (e.g. A0–A15, DI0–DI7, DO0–DO7) must each be placed
> entirely within one bank; and (3) a PIO `in pins, 32` / autopush captures only
> the **low 32 GPIO** in one instruction — so, unlike the guide's MSX/CoCo
> examples (whose ≤32-signal buses fit one `BusSignals`-style word), the S100
> decode must sample address, DI and DO across **multiple** PIO reads or state
> machines rather than one packed word. Plan the GPIO map around these before
> laying out the adapter. See also [§14](#14-open-questions-and-risks) risk 8.

### 7.1 A possible GPIO → S100 signal map

The Platform Bring-Up Guide teaches the pin map as a **routing decision, not a
fixed pinout**: its *default GPIO routing* (FEP-004 Ch. 4, Table 7) assigns each
RP2350 GPIO to an ISA-role signal, and every worked example then keeps what fits
and repurposes the rest. **MSX fit almost untouched** (Ch. 5, Table 9): 16 address
lines landed on `GP0–GP15`, the single 8-bit data bus on `GP20–GP27`, and the four
default "strobe" pins `GP28–GP31` were merely *reinterpreted* in the PIO as the
Z80's `/RD` / `/WR` / `/IORQ` / `/MERQ` — "nothing cut or patched." S100 does
**not** fit that cleanly, for a specific and instructive reason: **the classic
S100 bus carries two separate 8-bit data buses — DI0–DI7 (to the CPU) and DO0–DO7
(from the CPU) — not one.** That alone breaks the default single-`D0–D7`-on-
`GP20–GP27` assignment, and together with the wider control set it pushes the map
past 32 GPIO into the RP2350's upper bank — which is exactly why the PIO-bank rules
in the callout above bind here and did not for MSX.

Applying the guide's method to the S100 signal set (§3, §7, §8.4) yields the
**starting** assignment below — a routing proposal to settle before the adapter is
laid out, not a committed pinout. It deliberately packs everything the *decode*
state machine must sample into the **low 32 GPIO** (so one `in pins, 32` autopush
grabs address + cycle-decode + write-data in a single word, like the guide's
`send_bus` SM, Ch. 8–9) and puts the **driven** lines and the DMA/interrupt
handshake in the upper bank:

The **S100 pin** column is the physical bus-connector pin from the MITS 880-110
bus definition [Altair8800]. Note that the 100-line bus **does not order the
address and data lines sequentially** on the connector — A0–A15 and the two data
buses are scattered across both 50-pin rows (top = 1–50, bottom = 51–100) — so the
per-bit pins are listed in bit order, not as a range:

| S100 signal | S100 pin(s) | RP2350 GPIO | Bank | Dir | Role / note |
|---|---|---|---|---|---|
| A0–A15 | A0–A7: 79 80 81 31 30 29 82 83 · A8–A15: 84 34 37 87 33 85 86 32 | `GP0–GP15` | low | in | Address. I/O decode uses A0–A7; **memory** decode (North Star E800h, boot-ROM windows [§8.4](#84-boot-rom--prom-provisioning)) uses all 16. Same placement as the guide's default/MSX. |
| pSYNC | 76 | `GP16` | low | in | Cycle start — the status byte is valid at pSYNC (SYNC·Φ1). |
| pDBIN | 78 | `GP17` | low | in | Read data strobe (drive DI while asserted). |
| pWR* | 77 | `GP18` | low | in | Write data strobe (latch DO). Active low. |
| sINP | 46 | `GP19` | low | in | Status: I/O input cycle. |
| sOUT | 45 | `GP20` | low | in | Status: I/O output cycle. |
| sMEMR | 47 | `GP21` | low | in | Status: memory read (North Star / boot ROM). |
| *(spare)* | sWO* 97 / sM1 44 | `GP22–GP23` | low | — | In-bank spare — e.g. sWO* (write-cycle) / sM1 (opcode fetch) if a personality needs more status for bus-takeover ([§8.4](#84-boot-rom--prom-provisioning)). |
| DO0–DO7 | 36 35 88 89 38 39 40 90 | `GP24–GP31` | low | in | **Host write data** (data-out bus, from the CPU). Placed low so it is captured in the *same* autopush word as the decode above. |
| DI0–DI7 | 95 94 41 42 91 92 93 43 | `GP32–GP39` | upper | out | **Board read data** (data-in bus, to the CPU). Driven by a separate SM wholly inside one bank; does not straddle 16↔32. Honor `bus_inverted` ([§8.1](#81-the-generic-wd179x--fd1771-core)). |
| /PRDY | 72 | `GP40` | upper | out | Wait line for the PRDY / WAIT-port stall models ([§7](#7-s100-bus-interface-layer-pio) table). (XRDY pin 3 is the alternate; pick per backplane.) |
| PINT (/INT) | 73 | `GP41` | upper | out | Interrupt request (active low). Expand to VI0–VI7 vectored lines (pins 4–11) on the spares if a personality needs them. |
| PHOLD | 74 | `GP42` | upper | out | DMA bus-request (active low) — become S100 temporary bus master for the CompuPro DMA model ([§5](#5-the-reality-of-universal), [§7](#7-s100-bus-interface-layer-pio)). |
| PHLDA | 26 | `GP43` | upper | in | DMA bus-grant (host acknowledges the hold; buses go high-Z). |
| STA DSB | 18 | `GP44` | upper | out | Status-disable (active low) for the ROM/RAM-shadow **bus takeover** (Tarbell, [§8.4](#84-boot-rom--prom-provisioning)). |
| PRESET* / POC* | 75 / 99 | `GP45` | upper | in | Bus reset (both active low) — reset the emulated chip / re-arm the boot PROM ([§7](#7-s100-bus-interface-layer-pio)). |
| Φ2 | 24 | `GP46` | upper | in | Bus clock — timing reference for the strobe windows. (Φ1 pin 25 and CLOCK pin 49 are the other clock lines.) |
| *(spare)* | — | `GP47` | upper | — | Upper-bank spare (extra VI line, or the XRDY pin-3 wait line). |

That is **44 assigned signals of the RP2350B's 48 GPIO** — S100 genuinely needs
the high-pin-count part ([§3](#3-two-mcu-architecture)), and the map has almost no
slack. Call this **Option A (direct wiring)**: all 16 data lines are their own
GPIO. A **secondary option that trades a transceiver for seven freed GPIO** is
[below](#option-b--one-bidirectional-data-bus-via-a-transceiver). Two consequences
follow directly from the guide:

- **Transport should be USB-CDC, not UART, for the pin budget.** USB-CDC uses the
  RP2350's dedicated USB pins, leaving all 48 GPIO for the bus; a UART backend
  would consume two GPIO and force dropping the two low-bank spares (or a status
  line). This reinforces the roadmap's step-0 preference ([§15](#15-implementation-roadmap))
  and [risk 7](#14-open-questions-and-risks).
- **The three PIO-bank rules are load-bearing here** (callout above; guide Ch. 9
  `setup_state_machine()`): the build must define `PICO_PIO_USE_GPIO_BASE=1` (DI,
  /PRDY, interrupt, DMA and STA DSB all live above `GP31`); no state machine's
  contiguous pin span may cross the 16↔32 boundary (address + decode + DO fit
  entirely in the low bank; DI0–DI7 sit entirely in the upper bank at base 32);
  and the single-instruction autopush that captures the decode word reaches only
  the low 32 GPIO — which is *why* DO (sampled) is placed low and DI (driven) is
  placed high.

The decode word is the S100 analog of the guide's `BusSignals` union (Ch. 8.3,
9.1) — the union's bitfields **are** the GPIO map:

```c
// S100 decode word: one `in pins, 32` autopush from the LOW bank.
// (DI0..DI7 on GP32..GP39 are DRIVEN by a separate SM, not part of this word.)
typedef union {
  struct {
    uint32_t addr  : 16;   // A0..A15   GP0..GP15
    uint32_t psync :  1;   // pSYNC     GP16
    uint32_t dbin  :  1;   // pDBIN     GP17
    uint32_t pwr   :  1;   // pWR*      GP18  (active low)
    uint32_t sinp  :  1;   // sINP      GP19
    uint32_t sout  :  1;   // sOUT      GP20
    uint32_t smemr :  1;   // sMEMR     GP21
    uint32_t resv  :  2;   //           GP22..GP23 (spare)
    uint32_t dout  :  8;   // DO0..DO7  GP24..GP31 (host write data)
  } __attribute__((packed));
  uint32_t combined;
} S100BusSignals;
```

Signal **polarity** is handled in the PIO exactly as the guide handles the MSX
`/SLTSL` line (configured inverted, so `wait 1` means the line is low, Ch. 9): the
S100 status lines are active-high and `pWR*` active-low, so each `wait` / `in` is
set to the line's real sense (or inverted) in the state-machine config — a
per-signal `.define`, not a wiring change. Final GPIO assignments are validated on
the bench (roadmap step 0, [§15](#15-implementation-roadmap)) before the adapter
PCB is committed.

#### Option B — one bidirectional data bus via a transceiver

Option A spends **16 GPIO** on data because the S100 bus keeps data-in and
data-out physically separate. But the two buses are never active at once — a
machine cycle is either a host *read* (the board drives DI) or a host *write* (the
board samples DO), told apart by the latched status byte at pSYNC. So the board can
present a **single 8-bit bidirectional data port** to the RP2350 and let a small
amount of external glue steer it — collapsing 16 data GPIO to **8 data + 1
direction = 9**, and freeing seven pins.

**How.** Two 74LVC245-class transceivers (the same bus buffers an S100 board wants
anyway for drive and loading on a long backplane — [§14 risk 3](#14-open-questions-and-risks)):
one gates DO0–DO7 (host write data) onto the shared MCU `D0–D7`; the other gates
the shared `D0–D7` out to DI0–DI7 (board read data). **One GPIO — `DBUF_DIR` —
selects the direction:** on a host input cycle (sINP/sMEMR + pDBIN) it enables the
`MCU→DI` transceiver and tri-states `DO→MCU`; on a host output cycle (sOUT + pWR*)
it does the reverse. The two output-enables are complementary, so one direction
bit drives both (one via an inverter), and the RP2350's own eight data-pin
directions flip in lockstep (PIO `out pindirs` / a companion SM). The idle default
is *sample / drive-disabled* so the board never fights the bus.

This is a **guide-documented pattern used deliberately.** The Bring-Up Guide's
ESP32 H89 example drives a `74LVC245` via `OE`/`DIR` pins (Ch. 1), and the CoCo
PIO carries side-set `pindirs` machinery to flip a 245's direction (Ch. 9.3). The
guide's *RP2350* worked examples drive the bus directly because their data buses
are single or narrow; S100's split DI/DO is precisely the case where routing both
through a 245 pays off.

| S100 signal | S100 pin(s) | RP2350 GPIO | Bank | Dir | Role / note |
|---|---|---|---|---|---|
| A0–A15 | A0–A7: 79 80 81 31 30 29 82 83 · A8–A15: 84 34 37 87 33 85 86 32 | `GP0–GP15` | low | in | Address (unchanged from Option A). |
| pSYNC | 76 | `GP16` | low | in | Cycle start (status latched at SYNC·Φ1). |
| pDBIN | 78 | `GP17` | low | in | Read strobe. |
| pWR* | 77 | `GP18` | low | in | Write strobe (active low). |
| sINP | 46 | `GP19` | low | in | Status: I/O input. |
| sOUT | 45 | `GP20` | low | in | Status: I/O output. |
| sMEMR | 47 | `GP21` | low | in | Status: memory read. |
| sWO* | 97 | `GP22` | low | in | Status: write/output cycle — now afforded (was a spare in A). |
| **DBUF_DIR** | *(none — xcvr control)* | `GP23` | low | out | **The 1 direction GPIO.** Drives the data transceivers' DIR/OE from the decoded cycle type; set at pSYNC, stable through the pDBIN/pWR window. |
| D0–D7 | write: DO 36 35 88 89 38 39 40 90 · read: DI 95 94 41 42 91 92 93 43 | `GP24–GP31` | low | in/out | **Single bidirectional data port.** Samples DO on host writes (in the decode word), drives DI on host reads, through the transceivers. Honor `bus_inverted` ([§8.1](#81-the-generic-wd179x--fd1771-core)). |
| /PRDY | 72 | `GP32` | upper | out | Wait line (PRDY/WAIT-port models). |
| XRDY | 3 | `GP33` | upper | out | Independent external-ready wait line — now afforded. |
| PINT (/INT) | 73 | `GP34` | upper | out | Interrupt request (active low). |
| PHOLD | 74 | `GP35` | upper | out | DMA bus-request (active low). |
| PHLDA | 26 | `GP36` | upper | in | DMA bus-grant. |
| STA DSB | 18 | `GP37` | upper | out | Status-disable for bus takeover ([§8.4](#84-boot-rom--prom-provisioning)). |
| PRESET* | 75 | `GP38` | upper | in | Bus reset (active low) — now separate from POC. |
| POC* | 99 | `GP39` | upper | in | Power-on clear (active low). |
| Φ2 | 24 | `GP40` | upper | in | Bus clock / timing reference. |
| VI0–VI7 *(or spares)* | 4 5 6 7 8 9 10 11 | `GP41–GP47` + 1 | upper | out | **Seven freed pins** — enough for the full vectored-interrupt set (VI7 needs the eighth; share a pin or drop one line), **or** a UART transport backend, **or** plain spares. |

Now **≈41 GPIO carry the whole bus with room to spare** (vs 44/48 and no slack in
Option A). The freed budget is the real prize: it lets the **UART transport**
backend coexist without sacrificing a bus signal (relaxing Option A's USB-CDC-only
constraint and [risk 7](#14-open-questions-and-risks)), or brings out all eight
`VI0–VI7` lines. And because all data still lands in the **low bank**, the decode
word is unchanged — the eight data bits are simply bidirectional, and `DBUF_DIR`
takes the low-bank bit a spare held in Option A:

```c
// Option B decode word: still one `in pins, 32` autopush from the LOW bank.
typedef union {
  struct {
    uint32_t addr     : 16;  // A0..A15    GP0..GP15
    uint32_t psync    :  1;  // pSYNC      GP16
    uint32_t dbin     :  1;  // pDBIN      GP17
    uint32_t pwr      :  1;  // pWR*       GP18  (active low)
    uint32_t sinp     :  1;  // sINP       GP19
    uint32_t sout     :  1;  // sOUT       GP20
    uint32_t smemr    :  1;  // sMEMR      GP21
    uint32_t swo      :  1;  // sWO*       GP22
    uint32_t dbuf_dir :  1;  // xcvr DIR   GP23 (output; readback for sanity)
    uint32_t data     :  8;  // D0..D7     GP24..GP31 (DO on write / DI on read via xcvr)
  } __attribute__((packed));
  uint32_t combined;
} S100BusSignals_B;
```

**The trade.** Option B adds two transceivers and one hard-real-time output
(`DBUF_DIR`) whose direction-change timing carries the classic 245 contention
hazard — mitigated because the cycle type is known at pSYNC, ahead of the data
window, and because the idle default is safe. Option A needs no direction logic but
runs the RP2350B's pin count to its limit. Which to build is a **bench decision**
(roadmap step 0, [§15](#15-implementation-roadmap)): prototype Option A's direct
wiring first if the pin budget holds for the chosen transport; adopt Option B when
you want the UART backend, the full VI set, or simply margin — and note an S100
board is already carrying bus transceivers, so Option B's marginal hardware cost is
small.

### 7.2 What the PIO layer must do

It must:

**Decode a cycle.** On the Altair/S100 bus, an I/O cycle is signaled by the status
byte (`sINP`/`sOUT`) with the port address on A0–A7; data travels on the bus's
**separate 8-bit data-in (DI0–DI7, to the CPU) and data-out (DO0–DO7, from the
CPU)** buses, strobed by `pDBIN` (read) / `pWR*` (write). A **memory** cycle
(status `sMEMR`) is decoded on the 16-bit address A0–A15 — needed both for North
Star's memory-mapped registers [NorthStar] **and for boot ROM/PROM windows**
([§8.4](#84-boot-rom--prom-provisioning)). The PIO matches the personality's
address/mask and signals core-1 with the cycle type, address offset, and (for
writes) data.

**Drive data on reads.** For an input cycle the board must place a byte on the
data-in lines within the bus's timing and hold it through `pDBIN`. This is the
per-byte-critical path; it reads from the emulated register file / sector cache.

**Implement the personality's stall/transfer model.** One PIO program (+ params)
per model from [§5](#5-the-reality-of-universal):

| Model | PIO behavior |
|---|---|
| **PIO-poll-DRQ** (Cromemco) | No bus stall. DRQ is a *readable status bit*; core keeps the register file's status byte current. Optional Auto-Wait: when armed (control-latch D7) a read of the flags port asserts /PRDY-wait until DRQ/EOJ/timeout [Cromemco]. |
| **WAIT-port** (Tarbell) | A read of the designated port (FCh) asserts the S100 wait line (/PRDY or /XRDY) until DRQ/INTRQ, then releases and returns the flags byte [Tarbell]. |
| **PRDY-wait** (North Star, VersaFloppy) | A read/write of the *data* port asserts /PRDY until the byte is ready/consumed [NorthStar][VersaFloppy]. Data ports never fail; other ports never stall. |
| **DMA** (CompuPro) | On command completion the FDC "DRQ" drives a DMA burst: the Pico must become an S100 **temporary bus master** via **PHOLD/PHLDA**, drive the address bus from the board's DMA-address register, and read/write host memory [CompuPro]. This is the heaviest PIO/core path. |
| **ENWD/NRDA** (Altair 88-DCDD/MDS, FDC+) | No bus stall; flow is paced by status bits: NRDA (byte available) recurs every 32/64 µs on read, ENWD (ready for next) on write [88-DCDD][88-MDS]. The Pico synthesizes **sector-position** (hard-sector) counting and the per-sector timing, and presents the status port accordingly. |

**Synthesize interrupts.** Assert a chosen S100 VI0–VI7 line or PINT per the
personality (e.g. INTRQ, EOJ, or per-sector interrupt), with the documented
timing. Note single-level interrupt vectoring differs per board (e.g. 88-SIO →
location 070 octal [88-SIO]).

**Serve boot ROM / PROM in the host memory space.** For the enabled personality,
respond to **memory read** cycles in the ROM window with bytes fetched from the
ESP32 — the same PIO+DMA ROM-emulation technique `pico/coco` uses for a cartridge
ROM (firmware-architecture.md §9). Where the board **shadows existing RAM** while
its PROM is enabled (the Tarbell shadows 0000), the board **takes over the bus**:
it asserts **S-100 pin 18 (STA DSB, status disable)** to tri-state the CPU's
status-line buffers and drive the status lines itself, so it reads from the PROM
while RAM writes still land. Honor the personality's ROM-disable mechanism (Cromemco
`OUT 40h` [Cromemco], CompuPro boot-disable bit [CompuPro], Tarbell auto-switch-out
after the boot read [Tarbell]). See [§8.4](#84-boot-rom--prom-provisioning).

**Handle POC*/RESET*.** On bus reset, reset the emulated chip state (e.g. FD1771
loads 03h into the command register and runs a Restore on MR release [FD1771]), and
re-arm the boot PROM if the personality gates it on RESET (Tarbell [Tarbell]).

> `s100_bus.pio` in [Appendix A](#appendix-a--pico-side-templates) is a template
> for the I/O-cycle-decode + drive-data path, with `// TODO` markers for the parts
> that are personality- and backplane-timing-specific. The five transfer models
> above are separate PIO programs selected at `s100_apply_personality()` time.

---

## 8. FDC emulation

### 8.1 The generic WD179x / FD1771 core

All WD-based boards (Cromemco, Tarbell, VersaFloppy) share the same **five-register
model** selected by A1/A0 with the read/write strobe [FD1771][WD177X]:

| A1 A0 | Read | Write |
|---|---|---|
| 0 0 | **Status** | **Command** |
| 0 1 | Track | Track |
| 1 0 | Sector | Sector |
| 1 1 | Data | Data |

Command set (bit fields in true form) [FD1771][WD177X]:

- **Type I** — Restore `0000 hVr₁r₀`, Seek `0001 …`, Step `001u …`, Step-In
  `010u …`, Step-Out `011u …`. `h`=head load, `V`=verify, `r₁r₀`=step rate,
  `u`=update Track register.
- **Type II** — Read Sector `100m …`, Write Sector `101m …`. `m`=multiple record;
  Write DAM bits pick the address mark (FB/FA/F9/F8) on FD1771 [FD1771].
- **Type III** — Read Address `1100 …`, Read Track `1110 …`, Write Track (format)
  `1111 …`.
- **Type IV** — Force Interrupt `1101 I₃I₂I₁I₀`.

Status-register bits are **command-type dependent** — e.g. after Type I,
bit7=Not Ready, bit6=Write Protect, bit5=Head Loaded, bit4=Seek Error,
bit3=CRC, bit2=Track 0, bit1=Index, bit0=Busy; after Read Sector, bit4=Record
Not Found, bit2=Lost Data, bit1=DRQ [FD1771][WD177X]. The core keeps a
per-command status view. `INTRQ` sets on completion and clears on status-read or
command-write; `DRQ` clears on data-register access [FD1771][WD177X].

**Variant flags** the personality must set:

- **Density.** FD1771 is **FM only** — no DDEN, no MFM, no double-density address
  marks [FD1771]. FD1791/1793 and WD1770/1772/1773 add MFM via DDEN [WD177X].
- **Side select.** Not a command capability on FD1771; on WD177x it exists only on
  the **WD1773** (and the 1793 class): Type II bit3 = side-compare, bit1 = enable
  [WD177X]. On WD1770/1772 those bits are motor/precomp instead.
- **Data-bus polarity.** The FD1771/179x DAL is **inverted**; boards wrap it with
  inverting buffers so the guest sees true sense (explicit for Tarbell [Tarbell]
  and VersaFloppy [VersaFloppy]; the raw datasheets do not tabulate it
  [FD1771][WD177X]). Personality flag `bus_inverted`.
- **Post-write register access delay** (WD177x: same register unreadable for
  16 µs MFM / 32 µs FM after a write [WD177X]) — the core models this so tight
  guest code that relies on it still works.

### 8.2 Per-board wrappers (WD family)

The core is wrapped by a personality that maps ports and the control latch:

- **Cromemco** — WD registers at **30–33h**, control/flags at **34h**. Control
  OUT 34h: D7 Auto-Wait, D6 double-density (16/64FDC), D5 motor, D4 8″/5¼″,
  D3–D0 drive-select one-hot. Flags IN 34h: D7 DRQ, D0 EOJ, plus motor/timeout
  bits [Cromemco]. Side-select and eject live on port **04h** (PerSci controls),
  which differ across 4FDC/16FDC/64FDC [Cromemco].
- **Tarbell** — WD registers at **F8–FBh**; **FCh** is the WAIT/extended-command
  port; drive select is loaded **complemented** via a latch (real CBIOS:
  drive 0 = 0xF2 … drive 3 = 0xC2) [Tarbell]. Note the manual's own SELECT
  example contradicts the hardware — trust the bootstrap [Tarbell].
- **VersaFloppy** — WD registers at **64–67h**; **63h** is the board control/status
  latch (drive select, side, density on VF-II, wait-enable) [VersaFloppy]. Data
  port 67h is the only one that wait-states [VersaFloppy].

### 8.3 Non-WD personalities

- **CompuPro (µPD765/8272 core)** — a *different chip model*: Main Status Register
  + Data Register command/result phases, and **DMA** for the execution phase, with
  a DMA-address stack written MSB-first to port 2 [CompuPro]. This is a
  separate core (`fdc_upd765`) sharing the same internal disk interface.
- **North Star (discrete, memory-mapped)** — no chip: commands are *memory reads*
  whose address bits encode the command (CASE field), with two/three status bytes
  and its **own non-IBM format** whose check byte is a **rotate-left-XOR, not a
  CRC** [NorthStar]. Hard-sectored, 10 sectors/track.
- **Altair 88-DCDD / 88-MDS (discrete, hard-sectored)** — three ports (octal
  010–012): drive-select/status, control/sector-position, and data. Flow paced by
  **ENWD/NRDA**; the Pico synthesizes the 32-hole (8″) / 16-hole (Minidisk)
  sector channel, and the first data byte carries a **sync bit** in D7
  [88-DCDD][88-MDS]. The FDC+ is the same interface with soft-sector media and a
  serial back-end [FDC+].

All of these implement one internal interface (`fdc_backend`): `apply_personality`,
`bus_read(offset)`, `bus_write(offset, val)`, and callbacks into the disk cache /
FujiBus for sector data. The rest of the firmware does not care which core is live.

### 8.4 Boot ROM / PROM provisioning

A disk controller is useless if the machine can't boot from it, and on S100 that
means the controller must present a **boot PROM/ROM in the host's memory address
space** — the CPU jumps into it after reset and it loads sector 0/the OS. This is a
first-class capability of the universal board, not an afterthought. The ROM windows
differ per board [cited]:

| Board | ROM window | Enable / disable |
|---|---|---|
| **Tarbell** | 32-byte PROM **shadowing 0000h** | gated onto the bus by **RESET** (DIP bootstrap-enable); **auto-switches out** after the boot read; asserts **pin 18 (STA DSB, status disable)** to tri-state the CPU's status buffers and drive the status lines itself, so it reads PROM while writing RAM at 0000 [Tarbell] |
| **Cromemco** | RDOS: 4FDC 1K @C000; 16FDC 4K @C000–CFFF; 64FDC 8K @C000–DFFF | `OUT 40h` disables ROM (16/64FDC) [Cromemco] |
| **CompuPro** | boot EPROM | boot-disable bit (Disk 1 port 3 D0 / Disk 1A motor-ctrl D0) [CompuPro] |
| **North Star** | PROM inside the E800h memory-mapped block (CASE 0/1) | part of the memory window [NorthStar] |
| **Altair 88-DCDD** | (manual is preliminary; **no boot-loader listing** — front-panel or separate PROM boots it) [88-DCDD] | — |
| **VersaFloppy** | **none on-board** — DDBIOS lives on a separate PROM/memory board @F000h [VersaFloppy] | (out of scope: separate board) |

**How the board serves it.** The Pico emulates ROM as a memory device: it responds
to **memory read cycles** in the personality's ROM window with bytes from a local
copy of the ROM image, using the PIO+DMA ROM-emulation approach `pico/coco` already
demonstrates (firmware-architecture.md §9). The closest live prior art for the
*runtime-swappable* case is the `fujiversal` RP2350 firmware described in the
Platform Bring-Up Guide (Ch. 8.4–8.5): it holds the ROM image in RP2350 RAM and
serves bytes straight from a `rom_ptr` in its per-cycle PIO decode, and swaps the
active image when the host writes an `IO_CONTROL` register (`rom_activate()`) — the
activation trigger is a register write, not a packet. Our board does the same but
picks the image per loaded personality. The ROM image itself is **fetched from the
ESP32** at `APPLY` time (the ESP32's personality library holds the boot ROM blobs),
by the same push mechanism `pico/intellivision` uses to receive a ROM over FujiBus
— see the `ROM_FETCH` command in [§11](#11-fujibus-protocol-for-this-board) and the
ROM fields in the [§12](#12-personality-descriptor-wire-format) descriptor.

**Shadowing RAM (the Tarbell case).** When the ROM window overlaps RAM the guest
also uses (Tarbell shadows 0000, then the loaded sector is written *into* RAM at
0000 while the PROM is still executing), the board asserts **pin 18 (STA DSB,
status disable)** to tri-state the CPU's status-line buffers and drive the eight
status lines (SM1/SOUT/SINP/SMEMR/SHLTA/SINTA/SWO/SSTACK) plus CLOCK itself, so
each fetch is satisfied from the PROM while the concurrent RAM *write* to 0000 still
lands [Tarbell]. Getting this overlap and the auto-switch-out timing right is a real
risk item ([§14](#14-open-questions-and-risks)).

> **Mechanism (Altair/S-100).** The Tarbell does **not** use PHANTOM\*. It asserts
> **pin 18 — STA DSB (Status Disable)** — to tri-state the CPU's status-line
> buffers, then drives the eight status lines (SM1, SOUT, SINP, SMEMR, SHLTA, SINTA,
> SWO, SSTACK) plus CLOCK itself, so each instruction fetch is satisfied from the
> 82S123 PROM while the byte written to 0000 still lands in regular RAM. Reads are
> gated by `pDBIN` + `sINP*`, writes by `pWR*` + `sOUT` [Tarbell]. The Pico
> personality reproduces exactly this: assert STA DSB and source the status lines
> for the ROM window. If a *different* board in the target set takes the bus by some
> other means, name it and it becomes another bus-takeover case in
> [§7](#7-s100-bus-interface-layer-pio).

---

## 9. Disk geometry & image formats

### 9.1 The geometry problem

"Mixed densities and sector sizes" and "track/head/sector with hard sectoring"
means **no single flat layout works**. Concrete cases from the manuals:

- 8″ FM IBM 3740: 77 trk × 26 sec × 128 B [Tarbell][Cromemco].
- 8″ MFM: 77 × 26 × 256, or 50 × 128 (VersaFloppy density table [VersaFloppy]).
- 5¼″ FM: 40/35 × 18 × 128 [Cromemco][VersaFloppy].
- North Star SD: 35 × 10 × 256 (+16-byte preamble + check byte = 274 B/sector),
  DD: 35 × 10 × 512 (547 B/sector), **non-IBM** [NorthStar].
- Altair 88-DCDD: 77 × 32 × 137 (incl. sync), hard-sectored [88-DCDD]; 88-MDS:
  35 × 16 × 137, MFM hard-sectored [88-MDS].

### 9.2 Division of labor: logical sectors vs on-disk framing

To keep the ESP32 image code sane and the timing on the Pico:

- **ESP32 `MediaType` serves *logical* sector payloads.** Given
  `(drive, cyl, head, sector, size)` it returns exactly the user-data bytes, and
  stores them in an image. It knows geometry and file layout; it does **not**
  produce CRCs, address marks, gaps, sync bits, or North Star check bytes.
- **The Pico personality adds/strips on-disk framing.** WD cores compute the
  ID-field and data CRCs and honor gap/format; hard-sector personalities add the
  sync bit and (North Star) the rotate-XOR check byte. This is where the format
  quirks live, next to the timing that needs them.

So `DISK_READ_SECTOR` over FujiBus carries clean payload bytes; the Pico wraps
them for the wire. `DISK_WRITE_TRACK` (format) is the exception — the Pico may send
the parsed sector map to the ESP32 so the image's geometry is updated.

### 9.3 Image containers

**Two containers, both geometry-aware**, chosen by the ESP32 `MediaType` selector
(the same compile-time name-shadowing pattern as every other platform's media —
see firmware-architecture.md §8.1):

| Format | Use | Notes |
|---|---|---|
| **IMD-style** (ImageDisk) | primary for soft-sectored WD/µPD765 media, and anything with mixed geometry | Per-track mode (FM/MFM), cylinder, head, sector count, **sector size**, and a sector-number map — natively represents mixed density/size and interleave. This is the container that makes "mixed densities and sector sizes" work without external metadata. |
| **Raw `.dsk`** | flat sector images — the common interchange form, and the well-known fixed layouts | A flat run of sector payloads. Geometry is either **inferred from size** for known types (e.g. FDC+ 8″ 330K / Minidisk 75K [FDC+], Tarbell 243K [Tarbell], the Tarbell #2022 mixed-density 499,456-byte CP/M image [Tarbell]) or supplied by a small geometry descriptor. **Hard-sectored images (North Star 89.6K/179.2K [NorthStar], Altair) are raw `.dsk` too** — the sector payload lives in the file; the Pico personality adds the sync bit / North Star rotate-XOR check byte on the wire ([§9.2](#92-division-of-labor-logical-sectors-vs-on-disk-framing)). |

TD0/HxC and other interchange formats are explicitly **out of scope** for this
design; IMD-style and raw `.dsk` cover the target set.

`MediaType::mount()` sniffs the container and fills a `geometry` structure
(tracks, heads, per-track sector count/size/density, sectoring). `DISK_GEOMETRY`
returns this to the Pico so its FDC core can answer Read Address / seek limits
correctly.

For raw `.dsk`, size-based sniffing is a heuristic; when it is ambiguous the
mounted personality's geometry (or an explicit descriptor) disambiguates, since
the ESP32 knows which board is being emulated.

---

## 10. UART emulation

### 10.1 Generic UART core + personalities

The serial side mirrors the disk side: a generic UART core with per-chip register
views and per-board port maps. The core holds a TX byte, an RX byte/FIFO, and a
status/modem-line state; the personality maps ports and defines the register
layout and — critically for discrete boards — the **status-bit positions**.

Chips (all cited):

- **MC6850 ACIA** (Altair 2SIO) — two addresses: RS=0 → control(W)/status(R),
  RS=1 → TX(W)/RX(R). Control: bits1,0 = ÷1/÷16/÷64/master-reset; bits4-2 =
  word/parity/stop; bits6,5 = RTS/TX-int; bit7 = RX-int-enable. Status: bit0 RDRF,
  bit1 TDRE, bit2 DCD, bit3 CTS, bit4 FE, bit5 OVRN, bit6 PE, bit7 IRQ. Master
  reset (bits1,0=11) must precede config; CTS high inhibits TDRE; DCD high forces
  RDRF empty [6850][Altair 2SIO].
- **Intel 8251/8251A** (IMSAI SIO-2) — C/D̄ line selects data vs control/status;
  a **mode-then-command** programming sequence after reset, and an internal-reset
  command bit that rewinds to mode [Intel 8251]. Status: bit0 TxRDY, bit1 RxRDY,
  bit2 TxEMPTY, bits PE/OE/FE, bit7 DSR [Intel 8251]. On IMSAI, **CTS/DCD and the
  interrupt-enable live in a separate board control port (base+8), not the 8251**
  [IMSAI SIO-2].
- **Signetics 2651** — four ports (A1/A0): data, status, mode (MR1→MR2 pointer),
  command; on-chip baud generator [Signetics 2651].
- **COM2502 / 1602-family (discrete)** (MITS 88-SIO, SSM IO-4) — framing set by
  **hardware pins**, not registers; the chip exposes status *signals* (TBMT, RDA,
  ROR, RFE, RPE) that **the board wires to specific data-bus bit positions**
  [com2502]. **The 88-SIO has two incompatible status-word layouts** (original vs
  Rev-1/modified) [88-SIO], and the SSM IO-4 lets any status signal strap to any
  bit at any polarity, with documented "personality" strappings that mimic
  8251/Altair-Rev0/Altair-Rev1/Processor-Tech/IMSAI [SSM IO-4].

### 10.2 The board-defined-status trap

Because discrete-UART status positions are a *board* property, the UART personality
descriptor must carry an explicit **status-bit map** (which bus bit each of
RDRF/TDRE/DCD/CTS/errors lands on, and its polarity), not just a chip type. The
88-SIO's dual layout and the SSM IO-4's straps are the reason this is data, not
code [88-SIO][SSM IO-4][com2502].

### 10.3 Where the bytes go

The emulated UART's byte stream is tunneled over FujiBus to a FujiNet endpoint:
either the **modem** device (for a Hayes-style modem / `telnet` session) or an
**N:** network endpoint (see firmware-architecture.md §8, `network-protocol/`).
Line settings (baud/bits/parity) are **advisory** — the host owns them, and on a
clean byte transport the PE/OE/FE bits are modeled as never-set, an emulation
stance the reference files themselves recommend [Intel 8251][Signetics 2651][com2502].

---

## 11. FujiBus protocol for this board

FujiBus/SLIP framing is unchanged from firmware-architecture.md §2.5 and
`lib/bus/rs232/FujiBusPacket.*`: SLIP-delimited (END 0xC0, ESC 0xDB…), a
`device`/`command` header, typed params, an optional payload, a checksum. We define
device IDs and a command set for this front-end. These MUST be mirrored on the
ESP32 side in `include/fujiDeviceID.h` / `include/fujiCommandID.h` and the new
`s100` bus/device code.

**Device IDs** (the frame `device` field)

| ID | Meaning |
|---|---|
| `0x70` FUJINET | ESP32 control: mount/unmount/list, config-UI bridge (Pico→ESP32) |
| `0x31…` DISK0.. | one per emulated drive (Pico→ESP32) |
| `0x50…` SERIAL0.. | one per emulated serial channel (Pico→ESP32) |
| `0xFF` DBC | the front-end controller itself (the Pico), target of ESP32→Pico config pushes. This is the canonical `FUJI_DEVICEID_DBC` from `include/fujiDeviceID.h` ("bus controller itself"), reused here rather than inventing a new ID. |

**Command IDs**

_Front-end control (ESP32 ⇄ Pico)_
| Cmd | Dir | Payload / effect |
|---|---|---|
| `HELLO` | Pico→ESP32 | firmware version + capabilities on boot |
| `LOAD_PERSONALITY` | ESP32→Pico | one personality descriptor ([§12](#12-personality-descriptor-wire-format)) |
| `APPLY` | ESP32→Pico | commit loaded personalities, (re)init PIO |
| `RESET_FRONTEND` | ESP32→Pico | tear down all SMs, go blank |
| `STATUS` | ESP32→Pico | query state / stats |
| `ROM_FETCH` | Pico→ESP32 | fetch the boot ROM/PROM image for the active personality's window ([§8.4](#84-boot-rom--prom-provisioning)); reply = ROM bytes. (Or the ESP32 pushes it at `APPLY`, as `pico/intellivision` pushes a ROM.) |

_Disk serving (Pico→ESP32)_ — geometry is explicit per request, so mixed
density/size needs no global assumption:
| Cmd | Params | Reply |
|---|---|---|
| `DISK_GEOMETRY` | — | geometry struct (tracks/heads/per-track sectors,size,density,sectoring) |
| `DISK_READ_SECTOR` | cyl, head, sector, size_code | ACK + logical sector bytes |
| `DISK_WRITE_SECTOR` | cyl, head, sector, size_code + payload | ACK |
| `DISK_READ_TRACK` | cyl, head | ACK + track bytes (raw, for Read Track) |
| `DISK_WRITE_TRACK` | cyl, head + parsed sector map | ACK (format) |
| `DISK_READ_ADDRESS` | — | ACK + 6-byte ID field for the next sector |

_Serial (Pico→ESP32)_
| Cmd | Params | Reply |
|---|---|---|
| `UART_CONFIG` | baud, bits, parity, stop (advisory) | ACK |
| `UART_TX` | payload = bytes host wrote | ACK |
| `UART_RX_POLL` | — | ACK + any bytes to feed host (may be empty) |
| `UART_STATUS` | — | modem/line status (DCD/CTS/…) |

Replies come back with command `ACK` (0x06) or `NAK` (0x15) and an optional
payload, exactly as `sendReplyPacket()` does today on the RS232/FujiBus path
(`lib/bus/rs232/rs232.cpp`).

---

## 12. Personality descriptor wire format

The descriptor is the payload of `LOAD_PERSONALITY`. It is **data**: enough to
parameterize the generic cores and select/parameterize a PIO program. Sketch
(little-endian, versioned; final field widths TBD during bring-up):

```
struct s100_personality_wire {
  u8  version;              // descriptor format version
  u8  kind;                 // 0=FDC, 1=UART
  u8  chip_family;          // FDC: FD1771|FD179x|WD177x|UPD765|DISCRETE_NS|DISCRETE_ALTAIR
                            // UART: MC6850|I8251|S2651|DISCRETE_1602
  u8  interface;            // 0=IO_PORTS, 1=MEMORY_MAPPED (North Star)
  u16 base;                 // I/O port base, or memory base (North Star)
  u16 addr_mask;            // decode mask (how many ports/bytes, alignment)
  u8  flow_model;           // POLL_DRQ|WAIT_PORT|PRDY_WAIT|DMA|ENWD_NRDA
  u8  pio_program;          // which Pico PIO program implements the above
  u8  bus_inverted;         // data-bus polarity (FD1771/179x boards)
  u8  flags;                // density(FM/MFM), side_select, motor, etc. bitfield

  // --- register/offset map (meaning depends on chip_family) ---
  u8  reg_offset[8];        // e.g. WD: cmd/status, track, sector, data, control, flags
  // --- control-latch bit assignments (FDC) ---
  struct { u8 drive_sel_shift, drive_sel_mask, side_bit, density_bit,
               motor_bit, autowait_bit; } ctrl;
  // --- status-bit map (UART discrete boards: which bus bit each signal is on) ---
  struct { s8 rdrf, tdre, dcd, cts, fe, ovrn, pe, irq; u8 polarity_mask; } uart_status;

  // --- interrupt wiring ---
  u8  irq_line;             // VI0..VI7 or PINT
  u8  irq_sources;          // INTRQ|EOJ|per-sector|RX|TX bitfield

  // --- boot ROM / PROM window (design §8.4) ---
  u16 rom_base;             // host memory base of the ROM window (0 = none)
  u16 rom_size;             // bytes (Tarbell 32, Cromemco 1K-8K, ...)
  u8  rom_gated_on_reset;   // 1 = PROM enabled by RESET, auto-switches out (Tarbell)
  u8  rom_disable_kind;     // NONE | OUT_PORT_BIT (Cromemco 40h / CompuPro bit)
  u16 rom_disable_port; u8 rom_disable_bit;
  u8  bus_takeover;         // 1 = assert pin 18 STA DSB & drive the status lines
                            //     while ROM enabled and shadowing RAM (Tarbell)

  // --- geometry hint (for FDC; authoritative geometry comes from DISK_GEOMETRY) ---
  u8  tracks, heads; u8 sectoring; /* soft | hard10 | hard16 | hard32 */
  // optional trailer: raw PIO program bytes if pushing a program not baked in
};
```

Notes:
- `flow_model` + `pio_program` are what let the ESP32 "reconfigure the PIO on the
  fly." Most boards use a baked-in program selected by index; the optional trailer
  allows pushing a whole PIO program for a board the Pico firmware didn't ship.
- `uart_status` is why the discrete-UART trap ([§10.2](#102-the-board-defined-status-trap))
  is handled as data: the 88-SIO's two layouts and the SSM IO-4 straps are just two
  different descriptors [88-SIO][SSM IO-4].
- The ESP32 stores a library of these (one per emulated board) as small resource
  files and lets the user pick in the web UI.

---

## 13. ESP32 / FujiNet side

**Why a new bus at all — and why S100 departs from the "reuse rs232" rule.** The
Platform Bring-Up Guide's headline ESP32-side claim (FEP-004, Ch. 11–13) is that a
tandem platform usually writes **no new bus or device classes**: the RP2350 is a
*transparent* FujiBus byte pipe (the host itself speaks FujiBus through its ROM /
client library), so the ESP32 just reuses the existing `rs232`/`BUILD_RS232` bus,
adding only a build target and pin map. **S100 is the exception, and deliberately
so.** Here the Pico is not a transparent pipe — it emulates a WD1793 (etc.) to the
host and **translates** those register cycles into FujiBus requests it originates
itself ([§4](#4-the-pico-is-the-fujibus-client)). That reversed request direction,
the geometry-aware media (IMD / hard-sectored), and the personality-push control
channel are what the stock `rs232` device set does not cover — which is exactly the
guide's stated trigger for adding code ("only when your platform exposes a device
the existing set does not, or a disk image format not already handled"). Where the
transport itself is concerned we still reuse `FujiBusPacket` framing over an
`IOChannel`, unchanged. New per-`BUILD_*` subsystem, following the patterns in
firmware-architecture.md §3–§6, §8:

- **`lib/bus/s100/`** — a `systemBus : public SystemBusBase` whose transport is an
  `IOChannel` (USB-CDC `ACMChannel` or `UARTChannel`, per firmware-architecture.md
  §8 `hardware/`) carrying **FujiBus packets** (reuse `FujiBusPacket` framing). Its
  `service()` reads a SLIP frame, looks the device up in the bus's
  `std::forward_list<virtualDevice*> _daisyChain` (via `deviceById()`, per
  firmware-architecture.md §3 — there is no separate `DaisyChain` class), and
  dispatches. Because the Pico is the client, this bus mostly **answers** requests
  and occasionally **pushes** config frames to device `0xFF` (DBC).
- **`lib/device/s100/`** — `s100Disk : virtualDevice` (holds a `MediaType *`,
  answers the `DISK_*` commands), `s100Serial`/reuse of the shared `modem`/
  `network` bases (answers `UART_*`), and an `s100Fuji` control device for
  mount/list/personality-selection.
- **`lib/media/s100/`** — a geometry-aware `MediaType` (its own class, per the
  name-shadowing convention) plus `MediaTypeIMD`, `MediaTypeRawDSK`,
  `MediaTypeNSI` subclasses ([§9.3](#93-image-containers)).
- **Personality library** — resource files + a small manager the web UI drives,
  emitting `LOAD_PERSONALITY`/`APPLY` frames.
- **Registration** — a `BUILD_S100FDC` (name TBD to avoid colliding with the
  existing `BUILD_S100`/`s100spi`) block in `lib/bus/bus.h`, the media selector in
  `lib/media/media.h`, the device selectors in `lib/device/*.h`, and a
  `platformio-*.ini` board, exactly as firmware-architecture.md §6 describes.

Templates for the bus, device, and media classes are in
[Appendix B](#appendix-b--esp32-side-templates).

---

## 14. Open questions and risks

1. **Round-trip latency vs first-DRQ (the #1 risk).** The sector cache +
   read-ahead + native-stall strategy ([§4.1](#41-latency-and-the-sector-cache))
   must be validated against the *tightest* real timing: WD177x ~23.5 µs/byte MFM
   [WD177X], and boards with no stall mechanism at all in some modes. Measure the
   FujiBus round-trip (USB-CDC vs UART) early; it sets the read-ahead depth.
2. **DMA mastering for CompuPro.** Becoming an S100 temporary bus master via
   **PHOLD/PHLDA** to drive a DMA transfer [CompuPro] is a large,
   backplane-timing-sensitive PIO effort. Candidate for a **later phase**; ship
   PIO/wait-state boards first.
3. **Data-bus polarity & backplane electricals.** FD1771/179x inverted DAL
   [Tarbell][VersaFloppy] and the Altair/S100 **separate data-in / data-out** buses
   both need attention in hardware. On voltage, distinguish two things the Platform
   Bring-Up Guide (Ch. 1, Ch. 7) is careful to separate: the **RP2350 is 5 V
   tolerant and can connect to 5 V bus levels directly** — level-shifting purely for
   *protection* is not required and is called out in that guide as a common mistake.
   What an S100 board *does* still need is **bus drive and signal integrity on a
   long, heavily-loaded backplane** (many card slots, high capacitance, the separate
   DI/DO buses) — i.e. proper S100 bus transceivers/buffers, which the guide agrees
   are warranted "on a long or heavily-loaded bus." So: transceivers for drive and
   loading, **not** level-shifters for protection. (Verify the RP2350 5 V-tolerance
   claim against the current RP2350 datasheet/errata before committing the design;
   the guide notes it reversed position on this point across revisions.) All of this
   is out of scope for the software design but gating for the board.
4. **Manual gaps we must resolve on hardware, not guess** (from the extraction):
   - FDC+ does **not** document its serial wire protocol or reprint register bit
     fields (defers to 88-DCDD/88-MDS) [FDC+] — we define our own protocol anyway.
   - Cromemco 16FDC/64FDC and CompuPro give **no fixed geometry table** (chip/format
     detected) [Cromemco][CompuPro] — geometry comes from the image via
     `DISK_GEOMETRY`.
   - 88-SIO dual status-word layout and CR6/CR5-break-polarity disagreement between
     the 2SIO manual and the 6850 datasheet [88-SIO][Altair 2SIO][6850] — expose
     both as personalities; verify against real software.
   - Tarbell's WAIT-port bit (bit6 vs bit7 for DRQ) contradicts its own bootstrap;
     **trust the bootstrap** [Tarbell].
5. **North Star's memory-mapped interface** means the bus layer must decode memory
   cycles, not just I/O — a distinct PIO program and address-width path [NorthStar].
6. **Write-protect, formatting, and image write-back** semantics per format
   (esp. hard-sectored) need care so a guest `FORMAT` produces a valid image.
7. **Which link to ship first** — USB-CDC (RP2350 device / ESP32-S3 host, like
   `pico/intellivision`) is the recommended default for throughput; a UART backend
   is the fallback. Design the transport as an abstraction with both.
8. **RP2350 PIO GPIO-bank layout** ([§7](#7-s100-bus-interface-layer-pio)). The
   S100 signal set exceeds 32 GPIO, so the pin map is constrained by three RP2350
   rules: `PICO_PIO_USE_GPIO_BASE=1` for pins above 31, no state-machine pin span
   straddling the 16↔32 boundary, and single-instruction autopush capturing only
   the low 32 GPIO. Getting the GPIO grouping wrong forces a redesign late; settle
   it before the adapter layout. A concrete starting assignment that satisfies all
   three rules is proposed in [§7.1](#71-a-possible-gpio--s100-signal-map).

---

## 15. Implementation roadmap

Order the work as a **milestone ladder** in the sense of the Platform Bring-Up
Guide (FEP-004, Ch. 10, Ch. 17): each rung is independently testable and a failure
is contained to the rung you are on. Crucially, the guide's discipline is to prove
the **byte pipe and a real FujiBus round-trip on the bench — over USB, with no S100
card in existence yet — before fabricating hardware**, following the `fujinet-bringup`
MVP (a minimal byte relay + `iotest` loopback). That directly de-risks this design's
#1 unknown (round-trip latency, [§14](#14-open-questions-and-risks) risk 1), so step
0 below exists before any board is built:

0. **Bench loopback + FujiBus ACK (no S100 card).** Wire the RP2350 to the ESP32
   over USB-CDC. Prove a byte injected on one side reaches the other (loopback),
   then that a real `FujiBusPacket` request from the Pico gets an `ACK` (+data)
   reply from an ESP32 `s100` device stub. **Measure the round-trip time here** — it
   sets the sector-cache read-ahead depth ([§4.1](#41-latency-and-the-sector-cache))
   and decides USB-CDC vs UART before the adapter is laid out.
1. **Codec + skeletons.** FujiBus client on the Pico (port of
   `FujiBusPacket.cpp`, desktop self-test); `s100` bus/device/media skeletons on
   the ESP32; personality descriptor + `LOAD_PERSONALITY`/`APPLY` handshake.
2. **One WD board, read-only, PIO-poll — including its boot ROM.** Cromemco 16FDC
   (FD1793) read path: S100 I/O decode PIO, WD core Type I/II read, sector cache,
   `DISK_READ_SECTOR`, **plus serving the RDOS boot ROM at C000h** (memory-cycle
   response + `ROM_FETCH`) so the machine actually boots. Prove the timing on real
   hardware. (Tarbell's **pin-18** status-disable RAM-shadowing ROM is a harder variant — do it
   with the Tarbell personality in a later step.)
3. **Write + format** for that board (`DISK_WRITE_SECTOR`, Write Track).
4. **Stall models.** Add WAIT-port (Tarbell) and PRDY-wait (VersaFloppy/North
   Star), then the memory-mapped path (North Star).
5. **Hard-sectored.** 88-DCDD/88-MDS ENWD/NRDA personality + sector-pulse synthesis
   (this is the "Altair" in the name).
6. **Serial.** Generic UART core + 6850 (2SIO) first, then 8251 (IMSAI) and the
   discrete 88-SIO/SSM personalities; tunnel to modem/N:.
7. **DMA.** CompuPro µPD765 core + S100 DMA mastering (heaviest; last).
8. **Web UI** personality picker + image mounting.

---

## Appendix A — Pico-side templates

> Templates/scaffold. Not compiled by `build.sh`; build with the pico-sdk like the
> other `pico/*` projects (see firmware-architecture.md §9). `// TODO` marks the
> backplane-timing- and personality-specific parts that must be filled in and
> validated on hardware.
>
> **Relationship to `fujiversal`.** The Platform Bring-Up Guide's general RP2350
> pattern is `fujiversal` — a single Pico-SDK firmware that emulates ROM + I/O
> registers and acts as a *transparent* FujiBus byte pipe, selected per board by a
> `.pio` file. This S100 front-end deliberately is **not** that: it is a translating
> FDC/UART emulator with a sector cache, multiple flow-control PIO programs, and its
> own FujiBus *client* logic ([§4](#4-the-pico-is-the-fujibus-client),
> [§13](#13-esp32--fujinet-side)), so it is scaffolded as a standalone pico project
> rather than a `fujiversal` board. It still reuses `fujiversal`'s ROM-emulation
> idea ([§8.4](#84-boot-rom--prom-provisioning)) and the FujiBus wire format. The
> bench bring-up (roadmap step 0) follows the `fujinet-bringup` loopback method.

### A.1 `fujibus.h` — FujiBus/SLIP client (Pico is the client)

```c
// Bit-compatible plain-C port of lib/bus/rs232/FujiBusPacket.cpp, à la
// pico/intellivision/firmware/src/fujibus.c. The Pico issues requests and reads
// ACK/NAK(+data) replies; the ESP32 may push config frames to S100_DEVICEID_PICO.
#ifndef S100_FUJIBUS_H
#define S100_FUJIBUS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum { SLIP_END=0xC0, SLIP_ESCAPE=0xDB, SLIP_ESC_END=0xDC, SLIP_ESC_ESC=0xDD };

// Device IDs (frame `device` field)
#define S100_DEVICEID_FUJINET 0x70   // ESP32 control (mount/list/config bridge)
#define S100_DEVICEID_DISK0   0x31   // DISK0..DISK0+n
#define S100_DEVICEID_SERIAL0 0x50   // SERIAL0..+n
#define S100_DEVICEID_PICO    0xFF   // == FUJI_DEVICEID_DBC (bus controller itself);
                                     // ESP32 -> Pico config pushes land here

// Command IDs.
// NOTE: ACK/NAK are REPLY command bytes (== FUJICMD_ACK/FUJICMD_NAK in
// include/fujiCommandID.h). Because a FujiBus reply reuses the `command` field
// for ACK/NAK, no REQUEST command below may reuse 0x06 or 0x15.
#define S100_ACK 0x06
#define S100_NAK 0x15
#define S100_CMD_HELLO            0x01
#define S100_CMD_LOAD_PERSONALITY 0x02
#define S100_CMD_APPLY            0x03
#define S100_CMD_RESET_FRONTEND   0x04
#define S100_CMD_STATUS           0x05
#define S100_CMD_ROM_FETCH        0x07   // fetch boot ROM/PROM image (design §8.4)
#define S100_CMD_DISK_GEOMETRY    0x10
#define S100_CMD_DISK_READ_SECTOR 0x11
#define S100_CMD_DISK_WRITE_SECTOR 0x12
#define S100_CMD_DISK_READ_TRACK  0x13
#define S100_CMD_DISK_WRITE_TRACK 0x14
#define S100_CMD_DISK_READ_ADDRESS 0x16   // NOT 0x15 -- that value is NAK (see above)
#define S100_CMD_UART_CONFIG      0x20
#define S100_CMD_UART_TX          0x21
#define S100_CMD_UART_RX_POLL     0x22
#define S100_CMD_UART_STATUS      0x23

typedef enum { FB_OK=0, FB_ENOLINK, FB_ETIMEOUT, FB_EBADFRAME, FB_ETOOBIG } fb_status_t;
typedef struct { uint32_t value; uint8_t size; } fb_param_t;         // size 1/2/4, LE
typedef struct { uint8_t device, command; const uint8_t *data; uint16_t data_len; } fb_reply_t;

size_t fujibus_build_request(uint8_t device, uint8_t command,
                             const fb_param_t *params, unsigned nparams,
                             const uint8_t *payload, uint16_t payload_len,
                             uint8_t *out, size_t out_cap);
bool   fujibus_parse_reply(const uint8_t *in, size_t in_len, fb_reply_t *reply);
bool   fujibus_selftest(void);   // in-memory, no hardware
#endif
```

### A.2 `fdc.h` — generic FDC backend + WD179x core

```c
// One internal interface for every disk personality (WD179x, uPD765, discrete
// North Star / Altair). The rest of the firmware only sees fdc_backend.
#ifndef S100_FDC_H
#define S100_FDC_H
#include <stdint.h>
#include <stdbool.h>
#include "personality.h"   // struct s100_personality (parsed from the wire form)

typedef struct fdc_backend fdc_backend;
struct fdc_backend {
    void (*apply)(fdc_backend*, const struct s100_personality*);
    uint8_t (*bus_read)(fdc_backend*, uint16_t offset);          // S100 read cycle
    void    (*bus_write)(fdc_backend*, uint16_t offset, uint8_t v);
    bool    (*wants_stall)(fdc_backend*);       // for PRDY/WAIT-port models
    bool    (*irq_asserted)(fdc_backend*);
    void    (*reset)(fdc_backend*);             // POC*/RESET*
    void   *state;
};

// Concrete cores (each returns an fdc_backend*):
fdc_backend *fdc_wd179x_create(void);    // FD1771 / FD1791 / FD1793 / WD177x
fdc_backend *fdc_upd765_create(void);    // CompuPro
fdc_backend *fdc_discrete_ns_create(void);   // North Star (memory-mapped)
fdc_backend *fdc_discrete_altair_create(void); // 88-DCDD / 88-MDS / FDC+

// ---- WD179x core state (illustrative) ----
typedef struct {
    uint8_t cmd, status, track, sector, data;   // the five registers [FD1771][WD177X]
    uint8_t last_cmd_type;                       // status is command-type dependent
    bool    drq, intrq, busy;
    bool    fm_only;         // FD1771: no MFM/DDEN
    bool    has_side_select; // WD1773/1793 only
    bool    bus_inverted;    // DAL polarity (board-wrapped)
    // sector cache filled via FujiBus DISK_READ_SECTOR before DRQ streaming:
    uint8_t cache[1024]; uint16_t cache_len, cache_pos;
    struct s100_personality pers;
} wd179x_state;

// TODO: Type I-IV decode; per-command status view; DRQ pacing from cache;
// CRC + address-mark generation on read; INTRQ on completion.
#endif
```

### A.3 `s100_bus.pio` — I/O-cycle decode + drive-data (template)

```
; TEMPLATE. Decodes one S100 I/O cycle for an I/O-ported personality and hands
; (cycle_type, port_offset, wr_data) to core-1. The five flow-control models of
; design §7 are SEPARATE programs selected at s100_apply_personality() time; this
; is the common PIO-poll / no-stall read/write decoder.
;
; Pin groups (assigned at init from the board wiring; see main.c):
;   A0..A7   host I/O address       (IN pins)
;   D0..D7   data bus               (IN for host writes / OUT for host reads)
;   sINP,sOUT,pDBIN,pWR             status/strobe            (IN pins)
;
; TODO: match personality base/mask on A0..A7; distinguish sINP (read) vs sOUT
; (write); on read, drive D0..D7 from a value core-1 has staged (respecting
; bus_inverted); on write, latch D0..D7 and push to the RX FIFO; assert /PRDY only
; for the wait models (separate program). Timing must meet the specific backplane.

.program s100_io_decode
    ; ... TODO: real cycle decode; this stub only sketches the control flow ...
    wait 1 pin 0        ; TODO: wait for this board's I/O cycle (status decode)
    in   pins, 8        ; sample A0..A7 -> ISR (port address)
    push block          ; hand address to core-1
    ; core-1 decides read vs write and stages/consumes data; a companion SM drives
    ; or samples D0..D7 within the strobe window. TODO.
```

### A.4 `main.c` — dual-core wiring (template)

```c
// Core0: FujiBus link to the ESP32 (config in, disk/serial requests out).
// Core1: the S100 real-time service loop (PIO IRQs -> emulated cores).
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "fujibus.h"
#include "fdc.h"
#include "uart_emu.h"
#include "personality.h"
#include "transport.h"   // usb-cdc or uart backend, build-time selected

static fdc_backend *g_fdc;          // active disk personality (NULL until APPLY)
static uart_core   *g_uart[2];      // active serial channels
static PIO          g_pio = pio0;

// Called when the ESP32 sends LOAD_PERSONALITY + APPLY. Reconfigures PIO live.
void s100_apply_personality(const struct s100_personality *p) {
    // TODO: stop + unload current PIO programs / SMs
    // TODO: pick core by p->chip_family; g_fdc = fdc_wd179x_create()/... ; g_fdc->apply()
    // TODO: load p->pio_program into g_pio with p->base/addr_mask/pin bindings
    // TODO: start the flow-control SM matching p->flow_model
}

static void core1_main(void) {
    for (;;) {
        // TODO: block on PIO IRQ = one decoded S100 cycle
        // TODO: route offset -> g_fdc->bus_read/bus_write or g_uart[n]
        // TODO: on WD Read Sector with empty cache, request fill via a core0 mailbox
        //       and use the personality's stall model to hold the bus meanwhile (§4.1)
    }
}

int main(void) {
    stdio_init_all();
    transport_init();                 // USB-CDC or UART to ESP32
    fujibus_selftest();               // sanity
    // Announce ourselves; ESP32 will push personalities.
    // TODO: send S100_CMD_HELLO, then loop handling LOAD_PERSONALITY/APPLY.
    multicore_launch_core1(core1_main);
    for (;;) {
        // TODO: core0 - drain config pushes from ESP32; service disk/serial
        // fetch mailboxes from core1 by issuing FujiBus requests and returning data.
    }
}
```

### A.5 `uart_emu.h` — generic UART core (template)

```c
#ifndef S100_UART_EMU_H
#define S100_UART_EMU_H
#include <stdint.h>
#include <stdbool.h>
#include "personality.h"

typedef struct {
    uint8_t tx, rx;                 // holding registers
    bool    rdrf, tdre;             // receive-full / transmit-empty
    bool    dcd, cts;               // modem lines (from FujiNet endpoint)
    uint8_t chip_family;            // MC6850 | I8251 | S2651 | DISCRETE_1602
    // For discrete boards the STATUS BIT POSITIONS come from the personality,
    // not the chip -- see design §10.2 (88-SIO dual layout, SSM IO-4 straps).
    struct s100_uart_statusmap smap;
    // 8251 needs a mode-then-command sequencer; 2651 an MR1->MR2 pointer:
    uint8_t prog_state;
} uart_core;

uint8_t uart_bus_read(uart_core*, uint16_t offset);    // status/data per personality
void    uart_bus_write(uart_core*, uint16_t offset, uint8_t v);
uint8_t uart_status_byte(uart_core*);   // assembles status using smap (board-defined)
#endif
```

---

## Appendix B — ESP32-side templates

> Templates. On the ESP32 these belong under `lib/bus/s100/`, `lib/device/s100/`,
> `lib/media/s100/` and are wired into the selectors per firmware-architecture.md
> §6. Kept here (not created in the tree) so nothing half-built enters a build.
> The bus reuses `FujiBusPacket` framing from `lib/bus/rs232/`.

### B.1 `s100.h` — the bus (`systemBus : SystemBusBase`)

```cpp
#ifndef S100_H
#define S100_H
#include "bus.h"              // SystemBusBase (registry is a per-bus _daisyChain list)
#include "FujiBusPacket.h"    // reuse the RS232/FujiBus SLIP framing
#include "IOChannel.h"        // ACMChannel (USB-CDC) or UARTChannel

class s100Disk;   // fwd
class s100Fuji;

class systemBus : public SystemBusBase {
    IOChannel      *_port = nullptr;      // link to the Pico front-end
    FujiBusPacket  *_activePacket = nullptr;
    virtualDevice  *_activeDev = nullptr;

    void _s100_process_cmd();             // read one SLIP frame, dispatch
public:
    void setup();
    void service();                       // called from fn_service_loop()
    void shutdown();
    void addDevice(virtualDevice *pDevice, fujiDeviceID_t device_id) override;

    // Here the Pico is the CLIENT: we mostly answer its requests, and PUSH config.
    void pushPersonality(const uint8_t *descriptor, size_t len);  // -> device 0xFF (DBC)

    // Transaction contract (SystemBusBase). Replies via FujiBus ACK/NAK(+data),
    // exactly like rs232.cpp's sendReplyPacket().
    void transaction_accept(transState_t expectMoreData) override;
    void transaction_success() override;
    void transaction_error() override;
    success_is_true transaction_get(void *data, size_t len) override;
    void transaction_send(const void *data, size_t len, bool is_error=false) override;
};
extern systemBus SYSTEM_BUS;
#endif
```

### B.2 `disk.h` — the disk device (`s100Disk : virtualDevice`)

```cpp
#ifndef S100_DISK_H
#define S100_DISK_H
#include "bus.h"
#include "../media/s100/mediaType.h"   // geometry-aware MediaType (see B.3)

class s100Disk : public virtualDevice {
    MediaType *_disk = nullptr;        // holds the image; never parses the bus
protected:
    void s100_process(const FujiBusPacket &packet);   // dispatch DISK_* commands
    void s100_read_sector(const FujiBusPacket &p);     // -> transaction_send(payload)
    void s100_write_sector(const FujiBusPacket &p);
    void s100_geometry(const FujiBusPacket &p);        // -> geometry struct
    void s100_read_address(const FujiBusPacket &p);
    void s100_write_track(const FujiBusPacket &p);     // format
public:
    mediatype_t mount(fnFile *f, uint32_t disksize);   // sniff container, fill geometry
    void unmount();
    bool write_blank(fnFile *f, /*geometry*/...);      // create a new image
};
#endif
```

### B.3 `mediaType.h` — geometry-aware media base (S100's own copy)

```cpp
// S100's own class named MediaType (compile-time name-shadowing, per
// firmware-architecture.md §8.1). Serves LOGICAL sector payloads only; the Pico
// adds CRC / sync / check bytes (design §9.2).
#ifndef S100_MEDIATYPE_H
#define S100_MEDIATYPE_H
#include <cstdint>
#include "fnFile.h"

enum mediatype_t { MEDIATYPE_UNKNOWN=0, MEDIATYPE_IMD, MEDIATYPE_RAW_DSK,
                   MEDIATYPE_COUNT };   // two containers: IMD-style + raw .dsk
enum sectoring_t { SECT_SOFT=0, SECT_HARD_10, SECT_HARD_16, SECT_HARD_32 };

struct s100_geometry {              // returned to the Pico via DISK_GEOMETRY
    uint8_t  tracks, heads;
    sectoring_t sectoring;
    // Per-track descriptors (mixed density/size are first-class here):
    struct track_desc { uint8_t nsectors; uint16_t sector_size; uint8_t density_mfm;
                        uint8_t first_sector_id; } tracks_desc[/*max*/ 80][2];
};

class MediaType {
protected:
    fnFile        *_media_fileh = nullptr;
    uint32_t       _image_size  = 0;
    s100_geometry  _geo{};
public:
    virtual mediatype_t mount(fnFile *f, uint32_t disksize) = 0;   // sniff + fill _geo
    virtual void        unmount();
    // logical sector payload (no framing):
    virtual bool read (uint8_t cyl, uint8_t head, uint8_t sec, uint16_t size,
                       uint8_t *out, uint16_t *outlen) = 0;
    virtual bool write(uint8_t cyl, uint8_t head, uint8_t sec, uint16_t size,
                       const uint8_t *in, uint16_t inlen) = 0;
    virtual bool format_track(uint8_t cyl, uint8_t head /*, parsed map*/);
    const s100_geometry &geometry() const { return _geo; }
    static mediatype_t discover_mediatype(const char *filename);
    virtual ~MediaType();
};

// Two subclasses (design §9.3). Hard-sectored North Star/Altair images are raw
// .dsk; the Pico personality adds sync bit / rotate-XOR check byte on the wire.
class MediaTypeIMD    : public MediaType { /* per-track FM/MFM, size, sector map   */ };
class MediaTypeRawDSK : public MediaType { /* flat image; geometry by size or descr */ };
#endif
```

---

## Appendix C — reference sources

Board/chip facts in this document are cited inline with the tags below, each a file
under `~/src/altairsim/reference/`. Anything a manual does not state is flagged in
the text rather than inferred.

| Tag | File |
|---|---|
| [FD1771] | `Western Digital FD1771 - Datasheet.md` |
| [WD177X] | `Western Digital WD177X-00 - Datasheet.md` |
| [Cromemco] | `Cromemco 4FDC 16FDC 64FDC Floppy Controllers.md` |
| [CompuPro] | `CompuPro Disk 1 & 1A Floppy Controllers.md` |
| [NorthStar] | `North Star MDS Floppy Controllers.md` |
| [88-DCDD] | `Altair Floppy (88-DCDD) Manual.md` |
| [88-MDS] | `88-MDS Minidisk Manual.md` |
| [Altair8800] | `Altair 8800 Theory of Operation.md` (880-110 bus pinout) |
| [Tarbell] | `Tarbell_Floppy_Disk_Interface_Manual.md` |
| [VersaFloppy] | `SD Systems VersaFloppy.md` |
| [FDC+] | `FDC+ Manual.md` |
| [6850] | `6850.md` |
| [Altair 2SIO] | `Altair 2SIO User's Manual.md` |
| [88-SIO] | `88-SIO Rev 0 & 1.md` |
| [Intel 8251] | `Intel 8251 USART.md` |
| [Signetics 2651] | `Signetics 2651 USART.md` |
| [com2502] | `com2502.md` |
| [IMSAI SIO-2] | `IMSAI SIO-2.md` |
| [SSM IO-4] | `SSM IO-4 2P+2S IO Board.md` |
| [Console IO] | `Console IO Board.md` |

Internal references: [`firmware-architecture.md`](firmware-architecture.md) —
§2.5 (fixed vs SLIP framing), §3 (base classes), §6 (adding a bus), §8 (`lib/`
map & MediaType), §9 (the `pico/` companion firmware).
