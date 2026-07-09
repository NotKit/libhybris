/*
 * Copyright (C) 2019 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "linker_tls.h"
#include "linker_debug.h"

#include <vector>

#include <android/versioning.h>
#include "async_safe/CHECK.h"
#include "private/ScopedRWLock.h"
#include "private/ScopedSignalBlocker.h"
#include "private/bionic_defs.h"
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"
#include "private/linker_native_bridge.h"
#include "linker_main.h"
#include "linker_soinfo.h"
#include <stdint.h>

__LIBC_HIDDEN__ _Atomic(size_t) __libc_tls_generation_copy = {SIZE_MAX};

static bool g_static_tls_finished = false;
static std::vector<TlsModule> g_tls_modules = {};

extern "C" ssize_t g_hybris_static_tls_tp_offset = 0;
extern "C" size_t tls_tp_base = 0;

// hybris: per-thread bionic DTV pointer. Lives in the linker's own
// initial-exec TLS instead of a raw bionic TLS slot: slots are relative to
// the glibc thread pointer, and writing e.g. slot 2 (tp+16 on arm64) can
// alias TLS data of the main executable or an early-loaded library. Being a
// single pointer, it always fits glibc's static TLS surplus even though the
// linker is dlopened. The TP offset is exported for the TLSDESC resolvers,
// which cannot use C-level TLS access.
extern "C" __attribute__((tls_model("initial-exec"))) __thread void* hybris_dtv_slot = nullptr;
extern "C" ssize_t g_hybris_dtv_tp_offset = 0;

static size_t get_unused_module_index() {
  for (size_t i = 0; i < g_tls_modules.size(); ++i) {
    if (g_tls_modules[i].soinfo_ptr == nullptr) {
      return i;
    }
  }
  g_tls_modules.push_back({});
  __libc_shared_globals()->tls_modules.module_count = g_tls_modules.size();
  __libc_shared_globals()->tls_modules.module_table = g_tls_modules.data();
  return g_tls_modules.size() - 1;
}

static void register_tls_module(soinfo* si, size_t static_offset) {
  TlsModules& libc_modules = __libc_shared_globals()->tls_modules;

  // The global TLS module table points at the std::vector of modules declared
  // in this file, so acquire a write lock before modifying the std::vector.
  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&libc_modules.rwlock);

  size_t module_idx = get_unused_module_index();

  soinfo_tls* si_tls = si->get_tls();
  si_tls->module_id = __tls_module_idx_to_id(module_idx);

  const size_t new_generation = ++libc_modules.generation;
  __libc_tls_generation_copy = new_generation;
  // hybris: generation_libc_so is never registered since bionic libc.so's
  // constructor doesn't run; only the linker's own generation copy is used.

  g_tls_modules[module_idx].segment = si_tls->segment;
  g_tls_modules[module_idx].static_offset = static_offset;
  g_tls_modules[module_idx].first_generation = new_generation;
  g_tls_modules[module_idx].soinfo_ptr = si;
}

static void unregister_tls_module(soinfo* si) {
  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&__libc_shared_globals()->tls_modules.rwlock);

  soinfo_tls* si_tls = si->get_tls();
  TlsModule& mod = g_tls_modules[__tls_module_id_to_idx(si_tls->module_id)];
  // hybris: all modules use static TLS (backed by hybris_tls_storage),
  // so static_offset != SIZE_MAX is expected — skip original CHECK
  CHECK(mod.soinfo_ptr == si);
  mod = {};
  si_tls->module_id = kTlsUninitializedModuleId;
}

// The reference is valid until a TLS module is registered or unregistered.
const TlsModule& get_tls_module(size_t module_id) {
  size_t module_idx = __tls_module_id_to_idx(module_id);
  CHECK(module_idx < g_tls_modules.size());
  return g_tls_modules[module_idx];
}

void linker_finalize_static_tls() {
  g_static_tls_finished = true;
  __libc_shared_globals()->static_tls_layout.finish_layout();
  TlsModules& modules = __libc_shared_globals()->tls_modules;
  modules.static_module_count = modules.module_count;
}

void register_soinfo_tls(soinfo* si) {
  soinfo_tls* si_tls = si->get_tls();
  if (si_tls == nullptr || si_tls->module_id != kTlsUninitializedModuleId) {
    return;
  }
  size_t static_offset = SIZE_MAX;
  StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;

  if (!g_static_tls_finished) {
    // Attempt static allocation from our pre-allocated glibc array
    size_t offset = layout.reserve_solib_segment(si_tls->segment);
    if (!layout.overflowed()) {
      static_offset = offset;
    } else {
      // If it overflowed, we can't use static TLS for this module.
      DEBUG("hybris: static TLS storage exhausted, falling back to dynamic for %s", si->get_realpath());
      g_static_tls_finished = true;
    }
  }

  register_tls_module(si, static_offset);

  // Initialize the current thread's storage with the newly registered module's image
  if (static_offset != SIZE_MAX) {
    __init_static_tls_module(__tls_module_id_to_idx(si_tls->module_id));
  }
}

void unregister_soinfo_tls(soinfo* si) {
  soinfo_tls* si_tls = si->get_tls();
  if (si_tls == nullptr || si_tls->module_id == kTlsUninitializedModuleId) {
    return;
  }
  return unregister_tls_module(si);
}
