# Rodin bypass charging

EVONIX uses Rodin's existing Xiaomi navigation charge-pause policy. The
implementation is shared by the maintained HyperOS and ColorOS branches.
It is built into `Image`; no replacement charger modules, vendor_boot image,
or recovery modification is required for this backend.

## Operation

Enabling requests the stock navigation SOC-limit feature with threshold zero.
The OEM charger manager pauses battery charging, exits charge-pump charging,
and retains its normal adapter-to-system power path. Disabling releases only
the navigation request owned by EVONIX and lets OEM charging resume.

Other smart-charging feature bits and parameters are not overwritten. Enabling
returns `EBUSY` if navigation charge limiting is already enabled by another
owner, because the stock getter cannot recover that owner's threshold.

The backend never sets input suspend, forces the power path past OEM safety
decisions, changes charge-pump ratios or protection thresholds, disables ADCs,
or hooks charger functions. Thermal and electrical protections remain under
the existing OEM drivers. Adapter power may be insufficient under heavy load;
the connected battery can supplement system power. This is not electrical
battery isolation and does not promise zero battery current under every load.

## Interface

Attributes are under `/sys/class/power_supply/battery/`:

| Attribute | Meaning |
| --- | --- |
| `bypass_charging` | Read/write boolean request owned by EVONIX |
| `bypass_charge` | Alias of the request attribute |
| `bypass_charging_supported` | Required stock measurement interfaces exist |
| `bypass_charging_active` | Sustained battery-neutral observation, not the requested bit |
| `bypass_charging_diagnostics` | API version, backend, ownership and last error |

The active measurement requires connected USB input, no input suspension,
the OEM charge-pump state machine stopped, approximately 5 V at the primary
charger, and battery current within +/-100 mA for at least three seconds.
Unknown or failing measurements do not report active. Observation gaps over
two seconds restart confirmation: consumers should read at about 1 Hz while
enabled and connected. The application backend can keep observing after the
UI closes; these reads must not reapply charger controls.

The initial settling interval is real verification. A page must not add its
own timer after kernel confirmation. The legacy battery `status` field may
still say `Charging`; it alone cannot establish battery-current direction.

Both the request and observation are distinct from boot persistence. The
kernel starts with bypass off. A ROM-native daemon or module service is
responsible for restoring a user's saved request after boot. Closing the UI
does not release an already accepted kernel request.

## Implementation

- `drivers/misc/evonix_oem_bypass.c`: request ownership and measured status.
- `fs/sysfs/evonix_supply.c`: narrowly scoped access to OEM attributes using
  kernfs active references, including protection against device removal.
- `include/linux/evonix_oem_bypass.h`: built-in interface, not a vendor KMI.

There are no cached vendor function pointers or private structure offsets.
The accessor writes only the existing navigation-policy command. It does not
depend on filesystem mounts or the requesting app's SELinux permissions;
the new writable request attributes still require privileged backend access.
An incompatible vendor charger implementation is not made compatible merely
by choosing a different ROM or seeing a support flag.

### Source structure and build placement

```text
drivers/misc/Makefile
  -> evonix_oem_bypass.o          request, ownership, sysfs and observations
include/linux/evonix_oem_bypass.h
  -> built-in accessor declarations
fs/sysfs/Makefile
  -> evonix_supply.o              guarded calls to existing OEM attributes
stock vendor charger manager
  -> smart_chg navigation slot    battery charge-pause policy
```

Both objects are built into the kernel Image with `obj-y`. The header declares
`evonix_oem_supply_read()` and `evonix_oem_navigation_set()`; it does not export
a replacement charger-module API. This is why the backend ships in the kernel
ZIP without replacing the vendor charger modules, vendor_boot or recovery.
The retired private-layout bypass experiments are not the compiled backend.

Registration is limited to the `mediatek,MT6899` machine. A delayed work item
waits for the `battery` power supply, retrying at two-second intervals up to
60 times, then attaches the attribute group to that device. Missing or
incompatible OEM attributes are reported instead of emulating bypass.

