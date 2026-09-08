# BNET — IP Address Manager

[![release](https://img.shields.io/github/v/release/Flinterpop/BNET)](https://github.com/Flinterpop/BNET/releases)

One dialog that does what the Windows IPv4 property sheets do.

Setting a static address on Windows 10/11 means Settings → Network → Change adapter
options → adapter → Properties → Internet Protocol Version 4 → Properties → Advanced →
Add, and the *additional* addresses are two dialogs deeper than the first one. BNET puts
the whole thing on a single surface: adapter, DHCP-or-static, address, mask, gateway, DNS
and the full list of additional addresses, committed together by one **Apply**.

![the dialog](docs/screenshot.png)

## What it does

- **Picks an adapter** from a drop-down, showing its description, MAC, link state and
  whether it is currently on DHCP or static.
- **Switches between DHCP and static**, the same choice as the radio pair in the Windows
  dialog.
- **Sets IP address, subnet mask, default gateway** and the **preferred/alternate DNS**
  servers.
- **Adds and removes additional IP/mask pairs** on the same adapter — the part that
  normally lives behind the *Advanced* button.
- **Offers named presets** — pick a named network from a list and send it to the primary
  fields or straight into the additional list. See [Presets](#presets).
- **Re-reads the system after every Apply**, so what you see afterwards is what Windows
  actually has, not what was asked for.

Nothing is committed until Apply. Add and Remove only change the pending list.

## How it reads and writes

Reading is the IP Helper API (`GetAdaptersAddresses`) — fast, no COM, no elevation needed.

Writing is WMI's `Win32_NetworkAdapterConfiguration`, because `EnableStatic(ips[], masks[])`
takes **arrays** — which is exactly the additional-addresses case, applied in one call —
and because it persists the same way the control panel does. `SetGateways` and
`SetDNSServerSearchOrder` follow it. The adapter is located by its `SettingID` (the GUID),
**not** by `Index`: WMI's `Index` is not the `IfIndex` the IP Helper API reports, and
assuming they match configures the wrong adapter.

Applying runs on a worker thread and posts its result back, because WMI takes seconds on a
busy adapter and a frozen dialog looks like a crash.

## Behaviour worth knowing

- **It requires administrator** and its manifest asks for it up front, rather than failing
  at Apply with WMI error 91.
- **Under DHCP the fields still show the current values.** They are what the server handed
  out, which is the sane starting point for going static — Windows blanks them instead.
- **169.254.x.x autoconfiguration addresses are hidden.** They are not something you set,
  and offering one back as a static address would be a trap.
- **A gateway off-subnet raises the same warning Windows gives**, for the same reason: it
  is unreachable and silently does nothing. You can override it.
- **The subnet mask is a pick list, defaulting to 255.255.255.0.** There are only 32 legal
  IPv4 masks — a run of ones then a run of zeroes — so both mask fields offer all of them,
  each labelled with its prefix length (`255.255.255.0   /24`). That makes an invalid mask
  unrepresentable rather than merely rejected, which is why nothing validates one. The list
  runs longest prefix first, so the small subnets people actually pick are at the top of it
  rather than 24 rows down. An adapter's existing mask selects itself when you pick the
  adapter.
- **Double-click a row** in the additional list to lift it back into the entry fields for
  editing; press Add to put it back. That is the closest thing to editing in place without
  opening a second dialog.
- **A blank gateway clears the gateway.** Blank DNS fields clear the static DNS list, which
  hands DNS back to DHCP.
- **Windows does not allow additional static addresses while DHCP is on**, and neither does
  this — the list is disabled under DHCP, the same as the *Advanced* dialog.
- IPv6 is not touched. This is an IPv4 tool.

## Presets

The Preset list holds named networks, so a machine that moves between known ones is a pick,
a button and an Apply. A preset serves the primary address and the additional list alike:

- **Use as primary** puts its address, mask and gateway in the main fields, switching to
  static if DHCP was selected.
- **Add to list** appends it to the additional addresses instead, leaving the primary alone.

Choosing a preset does nothing by itself — the destination is a button, not a side effect,
so picking one can never silently overwrite an address you already entered. Both buttons
stay disabled until a real preset is selected, and **Add to list** refuses an address that
is already the primary or already in the list.

**The presets are not compiled in.** A preset names a real network, and this repository is
public — addresses, hostnames and MACs do not belong in a binary's defaults, which is the
same rule that genericised DHCPDealer's. So the list is a file you own:

```
BNET.presets.txt        beside BNET.exe -- yours, and gitignored
presets.example.txt     in this repository -- documentation addresses only
```

One preset per line, the `=` and the gateway both optional:

```
Lab bench     = 192.168.50.10/24
Lab bench alt = 192.168.50.11/24, 192.168.50.1
Test rig      = 10.0.2.15/255.255.255.0
```

The mask may be a prefix length or dotted. A malformed line is skipped and counted in the
status line rather than rejecting the whole file. **Refresh** reloads presets as well as
re-reading the adapters, so editing the file needs no restart. With no file present the
list reads `(no presets file)` and the hint names the path to create.

Either way it is only the dialog being filled in — Apply remains the one thing that touches
the adapter.

## If an address doesn't appear in ipconfig

**There is no activation step.** `EnableStatic` writes the address and the running stack
takes it immediately — nothing to enable, no restart, no adapter bounce. So an address that
Apply accepted but `ipconfig` does not list is not waiting to be activated; it is not there,
or it is there and unusable. BNET checks for both after every Apply and says which:

- **"address not usable"** — the address is on the adapter but duplicate address detection
  rejected it, almost always because another host on the segment already has it. The
  additional-address list shows `DUPLICATE` in its State column, and `ipconfig` will not
  present it as usable. Pick a free address, or find the host holding it.
- **"not all addresses took"** — the stack does not have it at all. Either a restart is
  pending and the change is in the registry only, or something reverted it: group policy, a
  VPN client, NIC teaming, or a Hyper-V switch that owns the adapter.

`netsh interface ipv4 show addresses` is the authority, and will agree with what BNET says.

The State column is blank for a healthy address; it only ever shows a problem.

## Building

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Plain Win32, C++20, no external dependencies. Builds `/W4 /WX` clean. The release exe links
the static CRT, so it needs no VC++ redistributable:

```
dumpbin /dependents build\Release\BNET.exe
```

shows only `IPHLPAPI`, `WS2_32`, `COMCTL32`, `ole32`, `OLEAUT32`, `KERNEL32` and `USER32`.

## Layout

```
src/NetConfig.h      adapter/apply model, no UI
src/NetConfig.cpp    IP Helper enumeration + the WMI apply
src/Presets.h/.cpp   the named IP/mask file, deliberately not compiled in
src/main.cpp         the dialog
res/BNET.rc          dialog template, version info, manifest reference
res/BNET.manifest    requireAdministrator, common controls v6, per-monitor DPI
presets.example.txt  the preset file format, with documentation addresses
```
