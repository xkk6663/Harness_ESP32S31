/*
 * Minimal elab_def.h shim for the 3rd-party ringbuf library.
 */
#ifndef __ELAB_DEF_H__
#define __ELAB_DEF_H__
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef ELAB_SIZE_ALIGN_DOWN
#define ELAB_SIZE_ALIGN_DOWN(x, align)  ((x) & ~((align) - 1))
#endif

#ifndef ELAB_TAG
#define ELAB_TAG(name)
#endif

#endif
