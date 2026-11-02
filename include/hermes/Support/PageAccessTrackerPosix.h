/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SUPPORT_PAGEACCESSTRACKERPOSIX_H
#define HERMES_SUPPORT_PAGEACCESSTRACKERPOSIX_H

#include <sigmux/sigmux.h>
#include <signal.h>
#include <memory>
#include <vector>

#include "llvh/Support/raw_ostream.h"

#include "hermes/Support/JSONEmitter.h"

namespace hermes {

/// Track paging behavior over a mmapped address range.
/// Usage:
///   ... create a mmapped buffer ...
///   auto tracker = PageAccessTracker::create(...);
///   ... do your thing ...
///   tracker->printStats(...);
class PageAccessTracker {
 private:
  uint32_t pageSize_{0};
  /// Page aligned address for the beginning of the buffer.
  void *bufStartPage_{nullptr};
  /// Total number of pages in the that we are tracking, starting from
  /// bufStartPage_.
  uint32_t totalPages_{0};
  /// Array of page ids in the order they are accessed.
  std::unique_ptr<uint32_t[]> accessedPageIds_{nullptr};
  /// Array of microseconds that each access took.
  std::unique_ptr<uint32_t[]> accessedMicros_{nullptr};
  /// Total number of pages accessed during executing bytecode.
  uint32_t accessedPageCount_{0};
  /// Signal number that we are tracking.
  const int signal_;
  /// Signal handling framework data.
  sigmux_registration *sigmuxCookie_{nullptr};

  PageAccessTracker(
      uint32_t pageSize,
      void *bufStartPage,
      uint32_t totalPages,
      int signal);

  /// Given \p accessedAddr, the accessed address, and \p micros, the number of
  /// microseconds it took, record the access.
  /// Intended to only be called by the signal handler.
  void recordPageAccess(void *accessedPage, uint32_t micros);

  /// Given \p addr, determine if we are tracking this address.
  bool isTracking(void *addr);

  /// Print the tracked stats, including page size, total number of pages,
  /// number of pages accessed, and the page ids in the accessed order.
  void printStats(llvh::raw_ostream &OS);
  void printStatsJSON(llvh::raw_ostream &OS);

  /// Get the tracked stats, including page size, total number of pages,
  /// number of pages accessed, page ids in the accessed order, and microseconds
  /// needed to access.
  void getJSONStats(JSONEmitter &json);

  /// Print only the page ids in the accessed order.
  void printPageAccessedOrder(llvh::raw_ostream &OS);
  void printPageAccessedOrderJSON(llvh::raw_ostream &OS);

  /// Handle the signal sent when an inaccesible page is touched. When the page
  /// touched is in the range we are tracking, mark it readable and continue
  /// execution; otherwise re-trigger the signal, handle it with default
  /// handler, and kill the process.
  static sigmux_action signalHandler(
      struct sigmux_siginfo *siginfo,
      void * /* unused */);

  /// Unregisters the signal handler and returns a non-volatile this, which
  /// can be used to access the collected stats.
  PageAccessTracker *uninstall() volatile;

  /// (Re)installs the signal handler and returns a volatile this.
  volatile PageAccessTracker *install();

 public:
  /// Create a new PageAccessTracker, mark the whole address range unreadable.
  /// Note if the beginning or end of the buffer is not on the page boundary,
  /// the first page or last page the buffer actually covers won't be tracked.
  /// \param bufStart pointer to the start of the buffer.
  /// \param bufSize the size of the buffer.
  /// \return The new object, or nullptr if creation failed.
  static std::unique_ptr<volatile PageAccessTracker> create(
      void *bufStart,
      size_t bufSize);

  /// Get the page ids in the order they were accessed.
  std::vector<uint32_t> getPagesAccessed() volatile;

  /// Get the microseconds for each access.
  std::vector<uint32_t> getMicros() volatile;

  /// Print the tracked stats, including page size, total number of pages,
  /// number of pages accessed, and the page ids in the accessed order.
  /// \param json If true, print in json format, otherwise in a more readable
  /// format.
  /// \return true if function completed successfully, false on failure.
  bool printStats(llvh::raw_ostream &OS, bool json) volatile;

  /// Public wrapper around the non-volatile version of this method.
  void getJSONStats(JSONEmitter &json) volatile;

  /// Print only the page ids in the accessed order.
  /// \param json If true, print in json format, otherwise in a more readable
  /// format.
  /// \return true if function completed successfully, false on failure.
  bool printPageAccessedOrder(llvh::raw_ostream &OS, bool json) volatile;

  /// Unregisters the signal handler.
  ~PageAccessTracker();
};

} // namespace hermes

#endif // HERMES_SUPPORT_PAGEACCESSTRACKERPOSIX_H
