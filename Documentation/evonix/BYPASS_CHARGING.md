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
