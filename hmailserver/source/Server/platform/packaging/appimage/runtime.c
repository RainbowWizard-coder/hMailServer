/*
 * https://www.progressiverobot.com
 * Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * THE EXECUTABLE PART OF hMailServer'S AppImage
 *
 * An AppImage of type 2 is an ELF program - the runtime - with a SquashFS image
 * of an AppDir appended to it. Run, the runtime finds where its own ELF ends,
 * treats everything after that as the filesystem, mounts it read-only with FUSE
 * at a fresh directory, and runs the AppDir's AppRun with the environment the
 * AppImage format promises (APPIMAGE, APPDIR, OWD, ARGV0).
 *
 * This is the project's own runtime, written against the published AppImage
 * type 2 format and not copied from the AppImage project's type2-runtime, which
 * is distributed only through GitHub - and the project fetches nothing from
 * GitHub. It is built statically on Alpine from Alpine's own packages: squashfuse
 * (libsquashfuse and libsquashfuse_ll, the filesystem reader and its FUSE
 * operations), fuse3-static, and the static decompressors squashfuse was built
 * with (build/ci/packages/appimage-runtime.sh). A static program needs nothing
 * from the machine it runs on but the kernel and, to mount, fusermount3.
 *
 * What it does, in the order it decides:
 *
 *   --appimage-help, --appimage-version, --appimage-offset,
 *   --appimage-updateinformation
 *         say what they say and exit.
 *   --appimage-extract [PATTERN]
 *         writes the AppDir (or the paths matching the fnmatch PATTERN) to
 *         ./squashfs-root, printing each path.
 *   --appimage-mount
 *         mounts, prints the mount point, and keeps it until interrupted.
 *   --appimage-extract-and-run ARGS..., or APPIMAGE_EXTRACT_AND_RUN=1
 *         extracts to a fresh directory under $TMPDIR (or /tmp) and runs AppRun
 *         from there, without FUSE. The directory is removed when the program and
 *         everything it started have exited, unless NO_CLEANUP is set.
 *   anything else
 *         mounts with FUSE and runs AppRun with the arguments as given. Where
 *         FUSE cannot be had - no /dev/fuse, as in most containers, or no
 *         fusermount3 - it says so once on stderr and extracts and runs instead,
 *         rather than failing the way the reference runtime does.
 *
 * The runtime does not stay in between. It execs AppRun, so the process a
 * supervisor started IS the server, with its PID and its exit status, and a
 * signal sent to it reaches the server and nothing else. What keeps the mount
 * (or the extracted copy) alive is a helper forked beforehand, which holds the
 * read end of a pipe whose write end the server inherits: when the server and
 * every process that inherited the write end have exited, the helper reads end
 * of file, unmounts (or deletes the copy) and exits. The helper ignores SIGINT,
 * SIGTERM and SIGHUP, so a Ctrl-C at a terminal or a unit being stopped cannot
 * take the filesystem away from a server that is still shutting down.
 *
 * What it does not do: signatures (.sha256_sig) and desktop integration. The
 * .upd_info section the format reserves for update information is present and
 * empty; the server's own update checker (UpdateChecker.cpp) is how an AppImage
 * of hMailServer learns of a new release.
 */

#define _GNU_SOURCE

/* squashfuse's installed configuration first: it says which FUSE API the
   library's operations were compiled against, and the prototypes below must
   agree with it. */
#include <config.h>
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include <squashfuse.h>
#include <ll.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HM_RUNTIME_VERSION
#define HM_RUNTIME_VERSION "unversioned"
#endif

#define PREFIX "hMailServer AppImage: "

/* The format's update information: a section of this name, which tools that
   update an AppImage in place read. Empty - see the header. */
__attribute__((section(".upd_info"), used))
static const char update_information[1024] = "";

