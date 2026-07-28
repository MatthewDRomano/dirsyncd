# dirsyncd

A small Linux daemon that mirrors a directory tree in near real time. Point it at a
`WATCH_PATH`, give it a `BACKUP_PATH`, and it uses `inotify` to keep the second
one in sync with the first — creates, writes, deletes, moves, and renames — without
polling or full-tree diffing.

No dependencies beyond glibc and a vendored copy of [uthash](https://troydhanson.github.io/uthash/).
No Makefile, no package manager, just a C file you compile and run.

## Why

Most "backup" tools either rsync on a timer (laggy, wasteful for mostly-idle trees)
or pull in a daemon-sized dependency tree to do something the kernel already exposes
for free via `inotify`. dirsyncd is the middle ground: event-driven, single binary,
readable source, and small enough to actually audit before you run it as root.

## How it works

On startup, dirsyncd recursively walks `WATCH_PATH` and registers an `inotify` watch
on every subdirectory, storing the watch descriptor → path mapping in a hash table
(`dsync_hash.c`, built on uthash). From then on it just drains the `inotify` event
queue and reacts:

- **File written** (`IN_CLOSE_WRITE`) → copy it into the mirrored location under `BACKUP_PATH`, preserving permission bits.
- **Directory created** → watch it, recreate it on the backup side.
- **File/dir deleted** → remove the mirrored copy.
- **Moved or renamed** → this is the fun one. `inotify` reports a move as two separate
  events, `IN_MOVED_FROM` and `IN_MOVED_TO`, correlated by a shared `cookie` value —
  and nothing guarantees the second one ever arrives (the file might have been moved
  outside the watched tree entirely). dirsyncd stashes the `MOVED_FROM` event in a
  cookie-keyed table and gives it a short grace window (one `poll()` timeout cycle)
  to be claimed by a matching `MOVED_TO`. If it's claimed, the backup gets renamed
  in place, even across two different watched directories. If it times out
  unclaimed, it's treated as a real delete.
- **Queue overflow** (`IN_Q_OVERFLOW`) → the kernel dropped events because the daemon
  fell behind. dirsyncd drains what's left, tears down every watch, and does a full
  rescan of `WATCH_PATH` to get back to a known-good state rather than silently
  drifting.

Everything is logged through `syslog` (facility `LOG_DAEMON`), and `SIGTERM` triggers
a clean shutdown — watches are released and hash tables freed before exit.

## Building

Requirements:

- C99-compliant compiler
- POSIX.1-2008 libc — for `getline()` (POSIX-only, never adopted into the C
  standard) and `strdup()` (POSIX.1-2008, standardized in C23); pulled in
  via `_POSIX_C_SOURCE 200809L` in-source, so no `-std=` flag is required
- Linux kernel with `inotify` support (≥ 2.6.13)

```sh
gcc -Wall -Wextra -O2 -o dirsyncd dirsyncd.c dsync_hash.c
```

That's it. Copy the resulting binary wherever you'd normally stash a system daemon
(`/usr/local/sbin`, etc.) and run it as a service (systemd unit, sysvinit script,
whatever your distro prefers) — it doesn't background/fork itself, so let your
service manager handle that part.

## Configuration

dirsyncd reads a single config file at `/etc/dirsyncd.conf` on startup:

```conf
WATCH_PATH=/home/matt/Projects
BACKUP_PATH=/mnt/SharedDrive/Projects

# Blacklist patterns below — one glob per line, matched against the
# basename of each file/dir (not the full path)

.*
*.log
*.tmp
*.swp
```

`WATCH_PATH` and `BACKUP_PATH` are required; the daemon refuses to start without
both, and validates that each one exists and is a directory. Anything below the
two path lines is treated as a blacklist entry — a shell-style glob (`fnmatch`)
checked against each entry's basename. Note that patterns matching a leading dot
need the dot written explicitly (`.*`, not `*`) — that's POSIX glob behavior, not
a bug.

Blank lines and lines starting with `#` are ignored, so you can comment your config
freely (see the shipped `dirsyncd.conf` for a documented example).

## Requirements & constraints

- **Linux only.** This is built directly on the `inotify` syscall family; there's
  no fallback for other platforms.
- **`WATCH_PATH` and `BACKUP_PATH` must never overlap or nest.** If the backup
  directory lives inside the watched tree (or vice versa), the daemon will start
  mirroring its own mirror, recursively, until it runs out of inotify watches or
  disk space. Keep them on genuinely separate paths.
- **Permissions**: the daemon needs read access throughout `WATCH_PATH` and
  read/write access throughout `BACKUP_PATH`. Don't hand-edit anything under
  `BACKUP_PATH` — the daemon assumes it's the only writer and will get confused
  if the mirror drifts out from under it.
- **inotify watch limits**: every watched directory consumes one kernel watch
  descriptor. Deep or enormous trees may need `fs.inotify.max_user_watches`
  raised via sysctl.
- **`PATH_MAX`-length paths** aren't handled gracefully — pathologically long
  paths can get silently truncated when building internal buffers. On Linux, 
  `Path_MAX` is abundantly forgiving.
- **No symlink following.** Symlinks are stat'd with `lstat`, so the daemon
  won't traverse into or copy through them.
- **Under sustained event-queue overflow**, a move/rename can theoretically
  land as a delete on the backup side if the `MOVED_TO` half of the pair is
  among the events the kernel drops. The full-rescan recovery keeps the tree
  eventually consistent, but a rename that raced an overflow isn't guaranteed
  to preserve the file's identity in the interim.

## Status

This is a personal project under active iteration, not a polished 1.0. It's been
exercised against real create/write/delete/move/rename workloads, but it hasn't
seen adversarial or high-throughput testing yet. Treat it as "useful and
watched closely," not "fire and forget onto anything you can't afford to lose."
