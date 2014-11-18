// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crazy_linker_rdebug.h"

#include <elf.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "crazy_linker_debug.h"
#include "crazy_linker_proc_maps.h"
#include "crazy_linker_util.h"
#include "crazy_linker_system.h"
#include "elf_traits.h"

namespace crazy {

namespace {

// Find the full path of the current executable. On success return true
// and sets |exe_path|. On failure, return false and sets errno.
bool FindExecutablePath(String* exe_path) {
  // /proc/self/exe is a symlink to the full path. Read it with
  // readlink().
  exe_path->Resize(512);
  ssize_t ret = TEMP_FAILURE_RETRY(
      readlink("/proc/self/exe", exe_path->ptr(), exe_path->size()));
  if (ret < 0) {
    LOG_ERRNO("%s: Could not get /proc/self/exe link", __FUNCTION__);
    return false;
  }

  exe_path->Resize(static_cast<size_t>(ret));
  LOG("%s: Current executable: %s\n", __FUNCTION__, exe_path->c_str());
  return true;
}

// Given an ELF binary at |path| that is _already_ mapped in the process,
// find the address of its dynamic section and its size.
// |path| is the full path of the binary (as it appears in /proc/self/maps.
// |self_maps| is an instance of ProcMaps that is used to inspect
// /proc/self/maps. The function rewind + iterates over it.
// On success, return true and set |*dynamic_offset| and |*dynamic_size|.
bool FindElfDynamicSection(const char* path,
                           ProcMaps* self_maps,
                           size_t* dynamic_address,
                           size_t* dynamic_size) {
  // Read the ELF header first.
  ELF::Ehdr header[1];

  crazy::FileDescriptor fd;
  if (!fd.OpenReadOnly(path) ||
      fd.Read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERRNO("%s: Could not load ELF binary header", __FUNCTION__);
    return false;
  }

  // Sanity check.
  if (header->e_ident[0] != 127 || header->e_ident[1] != 'E' ||
      header->e_ident[2] != 'L' || header->e_ident[3] != 'F' ||
      header->e_ident[4] != ELF::kElfClass) {
    LOG("%s: Not a %d-bit ELF binary: %s\n",
        __FUNCTION__,
        ELF::kElfBits,
        path);
    return false;
  }

  if (header->e_phoff == 0 || header->e_phentsize != sizeof(ELF::Phdr)) {
    LOG("%s: Invalid program header values: %s\n", __FUNCTION__, path);
    return false;
  }

  // Scan the program header table.
  if (fd.SeekTo(header->e_phoff) < 0) {
    LOG_ERRNO("%s: Could not find ELF program header table", __FUNCTION__);
    return false;
  }

  ELF::Phdr phdr_load0 = {0, };
  ELF::Phdr phdr_dyn = {0, };
  bool found_load0 = false;
  bool found_dyn = false;

  for (size_t n = 0; n < header->e_phnum; ++n) {
    ELF::Phdr phdr;
    if (fd.Read(&phdr, sizeof(phdr)) != sizeof(phdr)) {
      LOG_ERRNO("%s: Could not read program header entry", __FUNCTION__);
      return false;
    }

    if (phdr.p_type == PT_LOAD && !found_load0) {
      phdr_load0 = phdr;
      found_load0 = true;
    } else if (phdr.p_type == PT_DYNAMIC && !found_dyn) {
      phdr_dyn = phdr;
      found_dyn = true;
    }
  }

  if (!found_load0) {
    LOG("%s: Could not find loadable segment!?\n", __FUNCTION__);
    return false;
  }
  if (!found_dyn) {
    LOG("%s: Could not find dynamic segment!?\n", __FUNCTION__);
    return false;
  }

  LOG("%s: Found first loadable segment [offset=%p vaddr=%p]\n",
      __FUNCTION__,
      (void*)phdr_load0.p_offset,
      (void*)phdr_load0.p_vaddr);

  LOG("%s: Found dynamic segment [offset=%p vaddr=%p size=%p]\n",
      __FUNCTION__,
      (void*)phdr_dyn.p_offset,
      (void*)phdr_dyn.p_vaddr,
      (void*)phdr_dyn.p_memsz);

  // Parse /proc/self/maps to find the load address of the first
  // loadable segment.
  size_t path_len = strlen(path);
  self_maps->Rewind();
  ProcMaps::Entry entry;
  while (self_maps->GetNextEntry(&entry)) {
    if (!entry.path || entry.path_len != path_len ||
        memcmp(entry.path, path, path_len) != 0)
      continue;

    LOG("%s: Found executable segment mapped [%p-%p offset=%p]\n",
        __FUNCTION__,
        (void*)entry.vma_start,
        (void*)entry.vma_end,
        (void*)entry.load_offset);

    size_t load_bias = entry.vma_start - phdr_load0.p_vaddr;
    LOG("%s: Load bias is %p\n", __FUNCTION__, (void*)load_bias);

    *dynamic_address = load_bias + phdr_dyn.p_vaddr;
    *dynamic_size = phdr_dyn.p_memsz;
    LOG("%s: Dynamic section addr=%p size=%p\n",
        __FUNCTION__,
        (void*)*dynamic_address,
        (void*)*dynamic_size);
    return true;
  }

  LOG("%s: Executable is not mapped in current process.\n", __FUNCTION__);
  return false;
}

// Helper class to temporarily remap a page to readable+writable until
// scope exit.
class ScopedPageMapper {
 public:
  ScopedPageMapper() : page_address_(0), page_prot_(0) {}
  void MapReadWrite(void* address);
  ~ScopedPageMapper();

