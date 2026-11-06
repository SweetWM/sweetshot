#ifndef SWEETSHOT_EVDEV_H
#define SWEETSHOT_EVDEV_H

// evdev key and button codes; FreeBSD ships the same header in base under
// dev/evdev
#ifdef __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#else
#include <linux/input-event-codes.h>
#endif

#endif
