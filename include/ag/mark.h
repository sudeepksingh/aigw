/* SPDX-License-Identifier: Apache-2.0 */
/* Connection-mark layout: the contract between the AI (agd) and the kernel.
 *
 *  31        24 23        16 15         8 7          0
 * ┌───────────┬────────────┬────────────┬────────────┐
 * │ flags/ver │ zone/trust │ app class  │  action    │
 * └───────────┴────────────┴────────────┴────────────┘
 */
#ifndef AG_MARK_H
#define AG_MARK_H

#include <stdint.h>

enum ag_action {
	AG_ACT_UNLABELED  = 0,
	AG_ACT_ALLOW      = 1,
	AG_ACT_PRIORITY   = 2,
	AG_ACT_BULK       = 3,
	AG_ACT_THROTTLE   = 4,
	AG_ACT_QUARANTINE = 5,
	AG_ACT_BLOCK      = 6,
	AG_ACT_MIRROR     = 7,
};

#define AG_MARK_ACTION(m)  ((uint32_t)(m) & 0xffu)
#define AG_MARK_APP(m)     (((uint32_t)(m) >> 8) & 0xffu)
#define AG_MARK_ZONE(m)    (((uint32_t)(m) >> 16) & 0xffu)
#define AG_MARK_FLAGS(m)   (((uint32_t)(m) >> 24) & 0xffu)

#define AG_MARK(flags, zone, app, action) \
	((((uint32_t)(flags) & 0xffu) << 24) | (((uint32_t)(zone) & 0xffu) << 16) | \
	 (((uint32_t)(app) & 0xffu) << 8) | ((uint32_t)(action) & 0xffu))

#endif
