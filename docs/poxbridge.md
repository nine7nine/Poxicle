# poxicle — The poxbridge Streaming Protocol

poxbridge is the cross-process channel that lets a particle *producer* (a Wayland client running its own sim, e.g. the Chiguiro terminal) stream ready-to-draw instances to a *receiver* that draws them on the producer's window — the [KWin effect](kwin-effect.gen.html) or the [GNOME extension](gnome-extension.gen.html).

## Table of Contents

1. [Why a bridge exists](#1-why-a-bridge-exists)
2. [Transport: one shared region per producer](#2-transport-one-shared-region-per-producer)
3. [The shared-region header](#3-the-shared-region-header)
4. [The seqlock](#4-the-seqlock)
5. [The D-Bus control surface](#5-the-d-bus-control-surface)
6. [Window identity](#6-window-identity)
7. [Idle parking and waking](#7-idle-parking-and-waking)
8. [Design rules](#8-design-rules)

---

## 1. Why a bridge exists

A compositor effect can draw on a client's window, but it **cannot observe the client's internal triggers** — a terminal bell, a process spawn or exit, an overscroll gesture. Those events live inside the application. Porting them into the effect's own simulation is impossible (the effect can't see them) and would throw away the producer's existing, fully-configured sim.

So poxbridge inverts the split: the **client sims and streams** ready-to-draw `PoxInstance[]`, and the **receiver only positions and draws** them. The receiver applies none of its own rules to a streaming window — every tunable, colour and trigger has already been resolved by the producer before the instances exist. This is the path the [Chiguiro handoff](build.gen.html) describes: Chiguiro keeps configuring particles in-app but renders through the compositor instead of an in-process overlay.

The contract is defined entirely by one self-contained header, `include/poxbridge.h`. It is plain C with no dependency on `poxicle.h`, so a producer can copy it verbatim — Chiguiro's `KgxParticleInstance` is byte-identical to `PoxInstance`, so it maps the same body layout without linking poxicle at all.

## 2. Transport: one shared region per producer

Each producer toplevel gets **one POSIX shared region** — a `memfd` the producer creates and hands to the receiver once, over D-Bus. The producer writes each frame's `PoxInstance[]` into the region's body and bumps a counter; the receiver polls that counter every compositor frame and copies the latest complete frame. There is **no per-frame D-Bus traffic** — only `Register` / `Unregister` (once each) and `Wake` (only on an idle→active edge).

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 360" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .prod  { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .recv  { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .shm   { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .box   { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl   { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm{ fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut{ fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-pur{ fill: #bb9af7; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy{ fill: #7dcfff; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel{ fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln    { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .dbus  { stroke: #bb9af7; stroke-width: 1.2; stroke-dasharray: 6,4; fill: none; }
    .title { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="920" height="360" class="bg"/>
  <text x="460" y="26" text-anchor="middle" class="title">producer streams instances; receiver draws them</text>

  <rect x="20" y="60" width="250" height="130" class="prod"/>
  <text x="40" y="82" class="lbl-pur">producer (e.g. Chiguiro)</text>
  <text x="40" y="100" class="lbl-mut">runs its full sim + triggers</text>
  <rect x="40" y="112" width="210" height="30" class="box"/>
  <text x="145" y="131" text-anchor="middle" class="lbl-sm">write PoxInstance[] each frame</text>
  <rect x="40" y="150" width="210" height="30" class="box"/>
  <text x="145" y="169" text-anchor="middle" class="lbl-sm">bump seqlock (odd -> even)</text>

  <rect x="335" y="60" width="250" height="180" class="shm"/>
  <text x="355" y="82" class="lbl-yel">memfd shared region</text>
  <rect x="355" y="92" width="210" height="58" class="box"/>
  <text x="460" y="112" text-anchor="middle" class="lbl-sm">PoxBridgeHeader (32 B)</text>
  <text x="460" y="128" text-anchor="middle" class="lbl-mut">magic, version, seq, count,</text>
  <text x="460" y="141" text-anchor="middle" class="lbl-mut">capacity, inst_size, w, h</text>
  <rect x="355" y="158" width="210" height="68" class="box"/>
  <text x="460" y="182" text-anchor="middle" class="lbl-sm">body: PoxInstance[capacity]</text>
  <text x="460" y="200" text-anchor="middle" class="lbl-mut">~36 B each, 4-byte aligned</text>
  <text x="460" y="214" text-anchor="middle" class="lbl-mut">offset = 32 + i * inst_size</text>

  <rect x="650" y="60" width="250" height="130" class="recv"/>
  <text x="670" y="82" class="lbl-cy">receiver (KWin / GNOME)</text>
  <text x="670" y="100" class="lbl-mut">polls seq every frame</text>
  <rect x="670" y="112" width="210" height="30" class="box"/>
  <text x="775" y="131" text-anchor="middle" class="lbl-sm">copy latest complete frame</text>
  <rect x="670" y="150" width="210" height="30" class="box"/>
  <text x="775" y="169" text-anchor="middle" class="lbl-sm">offset by window rect, draw</text>

  <line x1="270" y1="127" x2="335" y2="127" class="ln"/>
  <line x1="585" y1="127" x2="650" y2="127" class="ln"/>

  <line x1="145" y1="280" x2="775" y2="280" class="dbus"/>
  <text x="460" y="274" text-anchor="middle" class="lbl-pur">D-Bus org.ninez.PoxicleBridge — Register / Unregister / Wake (control only)</text>
  <line x1="145" y1="190" x2="145" y2="280" class="dbus"/>
  <line x1="775" y1="190" x2="775" y2="280" class="dbus"/>
  <text x="40" y="320" class="lbl-mut">Register hands over the memfd once; the per-frame path is shared memory only</text>
</svg>
</div>

## 3. The shared-region header

The region begins with a fixed 32-byte header at offset 0, followed immediately by the instance body. Every field is 32-bit so the layout is identical on both sides regardless of struct-packing rules.

| Field (`uint32`) | Meaning |
| --- | --- |
| `magic` | `POX_BRIDGE_MAGIC` = `0x31584F50` (`'POX1'`, little-endian) — rejects a stale, wrong, or zero-filled fd. |
| `version` | `POX_BRIDGE_VERSION` = `1`. |
| `seq` | The seqlock (see below) — written **only** by the producer. |
| `count` | Live `PoxInstance` count in the body this frame. |
| `capacity` | Instance slots the body can hold. |
| `inst_size` | `sizeof(PoxInstance)` — sanity-checked against the reader's. |
| `width` / `height` | Producer surface size, logical pixels. |

The header is 32 bytes (8 × `uint32`, 4-aligned), so the float-based, 4-byte-aligned `PoxInstance` body starts correctly aligned right after it. Two inline helpers compute layout: `pox_bridge_map_size(capacity, inst_size)` gives the total region size, and `pox_bridge_body_offset(i, inst_size)` gives instance `i`'s byte offset. A receiver validates `magic`, `version`, `inst_size == sizeof(PoxInstance)`, and that the mapped region is at least `pox_bridge_map_size(...)` before trusting a single byte.

## 4. The seqlock

Frames are published with a **seqlock** — a single producer-written counter that lets a lock-free reader detect torn or mid-write frames without ever blocking the producer. The rule on `seq`:

- **even and unchanged** since the last read → no new frame.
- **odd** → the producer is mid-write; skip this poll.
- **even and advanced** → a new complete frame is in the body.

The producer bumps `seq` to odd, writes `count` and the body, then bumps it to even with release ordering. The reader loads `seq` with acquire ordering, copies the body if it is even and advanced, then **re-reads `seq` and retries** if it moved during the copy — so a torn frame is never drawn. A handful of retries is plenty, since the producer writes at most once per frame. Both receivers implement exactly this loop: the KWin effect's `readStreamFrame` retries up to 4 times, and the binding's `read_stream_vertices` up to 8, each using `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.

## 5. The D-Bus control surface

Control is a small D-Bus interface on the **session bus** — reachable from a normal Wayland client, since KWin effects and gnome-shell share the user session bus. The receiver owns the name; producers call its methods.

| Constant | Value |
| --- | --- |
| `POX_BRIDGE_SERVICE` / `POX_BRIDGE_IFACE` | `org.ninez.PoxicleBridge` |
| `POX_BRIDGE_PATH` | `/PoxicleBridge` |

| Method | Role |
| --- | --- |
| `Register(pid, shm)` | Hands the `memfd` (a Unix fd) to the receiver, which `dup`s it, `fstat`s the true size, maps it read-only, and validates the header. Replaces any prior stream for that pid. |
| `Unregister(pid)` | Drops the stream — unmap, close, and repaint the band so the last particles composite away. |
| `Wake(pid)` | Un-parks the receiver's repaint loop on an idle→active edge (see below). |

Both receivers reject a stream whose header fails validation, and both bind it to a window only after registration. Because the name is owned best-effort, a second effect instance simply finds external mode unavailable while its other paths keep working. (On the GNOME side, `Register` must be handled asynchronously to reach the message's Unix fd list — the plain method path can't carry file descriptors.)

## 6. Window identity

The hard part is matching a registered stream to the right on-screen window: a Wayland client can't obtain a compositor window handle, and the receiver can't obtain the client's `wl_surface` id. poxbridge uses a pragmatic match — **the producer's PID**. The producer registers with its `pid` (and writes its current window size into the header); the receiver binds the stream to the window whose process id matches.

Registration can race window creation, so an unbound stream is re-resolved when new windows appear and when geometry changes. Single-window producers match on pid alone today; multi-window disambiguation by geometry is the documented next step. Because the producer can only send **window-local** coordinates (a Wayland client cannot read its absolute screen position), the receiver offsets every instance by the bound window's frame rectangle — exactly what it already does for its own sim.

## 7. Idle parking and waking

The bridge preserves poxicle's idle-parking discipline across the process boundary. The receiver polls only the seqlock each frame; after a few consecutive empty frames (the KWin effect uses a grace of 4) it clears the stream's instances and stops requesting repaints, letting the compositor sleep. When the producer goes from idle to active it fires `Wake(pid)`, which resets the idle counter and requests a repaint so the receiver un-parks. This mirrors the in-process `pox_wl_wake()` of the [Wayland backend](wayland-backend.gen.html): the same "park when quiet, wake on new work" contract, just signalled over D-Bus instead of a function call.

## 8. Design rules

- **The producer sims, the receiver draws.** Triggers internal to an application (a bell, a process exit) can only be seen by that application, so it streams resolved instances rather than asking the compositor to reproduce its sim.
- **Shared memory for frames, D-Bus for control.** The per-frame path is a `memfd` and a seqlock; D-Bus carries only `Register` / `Unregister` / `Wake`. No per-frame IPC.
- **A self-contained, copyable header.** `poxbridge.h` depends on nothing, so a producer with a byte-identical instance layout joins without linking poxicle.
- **Never draw a torn frame.** The seqlock's odd/even discipline plus a re-read-and-retry reader guarantee the receiver only ever copies a complete frame.
- **Park across the boundary.** Empty-frame grace plus a `Wake` edge keep the compositor asleep when the stream is quiet — the same idle contract as the in-process backend.
