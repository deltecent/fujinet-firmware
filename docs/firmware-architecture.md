# FujiNet Firmware Architecture — End to End

This document explains how the FujiNet firmware works from the wire up: how a
command travels from the host computer, across the bus, into a device, and back.
It uses the **Atari SIO** bus as the running example because SIO is the oldest and
best-documented path, then shows the base classes every bus shares and, finally,
how you would add a brand-new bus to the firmware.

> One codebase, two builds. The same C++ runs as ESP32 firmware (PlatformIO +
> ESP-IDF) and as **FujiNet-PC**, a native desktop executable (CMake). Host-vs-PC
> differences hide behind `#ifdef ESP_PLATFORM`; per-computer differences hide
> behind `BUILD_<PLATFORM>` (`BUILD_ATARI`, `BUILD_APPLE`, …). Almost everything
> below is shared between the two builds.

---

## 1. The big picture: a bus with devices

The runtime is a **bus + devices** model, wired up in `src/main.cpp`:

- There is exactly **one** global bus object, `systemBus SYSTEM_BUS`
  (`src/main.cpp:61`). It is the physical link to the host computer.
- The concrete `systemBus` class is chosen at compile time by the `BUILD_*`
  macro. `lib/bus/bus.h` `#include`s the right one and nothing else:

  ```cpp
  #ifdef BUILD_ATARI
  #include "sio/sio.h"          // systemBus == the SIO bus
  #endif
  #ifdef BUILD_APPLE
  #include "iwm/iwm.h"          // systemBus == the IWM/SmartPort bus
  #endif
  // ... one #include per platform ...
  ```

  So `systemBus` is a *different class* in every firmware image, but the rest of
  the code only ever sees the name `systemBus` / `SYSTEM_BUS`.

- **Devices** are attached to the bus with
  `SYSTEM_BUS.addDevice(dev, deviceID)` inside the big per-platform block in
  `main_setup()`. Each device is registered under a numeric **device ID** the
  host uses to address it (e.g. `0x31` = D1: on Atari).

- Every device is a subclass of a per-bus device base — a different concrete
  class per bus, spelled **`virtualDevice`** on almost every bus (SIO, IWM,
  AdamNet, IEC, DriveWire, ComLynx, RS232, RC2014, S100, CX16, H89). The `mac`
  bus is the exception: its base is named `macDevice` (`lib/bus/mac/mac.h:34`).

The one control device present on **every** platform is **`theFuji`** — the
"FUJINET" device. Its bus-independent base is `fujiDevice`
(`lib/device/fujiDevice/fujiDevice.h`), built from a chain of command mixins
(`Base64Mixin`, `HashMixin`, `QRMixin`, `AppKeyMixin`); each bus subclasses it
(SIO's is `sioFuji`, `lib/device/sio/sioFuji.{h,cpp}`, which also defines the
global `sioFuji platformFuji; fujiDevice *theFuji = &platformFuji;`). It handles
configuration, mounting disk images, disk-image rotation, and the FujiNet command
protocol (the web UI and the `fujitools` host utilities talk to it).

```
   Host computer (Atari)                 FujiNet hardware / FujiNet-PC
 ┌────────────────────┐   SIO cable   ┌───────────────────────────────────┐
 │  OS calls SIO to    │ ============> │  IOChannel (UARTChannel / NetSIO)  │
 │  D1: read sector    │  cmd frame    │            │                       │
 └────────────────────┘               │            v                       │
                                       │   systemBus::service()             │
                                       │            │  find device by ID    │
                                       │            v                       │
                                       │   virtualDevice::sio_process()      │
                                       │        (e.g. sioDisk)              │
                                       │            │                       │
                                       │            v  media/FileSystem     │
                                       │   ATR/DSK image on SD or TNFS      │
                                       └───────────────────────────────────┘
```

For a full tour of what lives under `lib/` (media formats, filesystems,
network-protocol, the web UI, and the rest), see §8.

---

## 2. The SIO bus, step by step

Files: `lib/bus/sio/sio.h`, `lib/bus/sio/sio.cpp`. The command-frame and
transport helpers live alongside: `FujiSIOPacket.*`, `NetSIO.*`, `cmdFrame.h`.

### 2.1 The service loop

The main loop in `fn_service_loop()` (`src/main.cpp:513`) calls
`SYSTEM_BUS.service()` over and over. On ESP32 this loop is its own
high-priority task **pinned to CPU1** (`xTaskCreatePinnedToCore(..., MAIN_CPUAFFINITY=1)`,
`src/main.cpp:628`), leaving WiFi on CPU0. On FujiNet-PC the same loop also
services the HTTP server (`fnHTTPD.service()`) and the task manager. (ESP ADAM is
the one exception: it runs the bus in its own core-1 task via `start_bus_task()`.)

`systemBus::service()` (`lib/bus/sio/sio.cpp:362`) each pass:

1. Drains the internal message queue (`_sio_process_queue()` — e.g. disk-swap).
2. Handles special always-on modes if active: NetStream, CP/M, cassette, modem.
3. **If the SIO `CMD` line is asserted**, calls `_sio_process_cmd()` to read and
   dispatch a command frame (`sio.cpp:441`).
4. Polls network protocols for interrupts
   (`_netDev[i]->sio_poll_interrupt()`).

### 2.2 The command frame

The host addresses a device by sending a fixed-size **command frame**. Its layout
is `cmdFrame_t` (`lib/bus/cmdFrame.h`) — 5 bytes for most buses:

