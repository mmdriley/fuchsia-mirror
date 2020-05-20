// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// Kernel Address Sanitizer (KASAN) is a tool to detect use-after-free, use-out-of-bounds, and
// other common memory errors in kernel.
// See: //zircon/kernel/lib/instrumentation/asan/README.md for more context.

#ifndef ZIRCON_KERNEL_LIB_INSTRUMENTATION_INCLUDE_LIB_INSTRUMENTATION_ASAN_H_
#define ZIRCON_KERNEL_LIB_INSTRUMENTATION_INCLUDE_LIB_INSTRUMENTATION_ASAN_H_

#ifdef __x86_64__

#define X86_KERNEL_KASAN_PDP_ENTRIES (64)
#define KASAN_SHADOW_OFFSET (0xffffffe000000000UL)

#endif  // __x86_64__

#ifdef __clang__
#define NO_ASAN [[clang::no_sanitize("address")]]
#else
#define NO_ASAN __attribute__((no_sanitize_address))
#endif

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

// ASAN dynamic poison interface - allows caller to "poison" or "unpoison" a region of kernel
// virtual addresses. Accesses to poisoned memory are invalid and may cause a fault or asan
// instrumentation check.
// This interface corresponds to the one in llvm compiler-rt/lib/asan/asan_interface.h
// It differs because we allow callers of asan_poison_shadow to specify a poison
// value.

// asan_poison_shadow() marks the memory region denoted by
// [address, round_down(address+size, kAsanGranularity)) as invalid. If the
// byte located at address+size is already poisoned, the entire region
// [address, address+size) is marked as invalid.
// Memory accesses to that region will fail asan checks.
//
// |value| annotates the 'type' of poison and must be one of the values in the
// 'distinguished kasan values' section below.
void asan_poison_shadow(uintptr_t address, size_t size, uint8_t value);

// asan_unpoison_shadow() marks [round_down(address, kAsanGranularity), address+size) as
// valid memory. Memory accesses to that region will not fail asan checks.
void asan_unpoison_shadow(uintptr_t address, size_t size);

// ASAN dynamic check functions - allows callers to check if an access would be valid without
// doing the access (aka poisoned). External accesses to a poisoned address is invalid and
// may cause a fault.

// Return the address of the first poisoned byte in [|address|, |address + size|).
// If no bytes are poisoned, returns 0.
uintptr_t asan_region_is_poisoned(uintptr_t address, size_t size);

// Return true if kernel |address| is poisoned.
bool asan_address_is_poisoned(uintptr_t address);

// Return true if all bytes in [|address|, |address + size|) are poisoned.
bool asan_entire_region_is_poisoned(uintptr_t address, size_t size);

// Returns number of bytes to add to heap allocations of |size| for a redzone, to detect out-of
// bounds accesses. (Rounds up the size to an ASAN granule)
size_t asan_heap_redzone_size(size_t size);

// Distinguished kasan poison values.
// LLVM defines userspace equivalents of these in compiler-rt/lib/asan/asan_internal.h
// There are some differences - kernel ASAN has distinguished states for Pmm free, for example.

// These constants are reserved by the compiler for stack poisoning.
inline constexpr uint8_t kAsanStackLeftRedzoneMagic = 0xf1;
inline constexpr uint8_t kAsanStackMidRedzoneMagic = 0xf2;
inline constexpr uint8_t kAsanStackRightRedzoneMagic = 0xf3;
inline constexpr uint8_t kAsanStackAfterReturnMagic = 0xf5;
inline constexpr uint8_t kAsanStackUseAfterScopeMagic = 0xf8;

// These constants are only known to the asan runtime.
inline constexpr uint8_t kAsanArrayCookie = 0xac;
inline constexpr uint8_t kAsanInternalHeapMagic = 0xf0;
inline constexpr uint8_t kAsanGlobalRedzoneMagic = 0xf9;
inline constexpr uint8_t kAsanHeapLeftRedzoneMagic = 0xfa;
inline constexpr uint8_t kAsanPmmFreeMagic = 0xfb;
inline constexpr uint8_t kAsanQuarantineMagic = 0xfc;
inline constexpr uint8_t kAsanHeapFreeMagic = 0xfd;
inline constexpr uint8_t kAsanAllocHeader = 0xff;

#endif  // __ASSEMBLER__

#endif  // ZIRCON_KERNEL_LIB_INSTRUMENTATION_INCLUDE_LIB_INSTRUMENTATION_ASAN_H_
