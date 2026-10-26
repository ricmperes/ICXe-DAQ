# A5818 kernel module

The CAEN A5818 PCIe card is the optical controller for the V1730SB. Userspace CAEN libraries talk to it through the `a5818` kernel module (Xilinx XDMA, modified by CAEN). Package sources are `/usr/src/a5818-1.0.4` and `/usr/src/a5818-1.0.5`. The installed DKMS module is **1.0.5**, driver version string **2020.2.2**. Nothing can be read from the digitizer until this module is loaded for the running kernel.

`scripts/check_link.sh` is a read-only check (no root, no `modprobe`). It reports lspci, the three CAEN sonames, `/usr/include` headers, `uname -r` against `modinfo a5818` vermagic, whether `a5818` is loaded, and whether `/dev/a5818*` exists. `/dev/xdma*` is an optional legacy node: its absence is not a failure when `/dev/a5818*` is present. If neither prefix exists and `a5818` is not loaded, the check still fails.

## State on 2026-10-26

| Check | Result |
| --- | --- |
| Running kernel | `5.14.0-570.49.1.el9_6.x86_64` |
| Installed module | `/lib/modules/5.14.0-570.49.1.el9_6.x86_64/extra/a5818.ko.xz` |
| Vermagic of that file | `5.14.0-503.23.1.el9_5.x86_64 SMP preempt mod_unload modversions` |
| `rhelversion` field | 9.5 |
| Module loaded | no |
| `/dev/xdma*` | absent |
| `/dev/a5818*` | absent |
| Secure Boot | disabled |
| `CONFIG_MODULE_SIG_FORCE` | not set |
| Lockdown | none |

`dkms status` lists `a5818/1.0.5` as installed for both `5.14.0-503.23.1.el9_5` and `5.14.0-570.49.1.el9_6`. Both copies report the **503** vermagic. The 503 file is the right module for that older kernel. The copy under the running kernel was built against the older headers.

PCI BARs stay disabled until the matching module loads (`lspci` shows `Mem-` and `BusMaster-`).

## Why the 570 DKMS build has 503 vermagic

`/usr/src/a5818-1.0.5/xdma/Makefile` compiles with:

```make
BUILDSYSTEM_DIR:=/lib/modules/$(shell uname -r)/build
```

DKMS invokes `cd xdma && make`, so the recipe follows `uname -r` at build time and ignores the kernel DKMS recorded. `/var/lib/dkms/a5818/1.0.5/5.14.0-570.49.1.el9_6.x86_64/x86_64/log/make.log` (Fri Oct 23 16:07:36 BST 2026) is labeled as a build for `5.14.0-570.49.1.el9_6.x86_64` and then runs:

```text
make -C /lib/modules/5.14.0-503.23.1.el9_5.x86_64/build M=/var/lib/dkms/a5818/1.0.5/build/xdma modules
make[1]: Entering directory '/usr/src/kernels/5.14.0-503.23.1.el9_5.x86_64'
```

That log exits 0. The module installed for the current kernel is therefore the el9_5 object. `modprobe` of that file on today's kernel will be rejected for vermagic once a privileged insert is possible.

## Load and rebuild attempts

These commands were run as user `xenon` on the running 570 kernel. The system module was not replaced, and `/usr/src/a5818-*` was not modified.

```text
$ modprobe a5818
modprobe: ERROR: could not insert 'a5818': Operation not permitted

$ sudo -n modprobe a5818
sudo: a password is required

$ sudo -n dkms build -m a5818 -v 1.0.5 -k 5.14.0-570.49.1.el9_6.x86_64
sudo: a password is required
```

A user-writable copy of the 1.0.5 sources was then built with kbuild, which passes `KERNELRELEASE` and does not use the Makefile's `uname -r` shortcut:

```text
$ rm -rf /tmp/a5818-rebuild-icxe
$ cp -a /usr/src/a5818-1.0.5 /tmp/a5818-rebuild-icxe
$ make -C /lib/modules/$(uname -r)/build M=/tmp/a5818-rebuild-icxe/xdma modules
```