```cpp
typedef struct {
    union {
        struct {
            fujiDeviceID_t  device;   // which device (e.g. 0x31 = D1:)
            fujiCommandID_t comnd;    // which command (e.g. 'R' = read)
            union { struct { uint8_t aux1, aux2; }; u16le_t aux12; };
        };
        uint32_t commanddata;         // the 4 bytes the checksum covers
    };
    uint8_t checksum;
} __attribute__((packed)) cmdFrame_t;
static_assert(sizeof(cmdFrame_t) == 5, "cmdFrame_t must be 5 bytes");
```

(`cmdFrame.h` also defines a 7-byte `BUILD_RS232` variant with `aux1..aux4`, but
the RS232 / FujiBus path does not use a fixed frame at all — it uses SLIP-framed
`FujiBusPacket`s; see §2.5.)

The frame is wrapped by **`FujiSIOPacket`** (`FujiSIOPacket.h`), which the rest
of the code passes around. It exposes:

- `packet.device()` / `packet.command()` — the addressed device and command.
- `packet.param(i)` / `param8(i)` / `param16(i)` — typed access to the aux bytes.
- `packet.data()` — the trailing write payload, filled in lazily once its length
  is known (`setDataLength()` / `transaction_get()`).

`FujiSIOPacket` is deliberately **non-copyable** to prevent accidental
pass-by-value of a live frame.

### 2.3 Reading and dispatching a frame

`systemBus::_sio_process_cmd()` (`sio.cpp:195`) is the heart of it:

1. Read `sizeof(cmdFrame_t)` bytes from the active port into a `FujiSIOPacket`
   (`_port->read(...)`). Bail out on a short read.
2. Wait for the `CMD` line to de-assert.
3. Verify the checksum with `sio_checksum()`. On a mismatch, count failures and
   **auto-toggle the baud rate** after a threshold (this is how FujiNet
   negotiates high-speed SIO: it re-tries at the other speed).
4. On a good checksum, find the target device and hand it the frame:

   ```cpp
   _activeDev = _daisyChain.deviceWithFujiID(tmpFrame.device());
   if (_activeDev)
       _activeDev->sio_process(tmpFrame);
   ```

   Special cases handled here: the CONFIG **boot disk** (`_fujiDev->FUJI_BOOTDISK`,
   with boot-priority logic that lets a real D1: take over), and **Type-3 polls**
   (`FUJI_DEVICEID::TYPE3POLL`), which are broadcast to every device whose
   `listen_to_type3_polls` flag is set.

The lookup uses the **`DaisyChain`** (`lib/bus/DaisyChain.h`), which owns the
device registry: `addDevice()`, `deviceWithFujiID()`, `fujiIDForDevice()`,
`rotateDevices()` (for disk rotation), and iteration (`begin()/end()`). It was
introduced to centralize chain management across all buses.

### 2.4 The transaction contract (how a device replies)

