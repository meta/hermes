/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/PageAccessTracker.h" // @manual

#ifdef HERMES_HAS_REAL_PAGE_TRACKER

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "llvh/ADT/STLExtras.h"

using namespace hermes;

std::unique_ptr<volatile PageAccessTracker> PageAccessTracker::create(
    void *bufStart,
    size_t bufSize) {
  uint32_t pageSize = getpagesize();
  if (bufSize < pageSize) {
    llvh::errs()
        << "Nothing to track because the buffer is less than a page.\n";
    return nullptr;
  }
  // If buffer start is not page aligned, choose the next page's start address.
  void *bufStartPage =
      (void *)(((uintptr_t)bufStart + pageSize - 1) / pageSize * pageSize);
  // If buffer end is not page aligned, choose the last page end address that
  // is inside the buffer.
  void *bufEndPage =
      (void *)(((uintptr_t)bufStart + bufSize) / pageSize * pageSize);
  assert(
      (uintptr_t)bufStartPage <= (uintptr_t)bufEndPage &&
      "Buffer start page should be before end page.");
  uint32_t totalPages =
      ((uintptr_t)bufEndPage - (uintptr_t)bufStartPage) / pageSize;

// On linux, failure to access the page triggers a sigsegv; on macOS, a sigbus.
#if defined(__linux__)
  const int signum = SIGSEGV;
#elif defined(__APPLE__) && defined(__MACH__)
  const int signum = SIGBUS;
#else
#error "OS type not supported."
#endif

  std::unique_ptr<volatile PageAccessTracker> tracker;
  tracker.reset(
      (new PageAccessTracker(pageSize, bufStartPage, totalPages, signum))
          ->install());

  // Mark the whole bytecode file unreadable.
  if (mprotect(bufStartPage, totalPages * pageSize, PROT_NONE) != 0) {
    perror("mprotect failed");
    tracker.reset();
  }

  return tracker;
}

volatile PageAccessTracker *PageAccessTracker::install() {
  // Register custom signal handler for accessing unreadable pages.
  assert(sigmuxCookie_ == nullptr);
  if (sigmux_init(signal_)) {
    perror("sigmux_init failed");
    return nullptr;
  }
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, signal_);
  sigmuxCookie_ = sigmux_register(&mask, signalHandler, this, 0);
  return this;
}

PageAccessTracker *PageAccessTracker::uninstall() volatile {
  if (sigmuxCookie_) {
    sigmux_unregister(sigmuxCookie_);
    sigmuxCookie_ = nullptr;
  }
  // We are no longer volatile.
  return const_cast<PageAccessTracker *>(this);
}

PageAccessTracker::~PageAccessTracker() {
  if (sigmuxCookie_) {
    sigmux_unregister(sigmuxCookie_);
  }
}

PageAccessTracker::PageAccessTracker(
    uint32_t pageSize,
    void *bufStartPage,
    uint32_t totalPages,
    int signal)
    : pageSize_(pageSize),
      bufStartPage_(bufStartPage),
      totalPages_(totalPages),
      signal_(signal) {
  accessedPageIds_ = std::make_unique<unsigned int[]>(totalPages);
  accessedMicros_ = std::make_unique<unsigned int[]>(totalPages);
}

void PageAccessTracker::recordPageAccess(void *accessedAddr, uint32_t micros) {
  assert(accessedPageCount_ < totalPages_ && "Unexpected page count overflow.");
  accessedPageIds_[accessedPageCount_] =
      ((uintptr_t)accessedAddr - (uintptr_t)bufStartPage_) / pageSize_;
  accessedMicros_[accessedPageCount_] = micros;
  ++accessedPageCount_;
}

bool PageAccessTracker::isTracking(void *addr) {
  return (uintptr_t)addr >= (uintptr_t)bufStartPage_ &&
      (uintptr_t)addr < (uintptr_t)bufStartPage_ + pageSize_ * totalPages_;
}

