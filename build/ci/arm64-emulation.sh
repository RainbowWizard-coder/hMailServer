#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Whether this machine runs an aarch64 program: the first step of every job on linix
# that runs arm64 code under qemu-user, or that an arm64 container's job follows
# (docs/PackagesDeb.md, "What is left"; Roadmap3, "The package install and test jobs
# pass in a pipeline").
#
# linix is x86-64. Its kernel hands an aarch64 ELF file to qemu-user through the
# binfmt_misc handler qemu-aarch64, which Ubuntu's qemu-user-binfmt package registers
# at every boot through systemd-binfmt, with the F flag: the emulator is opened once,
# when it is registered, and so runs in any container without being in it. On 29
# September 2026 a privileged container whose first process was systemd unregistered
# every handler on the host as it stopped, and each arm64 job after it failed with
# "exec format error": an arm64 container before its script could start, kaniko's
# arm64 RUN deep in its log. This asks the kernel the question those jobs depend on,
# and refuses with the answer before an hour's build.
#
# How: it writes a 132-byte aarch64 program - an ELF header, one loadable segment and
# three instructions that exit with status 42 - and runs it. Only a kernel that hands
# it to an aarch64 emulator, or an aarch64 machine, answers 42; any other status (126
# or 127 from the shell, or whatever the shell makes of the bytes read as a script) is
# a refusal. It asks by running a program, not by reading /proc/sys/fs/binfmt_misc,
# which a container does not mount and which would not say whether the F flag's
# emulator can be opened from here; where that directory is readable, the refusal
# says what is in it.
#
#   sh build/ci/arm64-emulation.sh [what needs it]
#
# POSIX sh, for kaniko's busybox as well as for bash. Exit 0 when the program ran and
# answered 42, 1 when it did not. The program is written into a directory of its own
# under TMPDIR or /tmp, or under the current directory where neither can be written,
# and removed.
set -u

why="${1:-this job}"

dir=""
for base in "${TMPDIR:-}" /tmp "$PWD"; do
  [ -n "$base" ] && [ -d "$base" ] && [ -w "$base" ] || continue
  dir=$(mktemp -d "$base/arm64-emulation.XXXXXX" 2> /dev/null) && break
  dir=""
done
if [ -z "$dir" ]; then
  echo "REFUSED: no directory to write the aarch64 probe into (TMPDIR, /tmp and $PWD)."
  exit 1
fi
trap 'rm -rf "$dir"' EXIT
probe="$dir/probe"

# The ELF header: ELFCLASS64, little-endian, version 1, the System V ABI; ET_EXEC for
# EM_AARCH64 (183); the entry point 0x400078; one program header at offset 64. Bytes 8
# to 15 are zeros, which is what qemu-user-binfmt's match for aarch64 asks for.
printf '\177\105\114\106\002\001\001\000\000\000\000\000\000\000\000\000' > "$probe"
printf '\002\000\267\000\001\000\000\000\170\000\100\000\000\000\000\000' >> "$probe"
printf '\100\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' >> "$probe"
printf '\000\000\000\000\100\000\070\000\001\000\000\000\000\000\000\000' >> "$probe"
# PT_LOAD, readable and executable: the whole 132 bytes at 0x400000, aligned to 64 KiB
# so that a kernel with 4, 16 or 64 KiB pages maps it.
printf '\001\000\000\000\005\000\000\000\000\000\000\000\000\000\000\000' >> "$probe"
printf '\000\000\100\000\000\000\000\000\000\000\100\000\000\000\000\000' >> "$probe"
printf '\204\000\000\000\000\000\000\000\204\000\000\000\000\000\000\000' >> "$probe"
printf '\000\000\001\000\000\000\000\000' >> "$probe"
# mov x0, #42; mov x8, #93 (exit); svc #0
printf '\100\005\200\322\250\013\200\322\001\000\000\324' >> "$probe"
chmod 0755 "$probe"

size=$(wc -c < "$probe" | tr -d ' ')
if [ "$size" != 132 ]; then
  echo "REFUSED: the aarch64 probe came out $size bytes, not 132: this shell's printf does not write octal escapes."
  exit 1
fi

"$probe" > "$dir/output" 2>&1
code=$?
if [ "$code" = 42 ]; then
  echo "This machine ($(uname -m)) runs aarch64 programs: an aarch64 program answered as it should."
  exit 0
fi

echo "REFUSED: $why needs this machine ($(uname -m)) to run aarch64 programs, and it cannot:"
echo "an aarch64 program that exits with status 42 exited with $code here."
if [ -s "$dir/output" ]; then
  echo "It printed:"
  sed -n '1,3p' "$dir/output" | cut -c 1-200
fi
handler=/proc/sys/fs/binfmt_misc/qemu-aarch64
if [ -r "$handler" ]; then
  echo "$handler: $(tr '\n' ' ' < "$handler")"
elif [ -r /proc/sys/fs/binfmt_misc/status ]; then
  echo "/proc/sys/fs/binfmt_misc has no qemu-aarch64 handler (binfmt_misc is $(cat /proc/sys/fs/binfmt_misc/status))."
fi
echo "On linix, which is x86-64, the kernel's qemu-aarch64 binfmt_misc handler is gone or disabled. systemd-binfmt"
echo "registers it at boot, with the F flag, from qemu-user-binfmt's /usr/lib/binfmt.d/qemu-aarch64.conf, and a"
echo "privileged container whose first process is systemd unregisters every handler on the host when it stops."
echo "As root on linix: systemctl restart systemd-binfmt; then ls /proc/sys/fs/binfmt_misc | grep -c qemu counts"
echo "31 again, and this job can be run again (docs/PackagesDeb.md, \"What is left\")."
exit 1