A device never touches the serial port directly. (`sio.h` even carries a comment
about this: *"Everybody thinks 'oh I know how a serial port works, I'll just
bypass the bus!' ಠ_ಠ"*.) Instead it drives the bus through the **transaction
API** defined on the shared base `SystemBusBase` (`lib/bus/bus.h`) and
implemented per bus. A transaction is:

1. **Accept** the command: `transaction_accept(TRANS_STATE::NO_GET)` for a read,
   or `TRANS_STATE::WILL_GET` for a write. On SIO this sends the **ACK** (`'A'`).
   (For writes, ACK is deferred so NetSIO can piggyback the expected write size.)
2. **Optionally receive** the host's payload: `transaction_get(buf, len)`
   (for writes).
3. **Terminate** the transaction exactly once, one of:
   - `transaction_send(data, len, is_error)` — send `COMPLETE` (`'C'`) or
     `ERROR` (`'E'`), then the data frame + checksum.
   - `transaction_success()` — send `COMPLETE` with no data.
   - `transaction_error()` — send `NAK` (`'N'`) if not yet ACKed (bad command),
     or `ERROR` (`'E'`) if failing mid-processing.

Under the hood these map to the classic SIO handshake bytes via the private
helpers `_sio_nak()` / `_sio_ack()` / `_sio_complete()` / `_sio_error()`
(`sio.cpp:43`, `:54`, `:83`, `:91`); the `transaction_*` wrappers that call them
are at `sio.cpp:104–182`. Failing to send a COMPLETE *or* ERROR is what causes
the host to report SIO **TIMEOUT (138)**.

The state machine is asserted at every step (`_transaction_state`), so a device
that forgets to accept or terminate a transaction fails loudly in debug builds.

### 2.5 Command framing: fixed vs SLIP (this is cross-bus)

The 5-byte `cmdFrame_t` above is the **fixed-frame** style. It exists because the
real Atari SIO hardware protocol *requires* it: a peripheral can't announce a
length up front, so the frame is a rigid struct (device, command, aux1, aux2,
checksum). Most of the classic buses inherit that shape and wrap it in a
`FujiSIOPacket`-style object.

The newer buses use a **variable-length, SLIP-framed, self-describing packet**
instead. The reference implementation is `FujiBusPacket`
(`lib/bus/rs232/FujiBusPacket.{h,cpp}`), the native format of the RS232 / FujiBus
bus (`BUILD_RS232`, the "FEP-004" protocol — PR #1199). It is a genuinely
different design:

- **SLIP framing** (RFC 1055): the packet is delimited by `SLIP_END` (`0xC0`)
  bytes, with `0xDB` as an escape (`0xDB 0xDC` → literal `0xC0`, `0xDB 0xDD` →
  literal `0xDB`), so no payload byte can be mistaken for a frame boundary.
- **A self-describing header** (`fujibus_header`, 6 bytes): `device`, `command`,
  a `uint16 length` (total packet size — so the receiver knows how much to read),
  `checksum` (over the whole decoded packet), and a `descr` descriptor byte.
- **Typed, counted parameters**: descriptor bytes encode how many parameters
  follow and their widths (1/2/4 bytes), chaining more descriptors while bit 7 is
  set — replacing SIO's two fixed `aux` bytes.
- **An arbitrary-length payload**: whatever remains after the params.

Where it comes into play, contrasted with the SIO path:

| | Fixed frame (SIO) | SLIP frame (FujiBus) |
|---|---|---|
| Packet class | `FujiSIOPacket` (wraps `cmdFrame_t`) | `FujiBusPacket` (+ `FujiDWPacket`, …) |
| Read loop | `_sio_process_cmd()` reads `sizeof(cmdFrame_t)` bytes | `_rs232_process_cmd()` → `readBusPacket()` reads until **two** `SLIP_END` markers, then `FujiBusPacket::fromSerialized()` |
| Write | `transaction_send()` writes data + 1-byte checksum | `writeBusPacket()` → `packet.serialize()` (re-SLIP-encoded) |
| Device entry point | `virtualDevice::sio_process()` | `virtualDevice::rs232_process()` |
| Length known up front? | No (fixed size) | Yes (`length` field) |

Everything *above* the framing is identical: the SLIP buses still derive from
`SystemBusBase`, still look devices up through the `DaisyChain`, and still finish
work through the same `transaction_accept/get/send/success/error` contract. Only
the bytes on the wire and the per-command entry point differ.

Two unrelated SLIP users exist for completeness and should not be confused with
command framing: the IWM/Apple network connector (`lib/bus/iwm/iwm_slip.*`,
SmartPort tunneled over TCP) and the device relay (`lib/devrelay/slip/SLIP.*`).
Those are transport/relay framing, not the host command frame.

### 2.6 Is the firmware moving from fixed frames to SLIP?

There is a clear architectural drift in that direction, though it is not a
blanket migration and there is no announced deprecation of fixed frames. The
evidence in-tree:

- **A whole family of self-describing `Fuji*Packet` classes** now parallels
  `FujiSIOPacket`: `FujiBusPacket` (RS232), `FujiDWPacket` (DriveWire),
  `FujiAdamPacket` (AdamNet), `FujiLynxPacket` (ComLynx) — all sharing the same
  header + typed-descriptor + `serialize()`/`fromSerialized()` design.
- **SLIP framing specifically is spreading across the serial-style buses.**
  DriveWire (`lib/bus/drivewire/drivewire.cpp`) doesn't just copy the idea — it
  **reuses RS232's `FujiBusPacket`** and the same `SLIP_END`-delimited
  `readBusPacket()`/`writeBusPacket()`.
- **The framing has a formal spec** ("FEP-004", a FujiNet Enhancement Proposal),
  and the RS232 transport-plan doc (`docs/rs232-spi-transport-plan.md`) treats
  "SLIP-framed `FujiBusPacket`s in/out" as the *stable core*, with only the
  physical transport (UART / USB-CDC / SPI / BusOverIP) varying underneath an
  abstract `IOChannel`.

Two honest caveats:

1. **SIO itself cannot move.** Its fixed 5-byte frame is imposed by real Atari
   SIO hardware (no length field is possible), so `FujiSIOPacket` / `cmdFrame_t`
   is here to stay for the Atari target regardless of the trend.
2. **Not every new-style bus uses SLIP.** AdamNet and ComLynx adopt the
   self-describing `Fuji*Packet` abstraction but keep their own native bus
   framing rather than SLIP. So the more accurate summary is *two* convergences:
   (a) a common self-describing packet object across buses, and (b) SLIP as the
   byte-stream framing for the buses whose physical layer is a plain serial
   stream (RS232/FujiBus, DriveWire).

Net: new serial-style buses are being built on SLIP-framed `Fuji*Packet`s rather
than fixed frames, but fixed frames remain for hardware-constrained buses like
SIO.

---

## 3. Base classes

There are two layers of base classes, and they are the key to how one codebase
serves a dozen machines.

### 3.1 `SystemBusBase` — the bus contract

`SystemBusBase` (`lib/bus/bus.h`) is **bus-agnostic**. It owns the `DaisyChain`
and defines the transaction contract as pure-virtual methods that every concrete
bus must implement:

```cpp
class SystemBusBase {
protected:
    transState_t _transaction_state = TRANS_STATE::INVALID;
    DaisyChain   _daisyChain;
public:
    virtual void addDevice(virtualDevice*, fujiDeviceID_t);   // default: DaisyChain
    virtual void transaction_accept(transState_t expectMoreData) = 0;
    virtual void transaction_success() = 0;
    virtual void transaction_error() = 0;
    virtual success_is_true transaction_get(void* data, size_t len) = 0;
    virtual void transaction_send(const void* data, size_t len, bool is_error=false) = 0;
    // + convenience overloads (string / ByteBuffer / int)
    // + text encoding hooks: nativeTextToUnicode / unicodeTextToNative
};
```

`class systemBus : public SystemBusBase` (`sio.h:154`) then adds the SIO-specific
machinery: baud-rate/high-speed handling, the `IOChannel* _port` (switchable
between the real UART `_serial` and `NetSIO` for BusOverIP), the command pin,
cached pointers to well-known devices (`_fujiDev`, `_modemDev`, `_netDev[8]`,
`_cassetteDev`, `_cpmDev`, `_printerDev`), and the `service()` / `setup()` /
`_sio_process_cmd()` loop described above.

### 3.2 `virtualDevice` — the device contract

`virtualDevice` (`sio.h:86`) is the base for every SIO device. Its core is two
pure-virtual methods every device must implement, plus shared state:

```cpp
class virtualDevice {
    friend systemBus;
    friend fujiDevice;
protected:
    bool listen_to_type3_polls = false;
    virtual void sio_status(const FujiSIOPacket &packet) = 0;   // return 4 status bytes
    virtual void sio_process(const FujiSIOPacket &packet) = 0;  // command dispatcher
    virtual void shutdown() {}                                  // optional cleanup
public:
    fujiDeviceID_t id();               // this device's registered ID
    virtual void sio_high_speed();     // '?' -> report HSIO divisor
    bool is_config_device = false;     // holds the CONFIG boot disk?
    bool device_active = true;
    uint8_t status_wait_count = 5;     // SIO boot-priority (defer to a real D1:)
};
```

By convention `sio_process()` is a `switch` on `packet.command()` that fans out to
per-command handlers, and every device supports a **status** command that returns
four bytes (which the Atari puts in `DVSTAT`).

### 3.3 Shared device bases (cross-bus)

Above the per-bus device classes sit **shared, bus-independent device bases** in
`lib/device/` — `disk.h`, `printer.h`, `modem.h`, `network.h`, `cassette.h`.
These use the same `#define`-selects-the-implementation trick as the bus:

```cpp
// lib/device/disk.h
#ifdef BUILD_ATARI
#include "sio/disk.h"
#define DISK_DEVICE sioDisk
#endif
#ifdef BUILD_APPLE
#include "iwm/disk.h"
#define DISK_DEVICE iwmDisk
#endif
// ... one per platform ...
```

So `main.cpp` creates a `DISK_DEVICE`, and on Atari that resolves to `sioDisk`,
on Apple to `iwmDisk`, and so on.

### 3.4 A worked example: `sioDisk`

`sioDisk` (`lib/device/sio/disk.{h,cpp}`) is a textbook device:

```cpp
class sioDisk : public virtualDevice {
    MediaType *_disk = nullptr;                 // the mounted ATR/DSK image
    void sio_read(const FujiSIOPacket&);
    void sio_write(const FujiSIOPacket&);
    void sio_status(const FujiSIOPacket&) override;
    void sio_process(const FujiSIOPacket&) override;
    // ... percom, format, mount/unmount ...
};
```

`sio_process()` (`disk.cpp:323`) dispatches by command:

```cpp
switch (packet.command()) {
case CMD::DISK_READ:
case CMD::DISK_HSIO_READ:   sio_read(packet);   break;
case CMD::DISK_WRITE: /*…*/ sio_write(packet);  break;
case CMD::DISK_STATUS:      sio_status(packet); return;
case CMD::DISK_FORMAT:      sio_format();       return;
// ...
}
```

A read handler follows the transaction contract exactly:

```cpp
SYSTEM_BUS.transaction_accept(TRANS_STATE::NO_GET);   // ACK, no host payload
// ... read the sector from _disk into _disk->_disk_sectorbuff ...
SYSTEM_BUS.transaction_send(_disk->_disk_sectorbuff, readcount, err); // COMPLETE + data
```

A write handler is the mirror image — `transaction_accept(TRANS_STATE::WILL_GET)`,
then `transaction_get(buf, len)` to pull the host's sector, then
`transaction_success()` / `transaction_error()`. The actual bytes come from the
`MediaType` layer (`lib/media`), which reads/writes the disk image on the
`FileSystem` (SD, LittleFS, or TNFS network storage).

---

## 4. Startup: how it all gets wired

`main_setup()` (`src/main.cpp:111`) runs once at boot, in order:

1. Bring up the debug console, flash (`nvs_flash_init`), keys, LED managers.
2. Start storage: `fnSDFAT.start()`.
3. `crypto.setkey(...)` (before config, which may be encrypted), then
   `Config.load()`, then `fnPassword.setup()`.
4. `theFuji->setup()` and `SYSTEM_BUS.addDevice(theFuji, FUJI_DEVICEID::FUJINET)`.
5. A big **per-platform block** (`#ifdef BUILD_ATARI` / `BUILD_APPLE` / …) that
   constructs the machine's device set and calls `addDevice()` for each, then
   `SYSTEM_BUS.setup()`.

For Atari that block registers the FujiNet, clock, MIDI/UDP stream, PCLink,
printer(s), the R: modem/serial, voice, and CP/M devices. `systemBus::addDevice()`
(`sio.cpp:551`) both caches the well-known devices into typed pointers
(`_fujiDev`, `_modemDev`, `_netDev[]`, …) **and** forwards to
`SystemBusBase::addDevice()`, which records them in the `DaisyChain` for lookup by
ID.

After setup, `fn_service_loop()` starts WiFi/BT, mounts disks, and enters the
service loop.

---

## 5. The available buses

Each host family has its own `systemBus` under `lib/bus/`, selected by `BUILD_*`
in `lib/bus/bus.h`:

| Directory            | `BUILD_*`     | Host family / link                         |
|----------------------|---------------|--------------------------------------------|
| `sio/`               | `BUILD_ATARI` | Atari 8-bit SIO                            |
| `iwm/`               | `BUILD_APPLE` | Apple II (IWM / SmartPort)                 |
| `adamnet/`           | `BUILD_ADAM`  | Coleco ADAM (AdamNet, one-wire)            |
| `iec/`               | `BUILD_IEC`   | Commodore IEC serial                       |
| `drivewire/`         | `BUILD_COCO`  | TRS-80 CoCo (DriveWire)                    |
| `comlynx/`           | `BUILD_LYNX`  | Atari Lynx (ComLynx)                       |
| `rs232/`             | `BUILD_RS232` | Generic RS-232 (7-byte command frame)      |
| `rc2014bus/`         | `BUILD_RC2014`| RC2014 retro bus                          |
| `s100spi/`           | `BUILD_S100`  | S-100 (SPI)                               |
| `cx16_i2c/`          | `BUILD_CX16`  | Commander X16 (I²C)                        |
| `h89/`               | `BUILD_H89`   | Heathkit H89                              |
| `mac/`               | `BUILD_MAC`   | Macintosh (floppy)                        |

The refactored buses derive from `SystemBusBase` and implement the transaction
contract described here — `sio`, `iwm`, `adamnet`, `comlynx`, `rs232`, and
`drivewire`, plus `iec` (which also inherits `IECBusHandler`). Several older buses
(`mac`, `h89`, `cx16_i2c`, `s100spi`, `rc2014bus`) still define a **standalone**
`systemBus` that has not yet been migrated onto `SystemBusBase`; they use the same
name and `SYSTEM_BUS` global but their own device/handshake plumbing. New buses
should derive from `SystemBusBase`. Either way, the higher-level subsystems
(`network-protocol`, `media`, `config`, `http`, `printer-emulator`, `fujiDevice`)
are shared across every platform.

---

## 6. How to add a new bus

Adding a new host bus is mostly about implementing the two contracts
(`SystemBusBase` and `virtualDevice`) and slotting the new class into the
`#ifdef` selectors. A checklist:

### 6.1 Create the bus directory and class

1. `lib/bus/<mybus>/` with `<mybus>.h` / `<mybus>.cpp`.
2. Define your device base:
   ```cpp
   class virtualDevice {
       friend class systemBus;
   protected:
       virtual void sio_status(const FujiXPacket &packet) = 0;   // name to taste
       virtual void sio_process(const FujiXPacket &packet) = 0;
       virtual void shutdown() {}
   public:
       fujiDeviceID_t id();
       bool device_active = true;
   };
   ```
   (The method names are conventionally `sio_*` across buses; keep them or pick
   your own — only the `= 0` overrides in your devices must match.)
3. Define your bus:
   ```cpp
   class systemBus : public SystemBusBase {
   public:
       void setup();
       void service();                 // called every loop pass
       void shutdown();
       void addDevice(virtualDevice*, fujiDeviceID_t) override;

       // implement the transaction contract:
       void transaction_accept(transState_t expectMoreData) override;
       void transaction_success() override;
       void transaction_error() override;
       success_is_true transaction_get(void* data, size_t len) override;
       void transaction_send(const void* data, size_t len, bool is_error=false) override;
   };
   extern systemBus SYSTEM_BUS;
   ```

### 6.2 Implement the protocol

- **`service()`**: detect when the host wants attention (a pin, an incoming
  byte, an I²C address match…), read a command frame, look up the device with
  `_daisyChain.deviceWithFujiID(id)`, and call its `sio_process(packet)`.
- **`transaction_*`**: translate the abstract accept/get/send/success/error steps
  into your wire protocol's ACK/data/status bytes. Use SIO's implementation
  (`lib/bus/sio/sio.cpp:104–182`) as the reference — it shows the exact
  `_transaction_state` assertions to preserve.
- **Command frame**: if your frame differs from the 5-byte default, add a
  `#ifdef BUILD_<MYBUS>` branch to `lib/bus/cmdFrame.h` (see the RS232 example).

### 6.3 Register the bus in the selectors

- `lib/bus/bus.h`: add
  ```cpp
  #ifdef BUILD_MYBUS
  #include "mybus/mybus.h"
  #define FN_BUS_PORT fnUartBUS   // or your port
  #endif
  ```
- `lib/device/disk.h`, `printer.h`, etc.: add `#ifdef BUILD_MYBUS` branches
  pointing `DISK_DEVICE` (and friends) at your `lib/device/mybus/*` classes.

### 6.4 Implement devices

Create `lib/device/mybus/` with at least a disk (`myDisk : public virtualDevice`)
and the FujiNet control device. Model them on `lib/device/sio/disk.cpp`:
a `switch` in `sio_process()` and strict use of the transaction API. Reuse the
shared `MediaType`, `FileSystem`, and `network-protocol` layers — they are
bus-independent.

### 6.5 Wire it into startup and the build

- `src/main.cpp`: add a `#ifdef BUILD_MYBUS` block in `main_setup()` that
  constructs your devices, calls `SYSTEM_BUS.addDevice(...)` for each, and then
  `SYSTEM_BUS.setup()`.
- Add a board entry under `build-platforms/platformio-*.ini` that defines
  `BUILD_MYBUS`, and build with `./build.sh -s <board> -cb`.
- Sanity-check that you did not break other targets with `./build.sh -a`
  (build all platforms).

### 6.6 Conventions to respect

- Log with `Debug_printf` / `Debug_println` (from `debug.h`), never raw `printf`.
- Keep host-vs-PC differences behind `ESP_PLATFORM` and per-computer differences
  behind `BUILD_<PLATFORM>` — **except** in `lib/device/fujiDevice`, where
  per-platform behavior must go through a subclass override, not a `#ifdef`
  (there is a ctest, `check_no_build_ifdefs.py`, that enforces this).
- Formatting is enforced by `coding-standard.py` (clang-format, LLVM-based:
  4-space indent, 95-col limit, Allman braces). Only changed lines are checked.
- Add PC-side unit tests under `tests/` (doctest) where it makes sense — tests
  only run on the FujiNet-PC build (`cd build && ctest --output-on-failure`).

---

## 7. Quick reference — the request lifecycle (SIO)

```
host asserts CMD line
  └─ systemBus::service()                         lib/bus/sio/sio.cpp:362
       └─ _sio_process_cmd()                       sio.cpp:195
            ├─ _port->read(&frame, sizeof frame)   read 5-byte cmd frame
            ├─ sio_checksum(...) == frame.checksum? bad → toggle baud, retry
            ├─ dev = _daisyChain.deviceWithFujiID(frame.device())
            └─ dev->sio_process(packet)            e.g. sioDisk  disk.cpp:323
                 ├─ transaction_accept(NO_GET|WILL_GET)   → ACK 'A'
                 ├─ [transaction_get(buf,len)]            → pull write payload
                 └─ transaction_send(data,len,err)        → COMPLETE 'C' + data
                    or transaction_success()/transaction_error() → 'C' / 'E'/'N'
```

Key files:

- `src/main.cpp` — global `SYSTEM_BUS`, `main_setup()`, `fn_service_loop()`.
- `lib/bus/bus.h` — `SystemBusBase` contract + per-`BUILD_*` bus selection.
- `lib/bus/DaisyChain.h` — device registry / lookup / rotation.
- `lib/bus/cmdFrame.h` — the command-frame struct.
- `lib/bus/sio/sio.{h,cpp}` — the SIO `systemBus` and `virtualDevice`.
- `lib/bus/sio/FujiSIOPacket.*` — command-frame wrapper.
- `lib/device/disk.h` (+ `printer.h`, `modem.h`, `network.h`, `cassette.h`) —
  cross-bus device-base selectors.
- `lib/device/sio/disk.{h,cpp}` — the `sioDisk` example device.
- `lib/device/fujiDevice/` — `fujiDevice` base (+ command mixins); per-bus
  subclass e.g. `lib/device/sio/sioFuji.*` provides `theFuji`.

---

## 8. The `lib/` directory map

Almost all of the firmware's C++ lives under `lib/` (PlatformIO compiles each
subdirectory into a static library and links them together). The subsystems below
are grouped by role; the three already covered in depth above — `bus/`,
`device/`, `fuji/` + `fujiDevice` — are the core of the bus/device model.

### Core runtime (the bus/device model)

| Directory | Role |
|---|---|
| `bus/` | One `systemBus` per host family (`sio/`, `iwm/`, `adamnet/`, `iec/`, `drivewire/`, `comlynx/`, `rs232/`, `rc2014*`, `s100spi/`, `cx16_i2c/`, `h89/`, `mac/`), plus the shared `SystemBusBase` / `DaisyChain` / `cmdFrame.h` and the `Fuji*Packet` framing classes. See §2–§6. |
| `device/` | Device implementations, mirroring the same per-bus subdirs (`device/sio/…`) plus the cross-bus base selectors (`disk.h`, `printer.h`, `modem.h`, `network.h`, `cassette.h`) and `device/fujiDevice/` (the `theFuji` base + command mixins). |
| `fuji/` | Support classes owned by the FUJINET device: `fujiHost` (a mount source — SD or a TNFS/HTTP host slot) and `fujiDisk` (a mounted disk-image slot). Distinct from `device/fujiDevice/`, which is the device itself. |

### Storage, media, and networking

| Directory | Role |
|---|---|
| `media/` | Disk-**image** format decoders (ATR/ATX/XEX, WOZ/DSK/PO/DO, D64/D71/D81, MOOF/DCD, …). See §8.1. |
| `FileSystem/` | Virtual filesystem abstraction over the backends: SD card, on-chip LittleFS, TNFS, and (PC) the host OS filesystem. |
| `TNFSlib/` | Client for the **TNFS** network filesystem protocol — network-mounted storage. |
| `network-protocol/` | The protocol backends behind the **N:** network device — a large set: TCP, UDP, HTTP, TNFS, FTP, SFTP, SSH, SMB, NFS, S3, WebSocket (WS/WSS), Telnet, FS/SD, plus cloud/mail integrations (GDRIVE, GMAIL, GCAL, ONEDRIVE, ICAL/IMAPS/Calendar) and CLIPBOARD/CPM. Built via `NetworkProtocolFactory` over a common `Protocol` base. |
| `tcpip/`, `fn_esp_http_client/` | `tcpip/` is a **PC-side** BSD-sockets networking layer (TcpClient/TcpClientSecure/TcpServer/UDP/DNS, ported from ESP-Arduino's `WiFiClient`, guarded `#ifndef ESP_PLATFORM`); `fn_esp_http_client/` is Espressif's vendored `esp_http_client`. |
| `ftp/`, `webdav/`, `telnet/`, `meatloaf/` | `ftp/` — an FTP **client** (`fnFTP`). `webdav/` — **only** a WebDAV directory-listing XML parser (wraps expat), not general WebDAV support. `telnet/` — vendored **libtelnet** (used by the modem devices and the Telnet protocol). `meatloaf/` — the **Meatloaf** Commodore filesystem: a full VFS (container/disk/file/network/tape/service backends), used by the IEC drive. |

### Host-facing services and features

| Directory | Role |
|---|---|
| `http/` | The web-UI service — class `fnHttpService`, global `fnHTTPD` (a mongoose-based `mgHttpService` variant exists for PC) — serving the config UI and the `/files` endpoints host tools use; also holds the outbound HTTP client (`fnHttpClient`). |
| `config/` | The `Config` object and `fnconfig.ini` load/store. |
| `console/` | The serial/USB console command interface (includes `improv` Wi-Fi provisioning; vendors `cxxopts`). |
| `printer-emulator/` | Emulates many real printers (Atari 82x/102x, Epson, Okimate, Commodore MPS803, Coleco) for the P: device, rendering to PDF/PNG/SVG/HTML/text/raw. |
| `modem-sniffer/`, `sam/` | Logs the modem character stream to a dump file; **SAM** (Software Automatic Mouth, vendored) speech synthesis for the Atari voice device. |
| `runcpm/` | **RunCPM** — a vendored CP/M 2.2 machine emulator (Z80 + BDOS/CCP) behind the CP/M device, with FujiNet abstraction shims. |
| `devrelay/` | **SmartPort-over-SLIP** relay: forwards IWM/SmartPort commands to an external host (e.g. AppleWin) over TCP or COM. Its own SLIP framing lives in `devrelay/slip/`. Guarded by `DEV_RELAY_SLIP`. |
| `qrcode/`, `clipboard/`, `display/` | QR-code generation (the Fuji `QRMixin`); a clipboard **manager** (current entry + bounded history, shared between the host's CLIPBOARD: protocol and the web UI); and — despite the name — a WS2812B addressable-LED-strip driver (SPI/DMA), **not** a screen/GUI layer. |

### Support / cross-cutting / third-party

| Directory | Role |
|---|---|
| `hardware/` | Hardware abstraction: UART/LED/GPIO managers, `SystemManager`, and the `IOChannel` transports (`UARTChannel`/`ESP32UARTChannel`, `ACMChannel`/USB-CDC, `BoIPChannel`, plus `COMChannel`/`TTYChannel`) that the buses read/write through. |
| `encrypt/`, `encoding/`, `libb64/` | `encrypt/` — a single global `crypto` object; note it is **not** real cryptography, just a trivial reversible printable-char cipher (lifted from micro-emacs). `encoding/` — base64 + hashing (MD5/SHA-1/SHA-256/…). `libb64/` — a vendored base64 codec. |
| `fnjson/`, `fnsgml/`, `tinyxml2/` | `fnjson/` — a cJSON wrapper with FujiNet query flags (the JSON reader for the N: device); `fnsgml/` — an HTML reader wrapping **Gumbo** + gumbo-query (CSS selectors), mirroring the fnjson interface; `tinyxml2/` — vendored TinyXML-2. |
| `task/`, `utils/`, `compat/` | `task/` — a **PC-only, cooperative** async task manager (`service()` polled from the main loop, not FreeRTOS); `utils/` — general utilities; `compat/` — desktop compatibility shims (`strlcpy`, `dirent`, `uname`, `termios`). |

> Vendored / third-party code (compiled but not FujiNet-authored) includes
> `libb64`, `tinyxml2`, `sam`, `runcpm`, `fn_esp_http_client`, `libtelnet`
> (`telnet/`), Gumbo/gumbo-query (`fnsgml/`), cJSON (`fnjson/`), expat (`webdav/`),
> parts of `meatloaf/`, and the `cxxopts` / `improv` helpers under `console/`.

### 8.1 `lib/media` — disk-image formats

`lib/device/*/disk.*` (e.g. `sioDisk`) handles the *bus protocol* for a drive;
`lib/media` handles the *file format* of the image that drive is serving. A
device holds a `MediaType *` and never parses image bytes itself.

`lib/media/media.h` is a per-`BUILD_*` selector that pulls in the current
platform's media headers, exactly like `device/disk.h`. **There is no single,
shared `MediaType` base class.** Instead each platform subdir declares its *own*
class literally named `MediaType` — `apple/mediaType.h`, `adam/mediaType.h`,
`cbm/mediaType.h`, `mac/mediaType.h`, `drivewire/mediaType.h`,
`atari/diskType.h`, and so on (Atari's copy lives in the historically-named
`diskType.h`). `media.h` compiles in exactly one of them, so within a firmware
image there is one `MediaType` — but it is that platform's copy, selected by the
same compile-time name-shadowing used for `systemBus` and `virtualDevice`, *not*
a common base shared through inheritance. Each subdir then adds one subclass per
image format that derives from its own local `MediaType`:

| Platform subdir | Formats (subclasses) |
|---|---|
| `atari/` | ATR, ATX, XEX (bootable executable) |
| `apple/` | WOZ, DSK, DO, PO |
| `cbm/` | D64, D71, D80, D81, D82, D8B, T64, TCRT |
| `mac/` | MOOF, DCD |
| `adam/` | DDP, DSK, ROM |
| `drivewire/` (CoCo) | DSK, VDK, MRM, ROM |
| `rs232/`, `h89/`, `rc2014/` | DSK / IMG (+ ROM) |
| `lynx/`, `cx16/`, `s100spi/` | ROM / platform image |

These per-platform `MediaType` classes share a *convention*, not an enforced
contract — because nothing binds them to a common abstract base, the interfaces
have drifted between platforms (Atari and other sector-oriented buses expose the
`read(sectornum, …)` shape below; block/track-based platforms differ). The Atari
copy (`atari/diskType.h:46`) is representative:

```cpp
class MediaType {   // atari/diskType.h — each platform has its own copy of this name
public:
    virtual mediatype_t mount(fnFile *f, uint32_t disksize) = 0;  // sniff + attach
    virtual void        unmount();
    virtual error_is_true format(uint16_t *responsesize);
    virtual error_is_true read(uint16_t sectornum, uint16_t *readcount) = 0;
    virtual error_is_true write(uint16_t sectornum, bool verify);
    virtual uint16_t    sector_size(uint16_t sectornum);
    virtual void        status(uint8_t statusbuff[4]) = 0;
    static  mediatype_t discover_mediatype(const char *filename);  // pick by extension
    uint8_t     _disk_sectorbuff[DISK_SECTORBUF_SIZE];  // the shared I/O buffer
    mediatype_t _disktype = MEDIATYPE_UNKNOWN;
};
```

So a sector read is: `sioDisk::sio_read()` → `_disk->read(sector, &count)` (fills
`_disk->_disk_sectorbuff`) → `SYSTEM_BUS.transaction_send(_disk->_disk_sectorbuff,
count, err)`. The disk device knows the bus; the `MediaType` knows the format; the
`FileSystem` knows where the bytes physically live.

## 9. The `pico/` directory — companion microcontroller firmware

Everything above this section is one program: the ESP32 firmware (or its
FujiNet-PC native twin). `pico/` is **not** part of that program. It holds
separate firmware projects that run on a **Raspberry Pi Pico (RP2040/RP2350)**,
a different microcontroller with a different toolchain — the
[pico-sdk](https://github.com/raspberrypi/pico-sdk) (CMake + Ninja, or in one
case PlatformIO), **not** PlatformIO/ESP-IDF and not the CMake PC build. `build.sh`
never touches it; each subdirectory builds on its own into a `.uf2` you flash to
a Pico in BOOTSEL mode.

**Why a second MCU exists.** Some host computers talk to their peripherals over a
*cartridge* or *disk* bus whose timing is far too tight to bit-bang from the
ESP32 — you have to respond to a bus address within a handful of nanoseconds. The
RP2040/RP2350 has **PIO** (Programmable I/O) state machines: tiny deterministic
coprocessors that can emulate a ROM cartridge or a floppy interface in hardware
while the CPU does something else. So the Pico sits *between* the vintage machine's
bus and the FujiNet ESP32: it handles the hard-real-time bus emulation, and
forwards the actual peripheral requests to the ESP32 over a serial/USB link —
frequently speaking the very **FujiBus/SLIP** framing described in §2.5. In other
words, the Pico is a front-end bus adapter; the ESP32 remains the FujiNet.

```
vintage machine  --(cartridge/disk bus, PIO-emulated)-->  RP2040/RP2350  --(UART or USB-CDC, FujiBus/SLIP)-->  ESP32 fujinet-firmware
```

The four subdirectories are independent projects at different stages of maturity:

| Subdir | Host | What the Pico does | Build |
|---|---|---|---|
| `atari-2600/` | Atari 2600 | **PlusCart-Pico** — a port of the [PlusCart](https://github.com/Al-Nafuur/United-Carts-of-Atari) cartridge emulator (SD + on-board flash ROM storage, PlusROM/PlusStore, WiFi via an ESP8266/ESP32). Being merged in with the intent of replacing the old ESP32-AT front-end with calls into fujinet-firmware. Upstream lives at [gtortone/PlusCart-Pico](https://github.com/gtortone/PlusCart-Pico). | PlatformIO (`pico` / `vccgnd_yd_rp2040` boards) |
| `coco/` | TRS-80 Color Computer | Emulates the CoCo cartridge **ROM** in PIO+DMA (`cococart.pio`, `rom.c`) and adds a PIO-based UART (`uart_rx/tx.pio`) to talk to FujiNet. A single `main.c`. | pico-sdk (CMake) |
| `intellivision/` | Intellivision | RP2040/RP2350 bridge (a fork of Gennaro Tortone's **Minty** cartridge firmware) that bridges the CP-1610 bus to an ESP32-S3 running the `fujiversal-rs232` build over **USB-CDC**, via a PEEK/POKE mailbox at `$9C00–$9F3F`, and boots the console straight into FujiNet CONFIG. Includes a plain-C port of `FujiBusPacket` (`src/fujibus.c`) with a desktop unit test. See its `README.md` / `PROVENANCE.md`. | pico-sdk (CMake, `PICO_BOARD=fujicard`) |
| `mac/` | Macintosh (68k) | Emulates the Mac's external-drive interface in PIO — variable-speed floppy and **DCD** (hard disk) — via a stack of `.pio` programs (`commands`, `mux`, `latch`, `dcd_*`, …) driven from `commands.c`. This is the hardware counterpart to the `mac` bus (`macDevice`) referenced in §1 and §5. | pico-sdk (CMake) |

Takeaways for anyone reading the tree for the first time:

- **Different language and runtime.** These are mostly **C** on the pico-sdk, with
  the real work in `.pio` assembly. None of the `lib/` bus/device machinery from
  §1–§8 is compiled here; don't expect `systemBus`, `virtualDevice`, or the
  ESP-IDF APIs.
- **Vendored / forked, with provenance to respect.** `atari-2600/` and
  `intellivision/firmware/` are forks of external GPL projects — read the local
  `README.md` / `LICENSE` / `PROVENANCE.md` before touching them.
- **The bridge protocol is shared by convention, not by code.** Where a Pico
  forwards to the ESP32 it re-implements the FujiBus/SLIP wire format in C
  (`intellivision/firmware/src/fujibus.c` is a hand-port of
  `lib/bus/rs232/FujiBusPacket.cpp`) rather than linking the C++ original — so if
  that framing changes in `lib/bus/`, the Pico copies must be updated to match.