 private:
  static const uintptr_t kPageSize = 4096;
  uintptr_t page_address_;
  int page_prot_;
};

void ScopedPageMapper::MapReadWrite(void* address) {
  page_address_ = reinterpret_cast<uintptr_t>(address) & ~(kPageSize - 1);
  page_prot_ = 0;
  if (!FindProtectionFlagsForAddress(address, &page_prot_) ||
      (page_prot_ & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE)) {
    // If the address is invalid, or if the page is already read+write,
    // no need to do anything here.
    page_address_ = 0;
    return;
  }
  int new_page_prot = page_prot_ | PROT_READ | PROT_WRITE;
  int ret = mprotect(
      reinterpret_cast<void*>(page_address_), kPageSize, new_page_prot);
  if (ret < 0) {
    LOG_ERRNO("Could not remap page to read/write");
    page_address_ = 0;
  }
}

ScopedPageMapper::~ScopedPageMapper() {
  if (page_address_) {
    int ret =
        mprotect(reinterpret_cast<void*>(page_address_), kPageSize, page_prot_);
    if (ret < 0)
      LOG_ERRNO("Could not remap page to old protection flags");
  }
}

}  // namespace

bool RDebug::Init() {
  // The address of '_r_debug' is in the DT_DEBUG entry of the current
  // executable.
  init_ = true;

  size_t dynamic_addr = 0;
  size_t dynamic_size = 0;
  String path;

  // Find the current executable's full path, and its dynamic section
  // information.
  if (!FindExecutablePath(&path))
    return false;

  ProcMaps self_maps;
  if (!FindElfDynamicSection(
           path.c_str(), &self_maps, &dynamic_addr, &dynamic_size)) {
    return false;
  }

  // Parse the dynamic table and find the DT_DEBUG entry.
  const ELF::Dyn* dyn_section = reinterpret_cast<const ELF::Dyn*>(dynamic_addr);

  while (dynamic_size >= sizeof(*dyn_section)) {
    if (dyn_section->d_tag == DT_DEBUG) {
      // Found it!
      LOG("%s: Found DT_DEBUG entry inside %s at %p, pointing to %p\n",
          __FUNCTION__,
          path.c_str(),
          dyn_section,
          dyn_section->d_un.d_ptr);
      if (dyn_section->d_un.d_ptr) {
        r_debug_ = reinterpret_cast<r_debug*>(dyn_section->d_un.d_ptr);
        LOG("%s: r_debug [r_version=%d r_map=%p r_brk=%p r_ldbase=%p]\n",
            __FUNCTION__,
            r_debug_->r_version,
            r_debug_->r_map,
            r_debug_->r_brk,
            r_debug_->r_ldbase);
        // Only version 1 of the struct is supported.
        if (r_debug_->r_version != 1) {
          LOG("%s: r_debug.r_version is %d, 1 expected.\n",
              __FUNCTION__,
              r_debug_->r_version);
          r_debug_ = NULL;
        }

        // The linker of recent Android releases maps its link map entries
        // in read-only pages. Determine if this is the case and record it
        // for later. The first entry in the list corresponds to the
        // executable.
        int prot = self_maps.GetProtectionFlagsForAddress(r_debug_->r_map);
        readonly_entries_ = (prot & PROT_WRITE) == 0;

        LOG("%s: r_debug.readonly_entries=%s\n",
            __FUNCTION__,
            readonly_entries_ ? "true" : "false");
        return true;
      }
    }
    dyn_section++;
    dynamic_size -= sizeof(*dyn_section);
  }

  LOG("%s: There is no non-0 DT_DEBUG entry in this process\n", __FUNCTION__);
  return false;
}

namespace {

// Helper class providing a simple scoped pthreads mutex.
class ScopedMutexLock {
 public:
  explicit ScopedMutexLock(pthread_mutex_t* mutex) : mutex_(mutex) {
    pthread_mutex_lock(mutex_);
  }
  ~ScopedMutexLock() {
    pthread_mutex_unlock(mutex_);
  }

