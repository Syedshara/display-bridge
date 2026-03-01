#!/usr/bin/env python3
"""
scripts/apply_monitors.py
Configure an extended display using Mutter's DisplayConfig D-Bus API.

Sets up:
  eDP-1   1920x1200@60  at (0, 0)      [primary, physical]
  Virtual 2880x1800@60  at (1920, 0)   [extended, vkms virtual]

Prerequisites:
  1. vkms module loaded:     sudo modprobe vkms
  2. EDID injected:          python3 gen_edid.py |
                             sudo tee /sys/kernel/debug/dri/0/Virtual-1/edid_override
  3. Wait ~2 s for Mutter to process the hotplug event, then run this script.

Run with: python3 scripts/apply_monitors.py
"""

import sys
import dbus

SERVICE = 'org.gnome.Mutter.DisplayConfig'
PATH    = '/org/gnome/Mutter/DisplayConfig'


def find_best_mode(modes, target_w, target_h, min_rr=55.0, max_rr=65.0):
    """Return (mode_id, refresh_rate) for the closest-to-60Hz mode at target res."""
    best_id, best_rr = None, None
    for mode in modes:
        mid = str(mode[0])
        w, h, rr = int(mode[1]), int(mode[2]), float(mode[3])
        if w == target_w and h == target_h and min_rr <= rr <= max_rr:
            if best_id is None or abs(rr - 60.0) < abs(best_rr - 60.0):
                best_id, best_rr = mid, rr
    return best_id, best_rr


def main():
    bus   = dbus.SessionBus()
    proxy = bus.get_object(SERVICE, PATH)
    iface = dbus.Interface(proxy, SERVICE)

    # ------------------------------------------------------------------ #
    # 1.  GetCurrentState                                                  #
    # ------------------------------------------------------------------ #
    serial, monitors, _logical, _props = iface.GetCurrentState()
    print(f"Mutter serial: {serial}")

    edp_connector  = None
    edp_mode_id    = None
    edp_rr         = None
    virt_connector = None
    virt_mode_id   = None
    virt_rr        = None

    for monitor in monitors:
        info, modes, _ = monitor
        connector = str(info[0])
        print(f"\n  Connector: {connector}")
        for mode in modes:
            mid = str(mode[0])
            w, h, rr = int(mode[1]), int(mode[2]), float(mode[3])
            print(f"    {mid:38s}  {w}x{h}@{rr:.3f}")

        if 'eDP' in connector and edp_connector is None:
            edp_connector = connector
            edp_mode_id, edp_rr = find_best_mode(modes, 1920, 1200)

        if 'Virtual' in connector and virt_connector is None:
            virt_connector = connector
            virt_mode_id, virt_rr = find_best_mode(modes, 2880, 1800)

    # ------------------------------------------------------------------ #
    # 2.  Validate                                                         #
    # ------------------------------------------------------------------ #
    if not edp_connector:
        sys.exit("\nERROR: eDP connector not found in Mutter state")
    if not virt_connector:
        sys.exit("\nERROR: Virtual connector not found — is vkms loaded?")
    if not edp_mode_id:
        sys.exit(f"\nERROR: 1920x1200@60 mode not found on {edp_connector}")
    if not virt_mode_id:
        sys.exit(
            f"\nERROR: 2880x1800@60 mode not found on {virt_connector}\n"
            "       Inject EDID first:\n"
            "         python3 gen_edid.py | "
            "sudo tee /sys/kernel/debug/dri/0/Virtual-1/edid_override\n"
            "       Then wait ~2 s and re-run this script."
        )

    print(f"\n  eDP  -> {edp_connector}   mode={edp_mode_id}  ({edp_rr:.3f} Hz)")
    print(f"  Virt -> {virt_connector}  mode={virt_mode_id}  ({virt_rr:.3f} Hz)")
    print("\nApplying persistent extended config (method=2)...")

    # ------------------------------------------------------------------ #
    # 3.  ApplyMonitorsConfig                                              #
    #     signature: u serial, u method,                                   #
    #                a(iiduba(ssa{sv})) logical_monitors, a{sv} props       #
    # ------------------------------------------------------------------ #
    empty = dbus.Dictionary({}, signature='sv')

    def mon(connector_name, mode_id):
        return (dbus.String(connector_name),
                dbus.String(mode_id),
                dbus.Dictionary({}, signature='sv'))

    logical = dbus.Array([
        # Physical eDP-1: primary, position (0, 0)
        dbus.Struct(
            [dbus.Int32(0), dbus.Int32(0), dbus.Double(1.0), dbus.UInt32(0),
             dbus.Boolean(True),
             dbus.Array([mon(edp_connector, edp_mode_id)],
                        signature='(ssa{sv})')],
            signature='iiduba(ssa{sv})'
        ),
        # Virtual display: extended right of eDP-1, position (1920, 0)
        dbus.Struct(
            [dbus.Int32(1920), dbus.Int32(0), dbus.Double(1.0), dbus.UInt32(0),
             dbus.Boolean(False),
             dbus.Array([mon(virt_connector, virt_mode_id)],
                        signature='(ssa{sv})')],
            signature='iiduba(ssa{sv})'
        ),
    ], signature='(iiduba(ssa{sv}))')

    iface.ApplyMonitorsConfig(
        dbus.UInt32(serial),
        dbus.UInt32(2),   # 2 = persistent (survives reboot)
        logical,
        empty
    )

    print("\nSUCCESS: Extended display configured.")
    print(f"  {edp_connector}:  1920x1200 at (0, 0)    [primary]")
    print(f"  {virt_connector}: 2880x1800 at (1920, 0)  [extended]")


if __name__ == '__main__':
    main()
