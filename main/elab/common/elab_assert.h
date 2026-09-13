/*
 * Minimal elab_assert.h shim for the 3rd-party ringbuf library.
 */
#ifndef __ELAB_ASSERT_H__
#define __ELAB_ASSERT_H__
#include <assert.h>
#ifndef ELAB_ASSERT
#define ELAB_ASSERT(x) assert(x)
#endif
#endif