 private:
  pthread_mutex_t* mutex_;
};

// Helper runnable class. Handler is one of the two static functions
// AddEntryInternal() or DelEntryInternal(). Calling these invokes
// AddEntryImpl() or DelEntryImpl() respectively on rdebug.
class RDebugRunnable {
 public:
  RDebugRunnable(rdebug_callback_handler_t handler,
                 RDebug* rdebug,
                 link_map_t* entry,
                 bool is_blocking)
      : handler_(handler), rdebug_(rdebug),
        entry_(entry), is_blocking_(is_blocking), has_run_(false) {
    pthread_mutex_init(&mutex_, NULL);
    pthread_cond_init(&cond_, NULL);
  }

  static void Run(void* opaque);
  static void WaitForCallback(void* opaque);

 private:
  rdebug_callback_handler_t handler_;
  RDebug* rdebug_;
  link_map_t* entry_;
  bool is_blocking_;
  bool has_run_;
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
};

// Callback entry point.
void RDebugRunnable::Run(void* opaque) {
  RDebugRunnable* runnable = static_cast<RDebugRunnable*>(opaque);

  LOG("%s: Callback received, runnable=%p\n", __FUNCTION__, runnable);
  (*runnable->handler_)(runnable->rdebug_, runnable->entry_);

  if (!runnable->is_blocking_) {
    delete runnable;
    return;
  }

  LOG("%s: Signalling callback, runnable=%p\n", __FUNCTION__, runnable);
  {
    ScopedMutexLock m(&runnable->mutex_);
    runnable->has_run_ = true;
    pthread_cond_signal(&runnable->cond_);
  }
}

// For blocking callbacks, wait for the call to Run().
void RDebugRunnable::WaitForCallback(void* opaque) {
  RDebugRunnable* runnable = static_cast<RDebugRunnable*>(opaque);

  if (!runnable->is_blocking_) {
    LOG("%s: Non-blocking, not waiting, runnable=%p\n", __FUNCTION__, runnable);
    return;
  }

  LOG("%s: Waiting for signal, runnable=%p\n", __FUNCTION__, runnable);
  {
    ScopedMutexLock m(&runnable->mutex_);
    while (!runnable->has_run_)
      pthread_cond_wait(&runnable->cond_, &runnable->mutex_);
  }

  delete runnable;
}

}  // namespace

// Helper function to schedule AddEntry() and DelEntry() calls onto another
// thread where possible. Running them there avoids races with the system
// linker, which expects to be able to set r_map pages readonly when it
// is not using them and which may run simultaneously on the main thread.
bool RDebug::PostCallback(rdebug_callback_handler_t handler,
                          link_map_t* entry,
                          bool is_blocking) {
  if (!post_for_later_execution_) {
    LOG("%s: Deferred execution disabled\n", __FUNCTION__);
    return false;
  }

  RDebugRunnable* runnable =
      new RDebugRunnable(handler, this, entry, is_blocking);
  void* context = post_for_later_execution_context_;

  if (!(*post_for_later_execution_)(context, &RDebugRunnable::Run, runnable)) {
    LOG("%s: Deferred execution enabled, but posting failed\n", __FUNCTION__);
    delete runnable;
    return false;
  }

  LOG("%s: Posted for later execution, runnable=%p\n", __FUNCTION__, runnable);

  if (is_blocking) {
    RDebugRunnable::WaitForCallback(runnable);
    LOG("%s: Completed execution, runnable=%p\n", __FUNCTION__, runnable);
  }

  return true;
}

void RDebug::AddEntryImpl(link_map_t* entry) {
  LOG("%s: Adding: %s\n", __FUNCTION__, entry->l_name);
  if (!init_)
    Init();

  if (!r_debug_) {
    LOG("%s: Nothing to do\n", __FUNCTION__);
    return;
  }

  // Tell GDB the list is going to be modified.
  r_debug_->r_state = RT_ADD;
  r_debug_->r_brk();

  // IMPORTANT: GDB expects the first entry in the list to correspond
  // to the executable. So add our new entry just after it. This is ok
  // because by default, the linker is always the second entry, as in:
  //
  //   [<executable>, /system/bin/linker, libc.so, libm.so, ...]
  //
  // By design, the first two entries should never be removed since they
  // can't be unloaded from the process (they are loaded by the kernel
  // when invoking the program).
  //
  // TODO(digit): Does GDB expect the linker to be the second entry?
  // It doesn't seem so, but have a look at the GDB sources to confirm
  // this. No problem appear experimentally.
  //
  // What happens for static binaries? They don't have an .interp section,
  // and don't have a r_debug variable on Android, so GDB should not be
  // able to debug shared libraries at all for them (assuming one
  // statically links a linker into the executable).
  if (!r_debug_->r_map || !r_debug_->r_map->l_next ||
      !r_debug_->r_map->l_next->l_next) {
    // Sanity check: Must have at least two items in the list.
    LOG("%s: Malformed r_debug.r_map list\n", __FUNCTION__);
    r_debug_ = NULL;
    return;
  }

  link_map_t* before = r_debug_->r_map->l_next;
  link_map_t* after = before->l_next;

  // Prepare the new entry.
  entry->l_prev = before;
  entry->l_next = after;

  // IMPORTANT: Before modifying the previous and next entries in the
  // list, ensure that they are writable. This avoids crashing when
  // updating the 'l_prev' or 'l_next' fields of a system linker entry,
  // which are mapped read-only.
  {
    ScopedPageMapper mapper;
    if (readonly_entries_)
      mapper.MapReadWrite(before);
    before->l_next = entry;
  }

  {
    ScopedPageMapper mapper;
    if (readonly_entries_)
      mapper.MapReadWrite(after);
    after->l_prev = entry;
  }

  // Tell GDB that the list modification has completed.
  r_debug_->r_state = RT_CONSISTENT;
  r_debug_->r_brk();
}

void RDebug::DelEntryImpl(link_map_t* entry) {
  LOG("%s: Deleting: %s\n", __FUNCTION__, entry->l_name);
  if (!r_debug_)
    return;

  // Tell GDB the list is going to be modified.
  r_debug_->r_state = RT_DELETE;
  r_debug_->r_brk();

  // IMPORTANT: Before modifying the previous and next entries in the
  // list, ensure that they are writable. See comment above for more
  // details.
  if (entry->l_prev) {
    ScopedPageMapper mapper;
    if (readonly_entries_)
      mapper.MapReadWrite(entry->l_prev);
    entry->l_prev->l_next = entry->l_next;
  }

  if (entry->l_next) {
    ScopedPageMapper mapper;
    if (readonly_entries_)
      mapper.MapReadWrite(entry->l_next);
    entry->l_next->l_prev = entry->l_prev;
  }

  if (r_debug_->r_map == entry)
    r_debug_->r_map = entry->l_next;

  entry->l_prev = NULL;
  entry->l_next = NULL;

  // Tell GDB the list modification has completed.
  r_debug_->r_state = RT_CONSISTENT;
  r_debug_->r_brk();
}

}  // namespace crazy