### Enable and disable sequence

1. A privileged service writes a boolean to `battery/bypass_charging` (or its
   `bypass_charge` alias). The command mutex serializes requests.
2. The apply worker reads `battery/smart_chg`. If another owner already has
   navigation limiting enabled, a new EVONIX enable returns `EBUSY` rather than
   replacing an unreadable OEM threshold.
3. The built-in accessor submits only the navigation command: `3\n` to enable
   the navigation slot with threshold zero, or `2\n` to disable it. These are
   packed **OEM commands**, not current, voltage or wattage values. Other smart
   charging slots remain untouched.
4. The worker reads back `smart_chg` and checks its navigation bit (`BIT(1)`).
   A failed enable/readback triggers a disable rollback; rollback failure keeps
   ownership recorded so an outstanding pause can still be released later.
5. The write waits for the worker to finish and returns the actual command
   error. The state lock protects ownership and observation state. A successful
   request resets the evidence window and emits a power-supply change event.

An already owned, enabled navigation request is not rewritten on every status
read. Disabling an unowned request does not clear somebody else's navigation
control. The driver has no periodic task repeatedly forcing charger settings.

### Lifetime-safe OEM access

The accessor obtains a power-supply reference, takes a reference to the sysfs
parent and looks up the requested kernfs node. It holds a kernfs active
reference while calling the OEM attribute's `show` or `store` callback, then
releases those references. This protects the callback against concurrent
device/sysfs removal. It checks node ownership, attribute mode, callback
availability and returned lengths; read parsing failures propagate as errors.

Writes are fixed to the navigation command, not arbitrary caller-provided
attribute strings. Reads are restricted to `battery/smart_chg` and USB
`cp_sm_run_state`, `online`, `input_suspend` and `pmic_vbus`. Battery current
is read through the `bms` power-supply `CURRENT_NOW` property.

### Request versus observed active state

`bypass_charging=1` means EVONIX owns a navigation pause confirmed by the OEM
getter. It does not mean the battery has been electrically disconnected.
`bypass_charging_active=1` requires all of these observations:

- Owned navigation pause still present.
- USB online, `input_suspend=0`, and `cp_sm_run_state=0`.
- Primary charger VBUS between 4400 and 6000 mV.
- BMS battery current between -100000 and +100000 microamps continuously
  across the three-second confirmation window.

The active flag is a tolerance-based observation, **not an exact 0 mA claim**.
A failed sample or invalid condition clears confirmation. Read gaps over two
seconds restart the window; observation does not submit charging commands.
When bypass is inactive, the driver skips the extra USB/current measurements.
Diagnostics report `api=3`, `backend=oem-navigation`, ownership and last error.

### Rodin Essential integration responsibilities

The kernel interface accepts on/off; it does not store an app's 20/40/80/90%
threshold. Rodin Essential's daemon owns the saved user choice, waits until
the threshold is reached (or uses Immediate), enables the kernel request and
continues observing independently of the UI. Closing or force-stopping the
app must not be interpreted as disabling an accepted kernel request.

After reboot the driver starts unowned and off, so the daemon must restore the
saved policy. It must avoid competing charging-profile writes while bypass
is selected and distinguish waiting, requested and observed states. A fresh
page should use daemon/kernel state rather than restart its own verification
timer. Kernel support is necessary even when the ROM integrates the app natively.

## Validation scope

On 2026-10-02, the HyperOS KernelSU Next/SUSFS build was compiled with Clang 23,
ThinLTO and AutoFDO, passed the Kleaf KMI check, and booted on Rodin with
SELinux enforcing. Real-charger tests observed the charging current settle to
zero, charge-pump state stop, input remain enabled, and charging resume after
disable. UI enable and force-close tests also retained the paused state.

These observations validate the tested device and conditions. They do not
establish all-workload electrical isolation or runtime validation of every
ColorOS, normal, and permissive branch. Those variants retain their own
configuration and require device tests before equivalent claims are made.