sigmux_action PageAccessTracker::signalHandler(
    struct sigmux_siginfo *siginfo,
    void *trackerPtr) {
  auto tracker = reinterpret_cast<PageAccessTracker *>(trackerPtr);
  siginfo_t *si = siginfo->info;
  if (!tracker) {
    // This is impossible because the signal handler would only be registered
    // if the PageAccessTracker is initialized.
    _exit(EXIT_FAILURE);
  }

  // Check if accessed address falls into the file buffer range.
  // If the address is not in range, tell sigmux to search for other handlers.
  if (si->si_signo != tracker->signal_ || !tracker->isTracking(si->si_addr)) {
    return SIGMUX_CONTINUE_SEARCH;
  }
  // Mark the page readable so we can continue execution.
  void *accessedPage = (void *)((uintptr_t)si->si_addr / tracker->pageSize_ *
                                tracker->pageSize_);
  if (mprotect(accessedPage, tracker->pageSize_, PROT_READ) != 0) {
    _exit(EXIT_FAILURE);
  }
  // Measure how long it takes to actually read the page.
  auto toNanos = [](timespec &ts) {
    return static_cast<double>(ts.tv_sec) * 1e9 + ts.tv_nsec;
  };
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  auto before = toNanos(t);
  // Read the first byte and "use" it in clock_gettime to prevent reordering.
  t.tv_nsec += *reinterpret_cast<char *>(accessedPage);
  clock_gettime(CLOCK_REALTIME, &t);
  auto after = toNanos(t);
  tracker->recordPageAccess(si->si_addr, (after - before) / 1000);

  // We successfully handled the signal.
  return SIGMUX_CONTINUE_EXECUTION;
}

void PageAccessTracker::printStats(llvh::raw_ostream &OS) {
  OS << "Bytecode I/O stats:\n";
  OS << "page size: " << pageSize_ << "\n";
  OS << "number of pages in bytecode file (rounded down): " << totalPages_
     << "\n";
  OS << "number of pages accessed during execution: " << accessedPageCount_
     << "\n";
  printPageAccessedOrder(OS);
}

void PageAccessTracker::printStatsJSON(llvh::raw_ostream &OS) {
  JSONEmitter json(OS);
  getJSONStats(json);
}

void PageAccessTracker::getJSONStats(JSONEmitter &json) {
  json.openDict();
  json.emitKeyValue("page_size", pageSize_);
  json.emitKeyValue("total_pages", totalPages_);
  json.emitKeyValue("accessed_pages", accessedPageCount_);
  json.emitKey("page_ids");
  json.openArray();
  for (unsigned i = 0; i < accessedPageCount_; ++i) {
    json.emitValue(accessedPageIds_[i]);
  }
  json.closeArray();
  json.emitKey("micros");
  json.openArray();
  for (unsigned i = 0; i < accessedPageCount_; ++i) {
    json.emitValue(accessedMicros_[i]);
  }
  json.closeArray();
  json.closeDict();
}

void PageAccessTracker::printPageAccessedOrder(llvh::raw_ostream &OS) {
  OS << "Page ids (and microseconds to access) in accessed order:\n";
  for (unsigned i = 0; i < accessedPageCount_; ++i) {
    OS << accessedPageIds_[i] << " (" << accessedMicros_[i] << " us)\n";
  }
}

void PageAccessTracker::printPageAccessedOrderJSON(llvh::raw_ostream &OS) {
  JSONEmitter json(OS);
  json.openDict();
  json.emitKey("page_ids");
  json.openArray();
  for (unsigned i = 0; i < accessedPageCount_; ++i) {
    json.emitValue(accessedPageIds_[i]);
  }
  json.closeArray();
  json.closeDict();
}

std::vector<uint32_t> PageAccessTracker::getPagesAccessed() volatile {
  auto tracker = uninstall();
  std::vector<uint32_t> result(
      tracker->accessedPageIds_.get(),
      tracker->accessedPageIds_.get() + tracker->accessedPageCount_);
  tracker->install();
  return result;
}

std::vector<uint32_t> PageAccessTracker::getMicros() volatile {
  auto tracker = uninstall();
  std::vector<uint32_t> result(
      tracker->accessedMicros_.get(),
      tracker->accessedMicros_.get() + tracker->accessedPageCount_);
  tracker->install();
  return result;
}

bool PageAccessTracker::printStats(llvh::raw_ostream &OS, bool json) volatile {
  auto tracker = uninstall();
  if (json) {
    tracker->printStatsJSON(OS);
  } else {
    tracker->printStats(OS);
  }
  tracker->install();
  return true;
}

void PageAccessTracker::getJSONStats(JSONEmitter &json) volatile {
  auto tracker = uninstall();
  tracker->getJSONStats(json);
  tracker->install();
}

bool PageAccessTracker::printPageAccessedOrder(
    llvh::raw_ostream &OS,
    bool json) volatile {
  auto tracker = uninstall();
  if (json) {
    tracker->printPageAccessedOrderJSON(OS);
  } else {
    tracker->printPageAccessedOrder(OS);
  }
  tracker->install();
  return true;
}

#endif // HERMES_HAS_REAL_PAGE_TRACKER
