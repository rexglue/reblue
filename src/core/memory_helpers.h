/**
 * @file    core/memory_helpers.h
 * @brief   The Xbox 360 address space accessor.
 *
 *          A raw membase+va cast skips the 0xE0-heap 0x1000-byte compensation
 *          that TranslateVirtual applies. bd::mem is the one spelling that
 *          always routes through it.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <type_traits>

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace bd {

// rex/types.h exports be_u32 etc globally but not the be<> template itself.
template <typename T> using be = rex::be<T>;

namespace mem {

// T is the type as stored in memory, a be<> scalar for byte-swapped fields.
// at() swaps nothing itself. Null when the kernel is down or va is 0.
template <typename T> inline T *at(u32 va) {
  auto *memory = REX_KERNEL_MEMORY();
  if (!memory || !va)
    return nullptr;
  return memory->template TranslateVirtual<T *>(va);
}

// Scalar read through be<T>, so multi-byte types come back host-order.
template <typename T> inline T load(u32 va, T fallback = T{}) {
  auto *p = at<be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

template <typename T> inline bool store(u32 va, T value) {
  auto *p = at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

// Empty rather than null when unmapped.
inline const char *str(u32 va) {
  auto *p = at<const char>(va);
  return p ? p : "";
}

// ---- checked accessors ----
//
// at() translates whatever it is handed, since TranslateVirtual never fails, so
// a chained guest pointer freed and reused as unrelated data still resolves to
// a host address and faults on read. The try_ forms validate first, checking
// for non-null, aligned for T, and inside a committed readable heap, then
// return null or the fallback. Use them for anything reading a pointer chain
// off the guest thread: the console, the UI, the state readers. Guest thread
// sites that know their pointer is live can stay on at()/load().
//
// These are defined out of line, since the validation needs rex::Runtime and
// this header is included nearly everywhere.

// True once the guest address space exists.
bool ready();

// Host pointer for va, or nullptr when va fails validation. Prefer try_at.
void *try_translate(u32 va, u32 align);

template <typename T> inline T *try_at(u32 va) {
  return static_cast<T *>(try_translate(va, alignof(T)));
}

template <typename T> inline T try_load(u32 va, T fallback = T{}) {
  auto *p = try_at<const be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

// Validates readability, not writability, matching the accessor this replaced:
// the heaps it writes into are committed read-write, so a write-protect check
// would reject nothing it accepts today.
template <typename T> inline bool try_store(u32 va, T value) {
  auto *p = try_at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

// Separate from try_load(base + off) for the null base guard: a null base would
// otherwise read the offset as an address.
template <typename T> inline T try_field(u32 base, u32 off, T fallback = T{}) {
  return base ? try_load<T>(base + off, fallback) : fallback;
}

// A guest std::vector's { begin, end, capacity } pointer triple, declared in
// place inside a guest struct. T is the element type, so a vector of guest
// pointers, which is nearly all of them, is GuestVec<u32>.
template <typename T> struct GuestVec {
  be<u32> first;
  be<u32> last;
  be<u32> cap;

  bool empty() const {
    return !static_cast<u32>(first) ||
           static_cast<u32>(first) == static_cast<u32>(last);
  }

  u32 size() const {
    return empty() ? 0u
                   : (static_cast<u32>(last) - static_cast<u32>(first)) /
                         static_cast<u32>(sizeof(T));
  }

  // Guest address of element index, without a bounds check.
  u32 address(u32 index) const {
    return static_cast<u32>(first) + index * static_cast<u32>(sizeof(T));
  }

  // Zero past the end, matching what the loads this replaced returned for an
  // empty vector.
  T operator[](u32 index) const {
    if (index >= size())
      return T{};
    if constexpr (std::is_arithmetic_v<T>) {
      return load<T>(address(index));
    } else {
      auto *p = try_at<const T>(address(index));
      return p ? *p : T{};
    }
  }
};
static_assert(sizeof(GuestVec<u32>) == 0x0C);

template <typename T> struct GuestPtr {
  be<u32> va;

  u32 address() const { return static_cast<u32>(va); }
  explicit operator bool() const { return address() != 0; }
  T *get() const { return try_at<T>(address()); }
};
static_assert(sizeof(GuestPtr<int>) == 0x04);

} // namespace mem
} // namespace bd