That build exited 0. BTF generation was skipped because `vmlinux` is unavailable (the same skip is in the stock DKMS log). `modinfo` on the result:

| | Before (installed `a5818.ko.xz`) | After ( `/tmp/a5818-rebuild-icxe/xdma/a5818.ko` ) |
| --- | --- | --- |
| vermagic | `5.14.0-503.23.1.el9_5.x86_64 SMP preempt mod_unload modversions` | `5.14.0-570.49.1.el9_6.x86_64 SMP preempt mod_unload modversions` |
| rhelversion | 9.5 | 9.6 |
| signature | DKMS module signing key | unsigned |

Inserting the new object still needs `CAP_SYS_MODULE`:

```text
$ insmod /tmp/a5818-rebuild-icxe/xdma/a5818.ko
insmod: ERROR: could not insert module /tmp/a5818-rebuild-icxe/xdma/a5818.ko: Operation not permitted

$ sudo -n insmod /tmp/a5818-rebuild-icxe/xdma/a5818.ko
sudo: a password is required
```

**Blocker:** `sudo` for `xenon` requires a password, and there is no passwordless root path. That password was not available, so the module was not inserted. `a5818` is not loaded. `/dev/a5818*` and `/dev/xdma*` were not created. `/tmp/a5818-rebuild-icxe/xdma/a5818.ko` is built for the running kernel and is not installed into `/lib/modules`.

## Commands that still need root

The object `/tmp/a5818-rebuild-icxe/xdma/a5818.ko` is already built for `5.14.0-570.49.1.el9_6`. Do not rebuild it. `sudo` asks for the `xenon` password, which was not available here, so the block below has not been run and `a5818` is not loaded.

`insmod` is enough to test that file in this boot. The `install` and `depmod` lines copy the same file to `/lib/modules/$(uname -r)/extra` so `modprobe a5818` works after reboot. Delete the stale `a5818.ko.xz` for this kernel first; that compressed file still has the 503 vermagic, and `depmod` would keep it ahead of the new object. `install.sh` runs `dkms remove a5818/1.0.5 --all`, which also drops the good el9_5 build, so it is not the right next step. Leave `/lib/modules/5.14.0-503.23.1.el9_5.x86_64/extra/` in place.

```bash
modinfo /tmp/a5818-rebuild-icxe/xdma/a5818.ko | grep vermagic
sudo insmod /tmp/a5818-rebuild-icxe/xdma/a5818.ko
ls -l /dev/a5818*

sudo rm -f "/lib/modules/$(uname -r)/extra/a5818.ko.xz"
sudo install -D -m 644 /tmp/a5818-rebuild-icxe/xdma/a5818.ko \
    "/lib/modules/$(uname -r)/extra/a5818.ko"
sudo depmod -a
```

After the next reboot, load the installed copy with `sudo modprobe a5818`. Driver 1.0.5 creates `/dev/a5818*` and does not create `/dev/xdma*`.

The rebuilt object is unsigned. Secure Boot is off and `CONFIG_MODULE_SIG_FORCE` is unset, so the kernel should accept it. If `insmod` or `modprobe` then reports a signature error, sign with `/var/lib/dkms/mok.key` (root-only, mode 600) and the matching `mok.pub` before `depmod`.

## Device nodes

`XDMA_NODE_NAME` in `/usr/src/a5818-1.0.5/xdma/xdma_cdev.h` is `"a5818"`. After a successful `modprobe a5818`, device nodes are named `a5818%d_user`, `a5818%d_control`, `a5818%d_h2c_%d`, `a5818%d_c2h_%d`, `a5818%d_events_%d`, and `a5818%d_vme_events_%d` (index 0 for this single card). The stock Xilinx driver uses the `xdma` prefix; this CAEN tree does not. `check_link.sh` passes a loaded 1.0.5 driver when `/dev/a5818*` exists. It mentions `/dev/xdma*` only as an optional legacy node, and does not fail for a missing `xdma` prefix when `/dev/a5818*` is present.
