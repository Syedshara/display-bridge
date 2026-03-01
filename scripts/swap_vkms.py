#!/usr/bin/env python3
"""
swap_vkms.py - Replace the running vkms module with the patched version.

Strategy:
1. Write patched vkms.ko to the kernel module path (via sudo tee)
2. Restart GNOME Shell (which briefly releases DRM) while simultaneously
   running modprobe -r && modprobe in the background
3. Verify 2880x1800 appears in Mutter after restart

Must be run with NOPASSWD sudo for /sbin/modprobe and /usr/bin/tee.

Usage: python3 scripts/swap_vkms.py
"""
import dbus
import subprocess
import sys
import time
import os
import threading

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCHED_KO = os.path.join(REPO_ROOT, "ubuntu", "vkms-patched", "vkms.ko")


def get_virtual_modes():
    try:
        bus = dbus.SessionBus()
        p = bus.get_object('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig')
        i = dbus.Interface(p, 'org.gnome.Mutter.DisplayConfig')
        _, monitors, _, _ = i.GetCurrentState()
        for m in monitors:
            if 'Virtual' in m[0][0]:
                return [str(x[0]) for x in m[1]]
        return []
    except Exception as e:
        return []


def write_patched_module():
    """Write patched .ko over the existing module (or alongside it)."""
    release = os.uname().release
    mod_dir = f"/lib/modules/{release}/kernel/drivers/gpu/drm/vkms/"
    dest_ko = mod_dir + "vkms.ko"

    print(f"  Writing {PATCHED_KO} -> {dest_ko} via sudo tee...")
    with open(PATCHED_KO, 'rb') as f:
        result = subprocess.run(['sudo', '/usr/bin/tee', dest_ko],
                               stdin=f, capture_output=True)
    if result.returncode != 0:
        print(f"  tee failed (exit {result.returncode}): {result.stderr[:200]}")
        return False

    print("  Module written OK")
    return True


def reload_vkms_loop():
    """Background thread: keep trying modprobe -r vkms && modprobe vkms.
    systemctl restart blocks until shell is back up -- the DRM-free window
    is DURING the restart, so this thread hammers modprobe aggressively."""
    # Brief delay to let systemctl send SIGTERM before we start
    time.sleep(0.2)
    for attempt in range(60):
        r = subprocess.run(['sudo', '/sbin/modprobe', '-r', 'vkms'],
                          capture_output=True, text=True)
        if r.returncode == 0:
            print(f"\n  [bg] vkms unloaded on attempt {attempt+1}")
            time.sleep(0.1)
            r2 = subprocess.run(['sudo', '/sbin/modprobe', 'vkms'],
                               capture_output=True, text=True)
            if r2.returncode == 0:
                print("  [bg] patched vkms loaded!")
            else:
                print(f"  [bg] modprobe vkms failed: {r2.stderr.strip()}")
            return
        time.sleep(0.1)
    print("  [bg] Could not unload vkms in 6s")


def restart_gnome_shell_async():
    """Fire systemctl restart in a thread (it blocks until shell is back up)."""
    def _do():
        result = subprocess.run(
            ['systemctl', '--user', 'restart', 'org.gnome.Shell@wayland.service'],
            capture_output=True, text=True, timeout=30
        )
        print(f"\n  [shell] systemctl restart done: exit={result.returncode} "
              f"err={result.stderr.strip()[:100]}")
    t = threading.Thread(target=_do, daemon=True)
    t.start()
    return t


def main():
    if not os.path.exists(PATCHED_KO):
        print(f"ERROR: Patched module not found: {PATCHED_KO}")
        print("Build it first: make -C ubuntu/vkms-patched")
        sys.exit(1)

    print(f"Patched module: {PATCHED_KO}")
    print(f"Kernel: {os.uname().release}\n")

    # Check current Virtual-1 modes
    modes = get_virtual_modes()
    already_has = any('2880' in m for m in modes)
    print(f"Current Virtual-1 modes: {len(modes)} total")
    print(f"2880x1800 already present: {already_has}")
    if already_has:
        print("Nothing to do!")
        sys.exit(0)

    # Step 1: Write patched module to disk
    print("\n[1] Writing patched vkms.ko to module path...")
    if not write_patched_module():
        sys.exit(1)

    # Step 2: Start modprobe background thread FIRST (it will hammer modprobe -r)
    print("\n[2] Starting background vkms reload thread...")
    modprobe_t = threading.Thread(target=reload_vkms_loop, daemon=True)
    modprobe_t.start()

    # Step 3: Fire gnome-shell restart asynchronously (releases DRM during restart)
    print("\n[3] Triggering GNOME Shell restart (async) to release DRM device...")
    shell_t = restart_gnome_shell_async()

    # Step 4: Wait for both to finish
    print("\n[4] Waiting for modprobe + GNOME Shell restart to complete...")
    modprobe_t.join(timeout=15)
    shell_t.join(timeout=30)
    # Give Mutter a moment to re-initialize displays
    time.sleep(3)

    # Step 5: Wait for Mutter to re-detect displays
    print("\n[5] Checking for 2880x1800 in Mutter mode list...")
    for attempt in range(15):
        time.sleep(2)
        modes = get_virtual_modes()
        modes_2880 = [m for m in modes if '2880' in m]
        print(f"  [{attempt+1}/15] Virtual-1 modes: {len(modes)}, "
              f"2880x? = {modes_2880}")
        if modes_2880:
            print("\nSUCCESS: 2880x1800 mode is now available in Mutter!")
            print("Next: python3 scripts/apply_monitors.py")
            return

    print("\nWARNING: 2880x1800 not found after 30s.")
    print("Check: journalctl -k --since '1 min ago' | grep -i vkms")
    print("Check: lsmod | grep vkms")
    sys.exit(1)


if __name__ == '__main__':
    main()
