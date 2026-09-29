/*
 * https://www.progressiverobot.com
 * Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * offline COMMAND [ARGUMENT...]
 *
 * Runs COMMAND with the network refused: the source packages' builds
 * (.gitlab/ci/source-packages.yml) run dpkg-buildpackage and rpmbuild under it,
 * so a build that reaches for anything off the machine fails instead of
 * fetching it. `docker run --network none` and `unshare -n` would do the same,
 * and neither is open to a job on linix: it has no Docker socket, and its
 * containers are unprivileged under Docker's default seccomp profile, which
 * refuses unshare(2) (tried in WSL's Docker 29.1.3 on 28 September 2026: every
 * `unshare -n`, `-rn` and `-Urn` "Operation not permitted").
 *
 * What is open to any process is a seccomp filter of its own, under
 * no_new_privs, which the kernel keeps for every child and every exec and which
 * no child can lift. This one makes socket(2) fail with EACCES for every
 * address family but AF_UNIX and AF_NETLINK - so no IPv4 or IPv6 socket, no
 * packet socket, no DNS query can be made at all, loopback included - and makes
 * io_uring_setup(2) fail, since io_uring can open a socket without calling
 * socket(2). A system call of another architecture's table (a 32-bit program on
 * x86-64, or x32) is refused whole. Local sockets stay, which the build tools
 * use among themselves. It refuses making a socket, not using one: a network
 * socket already open in the process that runs it would stay usable, and the
 * jobs' shells have none.
 *
 * Built with the distribution's own compiler in the job, before the network
 * is refused: cc -O2 -o /usr/local/bin/offline build/ci/packages/offline.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define OFFLINE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OFFLINE_ARCH AUDIT_ARCH_AARCH64
#else
#error "offline.c knows the system call table of x86-64 and AArch64 only"
#endif

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

#define REFUSE (SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA))

int main(int argc, char *argv[])
{
   if (argc < 2) {
      fprintf(stderr, "usage: offline COMMAND [ARGUMENT...]\n");
      return 64;
   }

   struct sock_filter filter[] = {
      /* Another architecture's system call numbers mean other calls: refuse them. */
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, OFFLINE_ARCH, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, REFUSE),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#if defined(__x86_64__)
      /* The x32 ABI shares the architecture and sets bit 30 of the number. */
      BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, REFUSE),
#endif
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, REFUSE),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      /* socket(domain, ...): the family is the first argument's low word. */
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 2, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, REFUSE),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
   };
   struct sock_fprog program = {
      .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
      .filter = filter,
   };

   if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      fprintf(stderr, "offline: PR_SET_NO_NEW_PRIVS: %s\n", strerror(errno));
      return 70;
   }
   if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
      fprintf(stderr, "offline: the seccomp filter was not installed: %s\n", strerror(errno));
      return 70;
   }

   execvp(argv[1], argv + 1);
   fprintf(stderr, "offline: %s: %s\n", argv[1], strerror(errno));
   return 127;
}