static void
die(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

static void
die(const char *format, ...)
{
   va_list arguments;
   fputs(PREFIX, stderr);
   va_start(arguments, format);
   vfprintf(stderr, format, arguments);
   va_end(arguments);
   fputc('\n', stderr);
   exit(127);
}

/* ------------------------------------------------------------- the offset */

/* Where the ELF ends and the filesystem begins: the furthest of the section
   header table, the program header table, and every section and segment that
   occupies bytes in the file. The reference runtime takes the section header
   table's end; taking the maximum gives the same answer for any ELF a linker
   writes and a right one for an ELF whose tables are not last. Both machines
   this is built for are little-endian and 64-bit, and anything else is refused
   rather than guessed at. */
static off_t
elf_size(int fd)
{
   Elf64_Ehdr header;
   off_t end;
   int i;

   if (pread(fd, &header, sizeof header, 0) != (ssize_t) sizeof header)
      return -1;
   if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
       header.e_ident[EI_CLASS] != ELFCLASS64 ||
       header.e_ident[EI_DATA] != ELFDATA2LSB)
      return -1;

   end = (off_t) header.e_shoff + (off_t) header.e_shentsize * header.e_shnum;
   if ((off_t) header.e_phoff + (off_t) header.e_phentsize * header.e_phnum > end)
      end = (off_t) header.e_phoff + (off_t) header.e_phentsize * header.e_phnum;

   for (i = 0; i < header.e_shnum; i++)
   {
      Elf64_Shdr section;
      off_t at = (off_t) header.e_shoff + (off_t) i * header.e_shentsize;
      if (pread(fd, &section, sizeof section, at) != (ssize_t) sizeof section)
         return -1;
      if (section.sh_type != SHT_NOBITS && (off_t) (section.sh_offset + section.sh_size) > end)
         end = (off_t) (section.sh_offset + section.sh_size);
   }

   for (i = 0; i < header.e_phnum; i++)
   {
      Elf64_Phdr segment;
      off_t at = (off_t) header.e_phoff + (off_t) i * header.e_phentsize;
      if (pread(fd, &segment, sizeof segment, at) != (ssize_t) sizeof segment)
         return -1;
      if ((off_t) (segment.p_offset + segment.p_filesz) > end)
         end = (off_t) (segment.p_offset + segment.p_filesz);
   }

   return end;
}

/* ------------------------------------------------------------- helpers */

static const char *
temporary_directory(void)
{
   const char *tmp = getenv("TMPDIR");
   return (tmp && *tmp) ? tmp : "/tmp";
}

static void
set_environment(const char *image, const char *appdir, const char *argv0)
{
   char cwd[PATH_MAX];

   setenv("APPIMAGE", image, 1);
   setenv("APPDIR", appdir, 1);
   setenv("ARGV0", argv0, 1);
   if (getcwd(cwd, sizeof cwd))
      setenv("OWD", cwd, 1);
}

static void
exec_apprun(const char *appdir, int count, char **arguments)
{
   char apprun[PATH_MAX];
   char **argv;
   int i;

   if (snprintf(apprun, sizeof apprun, "%s/AppRun", appdir) >= (int) sizeof apprun)
      die("the path %s/AppRun is too long", appdir);

   argv = calloc((size_t) count + 2, sizeof *argv);
   if (!argv)
      die("out of memory");
   argv[0] = apprun;
   for (i = 0; i < count; i++)
      argv[i + 1] = arguments[i];

   execv(apprun, argv);
   die("could not run %s: %s", apprun, strerror(errno));
}

static int
mkdir_parents(const char *path)
{
   char copy[PATH_MAX];
   char *slash;

   if (snprintf(copy, sizeof copy, "%s", path) >= (int) sizeof copy)
      return -1;
   for (slash = strchr(copy + 1, '/'); slash; slash = strchr(slash + 1, '/'))
   {
      *slash = '\0';
      if (mkdir(copy, 0755) != 0 && errno != EEXIST)
         return -1;
      *slash = '/';
   }
   return 0;
}

/* A path from the image is used under the destination only when it is relative
   and has no ".." in it, so that no image, however it was made, writes outside
   the directory it is extracted to. */
static int
safe_relative(const char *path)
{
   const char *p = path;

   if (!*path || *path == '/')
      return 0;
   while (*p)
   {
      const char *slash = strchr(p, '/');
      size_t length = slash ? (size_t) (slash - p) : strlen(p);
      if (length == 2 && p[0] == '.' && p[1] == '.')
         return 0;
      if (!slash)
         break;
      p = slash + 1;
   }
   return 1;
}

static int
remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
   (void) st;
   (void) flag;
   (void) ftw;
   remove(path);
   return 0;
}

