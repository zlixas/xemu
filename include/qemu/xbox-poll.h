/*
 * Sleeping instead of spinning in guest polling loops (xemu)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_XBOX_POLL_H
#define QEMU_XBOX_POLL_H

/*
 * Signal that emulated hardware made progress a guest polling loop may be
 * waiting for: a device wrote guest memory or registers, or a vCPU was
 * kicked (e.g. for an interrupt).
 */
void xbox_poll_notify(void);

/* Sleep until the next xbox_poll_notify() or timeout_ns, whichever is first */
void xbox_poll_wait(int64_t timeout_ns);

#endif