static void
remove_tree(const char *path)
{
   nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ------------------------------------------------------------- extraction */

static int
write_file(sqfs *fs, sqfs_inode *inode, const char *path)
{
   static char buffer[1 << 16];
   uint64_t size = inode->xtra.reg.file_size;
   uint64_t done = 0;
   int out;

   unlink(path);
   out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (out < 0)
      return -1;

   while (done < size)
   {
      sqfs_off_t chunk = (sqfs_off_t) ((size - done) < sizeof buffer ? (size - done) : sizeof buffer);
      char *at = buffer;
      if (sqfs_read_range(fs, inode, (sqfs_off_t) done, &chunk, buffer) != SQFS_OK || chunk <= 0)
      {
         close(out);
         return -1;
      }
      done += (uint64_t) chunk;
      while (chunk > 0)
      {
         ssize_t written = write(out, at, (size_t) chunk);
         if (written < 0)
         {
            if (errno == EINTR)
               continue;
            close(out);
            return -1;
         }
         at += written;
         chunk -= written;
      }
   }

   /* The permission bits, without set-user-ID, set-group-ID or sticky: an
      extracted copy belongs to whoever extracted it. */
   if (fchmod(out, inode->base.mode & 0777) != 0)
   {
      close(out);
      return -1;
   }
   return close(out);
}

static int
extract(const char *image, off_t offset, const char *destination, const char *pattern, int verbose)
{
   sqfs fs;
   sqfs_traverse traversal;
   sqfs_err error = SQFS_OK;
   int failed = 0;

   memset(&fs, 0, sizeof fs);
   if (sqfs_open_image(&fs, image, (size_t) offset) != SQFS_OK)
   {
      fprintf(stderr, PREFIX "the filesystem at offset %lld of %s cannot be read\n", (long long) offset, image);
      return -1;
   }

   if (mkdir(destination, 0755) != 0 && errno != EEXIST)
   {
      fprintf(stderr, PREFIX "cannot create %s: %s\n", destination, strerror(errno));
      sqfs_destroy(&fs);
      close(fs.fd);
      return -1;
   }

   if (sqfs_traverse_open(&traversal, &fs, sqfs_inode_root(&fs)) != SQFS_OK)
   {
      fprintf(stderr, PREFIX "the filesystem's root cannot be read\n");
      sqfs_destroy(&fs);
      close(fs.fd);
      return -1;
   }

   while (sqfs_traverse_next(&traversal, &error))
   {
      char path[PATH_MAX];
      sqfs_inode inode;

      if (traversal.dir_end)
         continue;
      if (!safe_relative(traversal.path))
      {
         fprintf(stderr, PREFIX "refusing the path \"%s\" in the image\n", traversal.path);
         failed = 1;
         break;
      }
      if (pattern && fnmatch(pattern, traversal.path, FNM_LEADING_DIR) != 0)
         continue;
      if (snprintf(path, sizeof path, "%s/%s", destination, traversal.path) >= (int) sizeof path ||
          mkdir_parents(path) != 0 ||
          sqfs_inode_get(&fs, &inode, traversal.entry.inode) != SQFS_OK)
      {
         fprintf(stderr, PREFIX "cannot extract %s\n", traversal.path);
         failed = 1;
         break;
      }

      if (S_ISDIR(inode.base.mode))
      {
         /* Owner-writable whatever the image says, or nothing could be
            extracted into it. */
         if (mkdir(path, (inode.base.mode & 0777) | 0700) != 0 && errno != EEXIST)
            failed = 1;
      }
      else if (S_ISREG(inode.base.mode))
      {
         if (write_file(&fs, &inode, path) != 0)
            failed = 1;
      }
      else if (S_ISLNK(inode.base.mode))
      {
         size_t size = 0;
         char *target;
         if (sqfs_readlink(&fs, &inode, NULL, &size) != SQFS_OK || !(target = malloc(size + 1)))
         {
            failed = 1;
         }
         else
         {
            size += 1;
            if (sqfs_readlink(&fs, &inode, target, &size) != SQFS_OK)
               failed = 1;
            else
            {
               unlink(path);
               if (symlink(target, path) != 0)
                  failed = 1;
            }
            free(target);
         }
      }
      /* Devices, pipes and sockets have no place in an AppDir and are skipped. */

      if (failed)
      {
         fprintf(stderr, PREFIX "cannot extract %s: %s\n", traversal.path, strerror(errno));
         break;
      }
      if (verbose)
         printf("%s\n", path);
   }

   if (error != SQFS_OK)
   {
      fprintf(stderr, PREFIX "the filesystem could not be read to its end\n");
      failed = 1;
   }

   sqfs_traverse_close(&traversal);
   sqfs_destroy(&fs);
   close(fs.fd);
   return failed ? -1 : 0;
}

/* ------------------------------------------------------------- the keeper */

/* The helper that outlives the exec. It holds the read end of the keep-alive
   pipe and waits for end of file, which comes when the last process holding
   the write end - the server, and whatever it started that inherited it - has
   gone. */
static void
keeper_signals(void)
{
   signal(SIGINT, SIG_IGN);
   signal(SIGTERM, SIG_IGN);
   signal(SIGHUP, SIG_IGN);
   signal(SIGPIPE, SIG_IGN);
   signal(SIGQUIT, SIG_IGN);
}

static void
quiet_standard_streams(int keep_stderr)
{
   int null = open("/dev/null", O_RDWR | O_CLOEXEC);
   if (null < 0)
      return;
   dup2(null, STDIN_FILENO);
   dup2(null, STDOUT_FILENO);
   if (!keep_stderr)
      dup2(null, STDERR_FILENO);
   if (null > STDERR_FILENO)
      close(null);
}

static void
wait_for_end_of_file(int fd)
{
   char byte;
   for (;;)
   {
      ssize_t n = read(fd, &byte, 1);
      if (n == 0 || (n < 0 && errno != EINTR))
         return;
   }
}

/* ------------------------------------------------------------- extract and run */

static int
extract_and_run(const char *image, off_t offset, const char *argv0, int count, char **arguments)
{
   char directory[PATH_MAX];
   int keep[2];
   pid_t keeper;

   if (snprintf(directory, sizeof directory, "%s/appimage_extracted_XXXXXX", temporary_directory()) >= (int) sizeof directory ||
       !mkdtemp(directory))
      die("cannot create a directory to extract to under %s: %s", temporary_directory(), strerror(errno));

   if (extract(image, offset, directory, NULL, 0) != 0)
   {
      remove_tree(directory);
      die("the AppImage could not be extracted to %s", directory);
   }

   if (!getenv("NO_CLEANUP"))
   {
      if (pipe(keep) != 0)
         die("pipe: %s", strerror(errno));
      keeper = fork();
      if (keeper < 0)
         die("fork: %s", strerror(errno));
      if (keeper == 0)
      {
         close(keep[1]);
         keeper_signals();
         quiet_standard_streams(0);
         /* Out of the terminal's process group, so that it is not the one that
            gets the terminal's signals; it ignores them anyway. */
         setsid();
         wait_for_end_of_file(keep[0]);
         remove_tree(directory);
         _exit(0);
      }
      close(keep[0]);
      /* keep[1] is inherited across the exec on purpose. */
   }

   set_environment(image, directory, argv0);
   exec_apprun(directory, count, arguments);
   return 127;
}

/* ------------------------------------------------------------- mount and run */

struct watcher
{
   int fd;
   const char *mountpoint;
   struct fuse_session *session;
};

static void *
watch(void *argument)
{
   struct watcher *w = argument;
   pid_t helper;

   wait_for_end_of_file(w->fd);
   fuse_session_exit(w->session);

   /* A lazy unmount: the kernel tears the connection down, which ends the loop
      below with ENODEV, as soon as nothing uses the filesystem - and nothing
      does, or the pipe would still be open. Root unmounts itself; anybody else
      asks the setuid helper, the way libfuse does. */
   if (geteuid() == 0 && umount2(w->mountpoint, MNT_DETACH) == 0)
      return NULL;

   helper = fork();
   if (helper == 0)
   {
      execl("/usr/bin/fusermount3", "fusermount3", "-u", "-q", "-z", "--", w->mountpoint, (char *) NULL);
      execlp("fusermount3", "fusermount3", "-u", "-q", "-z", "--", w->mountpoint, (char *) NULL);
      execlp("fusermount", "fusermount", "-u", "-q", "-z", "--", w->mountpoint, (char *) NULL);
      _exit(127);
   }
   if (helper > 0)
      waitpid(helper, NULL, 0);
   return NULL;
}

/* The FUSE side, in the forked helper. Says '1' on the ready pipe once the
   filesystem is mounted, or '0' when it cannot be. */
static int
serve(const char *image, off_t offset, const char *mountpoint, int ready, int keep)
{
   struct fuse_lowlevel_ops operations;
   char *fuse_argv[] = { "hmailserver-appimage", "-o", "ro,subtype=hmailserver-appimage", NULL };
   struct fuse_args arguments = FUSE_ARGS_INIT(3, fuse_argv);
   sqfs_ll_chan channel;
   struct watcher w;
   pthread_t thread;
   sqfs_ll *ll;

   keeper_signals();
   /* libfuse's own complaints ("fuse: device not found") would be noise before
      the fallback's one clear sentence; APPIMAGE_DEBUG keeps them. */
   quiet_standard_streams(getenv("APPIMAGE_DEBUG") != NULL);

   ll = sqfs_ll_open(image, (size_t) offset);
   if (!ll)
   {
      (void) !write(ready, "0", 1);
      return 1;
   }

   memset(&operations, 0, sizeof operations);
   operations.getattr = sqfs_ll_op_getattr;
   operations.opendir = sqfs_ll_op_opendir;
   operations.releasedir = sqfs_ll_op_releasedir;
   operations.readdir = sqfs_ll_op_readdir;
   operations.lookup = sqfs_ll_op_lookup;
   operations.open = sqfs_ll_op_open;
   operations.create = sqfs_ll_op_create;
   operations.release = sqfs_ll_op_release;
   operations.read = sqfs_ll_op_read;
   operations.readlink = sqfs_ll_op_readlink;
   operations.listxattr = sqfs_ll_op_listxattr;
   operations.getxattr = sqfs_ll_op_getxattr;
   operations.forget = sqfs_ll_op_forget;
   operations.statfs = stfs_ll_op_statfs;
   operations.init = sqfs_ll_op_init;

   if (sqfs_ll_mount(&channel, mountpoint, &arguments, &operations, sizeof operations, ll) != SQFS_OK)
   {
      (void) !write(ready, "0", 1);
      return 1;
   }

   w.fd = keep;
   w.mountpoint = mountpoint;
   w.session = channel.session;
   if (pthread_create(&thread, NULL, watch, &w) != 0)
   {
      sqfs_ll_unmount(&channel, mountpoint);
      (void) !write(ready, "0", 1);
      return 1;
   }

   (void) !write(ready, "1", 1);
   close(ready);
   setsid();

   fuse_session_loop(channel.session);

   pthread_join(thread, NULL);
   rmdir(mountpoint);
   return 0;
}

/* Returns only when the filesystem could not be mounted; the caller falls back
   to extracting. */
static void
mount_and_run(const char *image, off_t offset, const char *argv0, int count, char **arguments, int mount_only)
{
   char mountpoint[PATH_MAX];
   int keep[2];
   int ready[2];
   char status = 0;
   ssize_t n;
   pid_t helper;

   if (snprintf(mountpoint, sizeof mountpoint, "%s/.mount_hMailServer_XXXXXX", temporary_directory()) >= (int) sizeof mountpoint ||
       !mkdtemp(mountpoint))
      return;

   if (pipe(keep) != 0 || pipe2(ready, O_CLOEXEC) != 0)
   {
      rmdir(mountpoint);
      return;
   }
   /* The read end must not reach AppRun, or the server would hold both ends
      and the helper would never see end of file. */
   fcntl(keep[0], F_SETFD, FD_CLOEXEC);

   helper = fork();
   if (helper < 0)
   {
      rmdir(mountpoint);
      return;
   }
   if (helper == 0)
   {
      close(keep[1]);
      close(ready[0]);
      _exit(serve(image, offset, mountpoint, ready[1], keep[0]));
   }

   close(keep[0]);
   close(ready[1]);
   do
      n = read(ready[0], &status, 1);
   while (n < 0 && errno == EINTR);
   close(ready[0]);

   if (n != 1 || status != '1')
   {
      close(keep[1]);
      waitpid(helper, NULL, 0);
      rmdir(mountpoint);
      return;
   }

   if (mount_only)
   {
      printf("%s\n", mountpoint);
      fflush(stdout);
      /* Until a signal ends this process; its end closes keep[1], and the
         helper unmounts. */
      for (;;)
         pause();
   }

   set_environment(image, mountpoint, argv0);
   exec_apprun(mountpoint, count, arguments);
}

/* ------------------------------------------------------------- main */

static void
usage(void)
{
   printf("hMailServer AppImage runtime %s\n"
          "\n"
          "  --appimage-help                 this text\n"
          "  --appimage-version              the runtime's version\n"
          "  --appimage-offset               where the filesystem begins in this file\n"
          "  --appimage-updateinformation    the update information (none)\n"
          "  --appimage-extract [PATTERN]    extract to ./squashfs-root\n"
          "  --appimage-extract-and-run ...  extract to a temporary directory and run\n"
          "  --appimage-mount                mount, print the mount point, wait\n"
          "\n"
          "Anything else is passed to the server. APPIMAGE_EXTRACT_AND_RUN=1 always\n"
          "extracts instead of mounting; NO_CLEANUP=1 keeps the extracted copy;\n"
          "TMPDIR says where either goes.\n",
          HM_RUNTIME_VERSION);
}

int
main(int argc, char **argv)
{
   char image[PATH_MAX];
   const char *argument = argc > 1 ? argv[1] : "";
   const char *argv0 = argc > 0 ? argv[0] : "hMailServer.AppImage";
   struct stat st;
   off_t offset;
   int fd;

   if (!realpath("/proc/self/exe", image))
      die("cannot find this program's own file through /proc/self/exe: %s", strerror(errno));
   fd = open(image, O_RDONLY | O_CLOEXEC);
   if (fd < 0 || fstat(fd, &st) != 0)
      die("cannot read %s: %s", image, strerror(errno));
   offset = elf_size(fd);
   close(fd);
   if (offset <= 0)
      die("%s is not a 64-bit little-endian ELF this runtime understands", image);

   if (strcmp(argument, "--appimage-help") == 0)
   {
      usage();
      return 0;
   }
   if (strcmp(argument, "--appimage-version") == 0)
   {
      printf("hMailServer AppImage runtime %s\n", HM_RUNTIME_VERSION);
      return 0;
   }
   if (strcmp(argument, "--appimage-offset") == 0)
   {
      printf("%lld\n", (long long) offset);
      return 0;
   }
   if (strcmp(argument, "--appimage-updateinformation") == 0)
   {
      printf("%s\n", update_information);
      return 0;
   }

   if (offset >= st.st_size)
      die("nothing is appended to %s: this is the runtime alone, not an AppImage", image);

   if (strcmp(argument, "--appimage-extract") == 0)
      return extract(image, offset, "squashfs-root", argc > 2 ? argv[2] : NULL, 1) == 0 ? 0 : 1;
   if (strcmp(argument, "--appimage-extract-and-run") == 0)
      return extract_and_run(image, offset, argv0, argc - 2, argv + 2);
   if (strcmp(argument, "--appimage-mount") == 0)
   {
      mount_and_run(image, offset, argv0, 0, NULL, 1);
      die("the filesystem could not be mounted with FUSE (run with APPIMAGE_DEBUG=1 to see why)");
   }
   if (strncmp(argument, "--appimage-", 11) == 0)
      die("unknown option %s; --appimage-help lists the ones there are", argument);

   {
      const char *extract_flag = getenv("APPIMAGE_EXTRACT_AND_RUN");
      if (extract_flag && strcmp(extract_flag, "1") == 0)
         return extract_and_run(image, offset, argv0, argc - 1, argv + 1);
   }

   mount_and_run(image, offset, argv0, argc - 1, argv + 1, 0);
   fprintf(stderr, PREFIX "FUSE is not available here (no /dev/fuse, or no fusermount3), so this runs\n"
                   PREFIX "from a copy extracted under %s instead. APPIMAGE_EXTRACT_AND_RUN=1 skips the attempt.\n",
           temporary_directory());
   return extract_and_run(image, offset, argv0, argc - 1, argv + 1);
}
