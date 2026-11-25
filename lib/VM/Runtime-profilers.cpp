/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "vm"

#include "hermes/VM/Runtime.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringView.h"

#ifdef HERMESVM_PROFILER_OPCODE
#include <iomanip>
#include <iostream>
#include "hermes/Inst/InstDecode.h"
#endif

namespace hermes {
namespace vm {

#ifdef HERMESVM_PROFILER_OPCODE
void Runtime::dumpOpcodeStats(llvh::raw_ostream &os) const {
  std::ostringstream stream;
  // Get all non-zero occurrence opcodes.
  std::vector<size_t> idx;
  for (size_t i = 0; i < static_cast<uint32_t>(inst::OpCode::_last); ++i) {
    if (opcodeExecuteFrequency[i])
      idx.push_back(i);
  }

  std::vector<size_t> idx_freq{idx};
  const auto *t = (const uint64_t *)timeSpent;
  const auto *f = (const uint32_t *)opcodeExecuteFrequency;

  // sort indexes based on total time spent.
  sort(idx.begin(), idx.end(), [t](size_t i1, size_t i2) {
    return t[i1] > t[i2];
  });

  // sort indexes based on frequency.
  sort(idx_freq.begin(), idx_freq.end(), [f](size_t i1, size_t i2) {
    return f[i1] > f[i2];
  });

  stream << "Opcodes sorted by total time:\n"
         << std::left << std::setfill(' ') << std::setw(25)
         << "==Opcode==" << std::setw(22) << "==Time Spent==" << std::setw(11)
         << "==Frequency==" << "\n";
  for (uint32_t i = 0; i < idx.size(); ++i) {
    stream << std::left << std::setfill(' ') << std::setw(25)
           << inst::getOpCodeString(static_cast<inst::OpCode>(idx[i])).data()
           << std::setw(22) << t[idx[i]] << std::setw(11) << f[idx[i]] << "\n";
  }

  stream << "\nOpcodes sorted by frequency:\n"
         << std::left << std::setfill(' ') << std::setw(25)
         << "==Opcode==" << std::setw(22) << "==Time Spent==" << std::setw(11)
         << "==Frequency==" << "\n";
  for (uint32_t i = 0; i < idx_freq.size(); ++i) {
    size_t op = idx_freq[i];
    stream << std::left << std::setfill(' ') << std::setw(25)
           << inst::getOpCodeString(static_cast<inst::OpCode>(op)).data()
           << std::setw(22) << t[op] << std::setw(11) << f[op] << "\n";
  }
  os << stream.str();
}
#endif

#if defined(HERMESVM_PROFILER_JSFUNCTION)
std::atomic<ProfilerID> Runtime::nextProfilerId;

ProfilerID Runtime::getProfilerID(CodeBlock *block) {
  auto &profilerID = block->profilerID;
  if (profilerID == NO_PROFILER_ID) {
    std::string name = block->getNameString();
    profilerID = nextProfilerId.fetch_add(1, std::memory_order_relaxed);
    functionInfo.emplace_back(
        profilerID,
        name,
        block->getFunctionID(),
        block->getRuntimeModule()->getBytecodeSharedPtr());
  }
  return profilerID;
}

const ProfilerFunctionInfo *Runtime::getProfilerInfo(ProfilerID id) {
  auto iterator = std::lower_bound(
      functionInfo.begin(),
      functionInfo.end(),
      id,
      [](ProfilerFunctionInfo &info, ProfilerID id) {
        return info.profilerId < id;
      });

  if (iterator == functionInfo.end()) {
    return nullptr;
  }

  if (iterator->profilerId != id) {
    return nullptr;
  }

  return &(*iterator);
}
#endif

#if defined(HERMESVM_PROFILER_JSFUNCTION)
/// Aggregate JS Function Profiling results by adding total time spent
/// per stacktrace and dump it to llvh::outs(). Each line of the output
/// includes a stacktrace with function names separated by semicolon,
/// followed by a tab, and then the total time (in terms of number of clock
/// cycles) spent in this stack trace. Each function name is displayed with
/// a prefixing line:column or ProfilerID, as some functions do not have names.
///
/// ProfileType::OPCODES uses opcodes executed instead of time spent.
///
/// TODO: Record line:col for inner functions.
void Runtime::dumpJSFunctionStats(ProfileType type) {
  if (type == ProfileType::ALL) {
    dumpJSFunctionStats(ProfileType::TIME);
    dumpJSFunctionStats(ProfileType::OPCODES);
    // The peak number of registers on the stack.
    llvh::outs() << "\nPeak stack usage: " << maxStackLevel << "\n";
    return;
  }
  const bool opcodes = type == ProfileType::OPCODES;
  // Map from a stacktrace to total exclusive time spent.
  std::map<std::vector<ProfilerID>, uint64_t> funcTimeSpent;
  // Current stacktrace.
  std::vector<ProfilerID> functionStack;
  // Timestamp to enter the current stacktrace.
  std::vector<uint64_t> enterTimes;
  // Total time spent in all children functions from the current stacktrace.
  std::vector<uint64_t> childrenTimes;

  for (const auto &event : functionEvents) {
    if (event.isEnter) {
      functionStack.push_back(event.functionID);
      enterTimes.push_back(opcodes ? event.opcodeStamp : event.timeStamp);
      childrenTimes.push_back(0);
    } else {
      if (functionStack.back() != event.functionID) {
        llvm_unreachable("Function name at enter/exit mismatch");
      }
      uint64_t inc =
          (opcodes ? event.opcodeStamp : event.timeStamp) - enterTimes.back();
      // inc now holds the inclusive time spent in the current stacktrace.
      uint64_t exc = inc - childrenTimes.back();
      // exc now holds the exclusive time spent in the current stacktrace.
      auto ret = funcTimeSpent.insert(std::make_pair(functionStack, exc));
      if (!ret.second) {
        // Stack already exists in the map, accumulate the time.
        ret.first->second += exc;
      }
      functionStack.pop_back();
      enterTimes.pop_back();
      childrenTimes.pop_back();
      if (!childrenTimes.empty()) {
        childrenTimes.back() += inc;
      }
    }
  }
  if (!(functionStack.empty() && enterTimes.empty() && childrenTimes.empty())) {
    llvm_unreachable("Function stack corrupted");
  }
  llvh::outs() << (opcodes ? "==Opcode Profile==\n" : " ==Time Profile==\n");
  for (const auto &kv : funcTimeSpent) {
    // Format of each line:
    //   [Stack trace (function ID/name separated by semicolon)] [timeSpent]
    bool isFirst = true;
    for (auto profID : kv.first) {
      if (!isFirst) {
        llvh::outs() << ";";
      }
      isFirst = false;
      const auto *info = getProfilerInfo(profID);
      assert(info);
      llvh::outs() << "[" << profID << "]" << info->functionName;
    }
    llvh::outs() << " " << kv.second << "\n";
  }
}
#endif

#ifdef HERMESVM_PROFILER_NATIVECALL

void Runtime::dumpNativeCallStats(llvh::raw_ostream &OS) {
  // Pick an arbitrary static function in Hermes to use as a base.
  const auto base = reinterpret_cast<uintptr_t>(ArrayStorage::ensureCapacity);

  OS << "Native call base offset 12ArrayStorage14ensureCapacity\n";
  OS << "Native call stats\n";
  OS << "=================\n";

  // The stats we gather for a single native function.
  struct Info {
    uint32_t count{};
    uint64_t duration{};
    uint64_t durationPerCall() const {
      return count != 0 ? duration / count : 0;
    }
  };

  // Map from a C++ function pointer to stats for that function. We need to
  // cast to "void *" because DenseMapInfo uses alignOf() by default and that
  // fails with pointers to function.
  using MapTy = llvh::DenseMap<void *, Info>;

  // There could be multiple ones pointing to the same native code, so we need
  // to merge them.
  MapTy funcMap{};

  // Scan the heap for instances of NativeFunction.
  heap_.forAllObjs([&funcMap](GCCell *cell) {
    auto *nf = dyn_vmcast<NativeFunction>(cell);
    if (!nf)
      return;
    auto &info = funcMap[(void *)nf->getFunctionPtr()];
    info.count += nf->getCallCount();
    info.duration += nf->getCallDuration();
  });

  // Sort by duration.
  std::vector<MapTy::value_type> funcs{funcMap.begin(), funcMap.end()};

  std::sort(
      funcs.begin(),
      funcs.end(),
      [](const MapTy::value_type &a, const MapTy::value_type &b) {
        return a.second.duration > b.second.duration;
      });

  OS << "Count   Total   Per-call Address-base\n";
  OS << "====== ======== ======== ============\n";

  for (const auto &entry : funcs) {
    auto ofs = reinterpret_cast<uintptr_t>(entry.first);

    OS << llvh::format_decimal(entry.second.count, 6) << " "
       << llvh::format_decimal(entry.second.duration, 8) << " "
       << llvh::format_decimal(entry.second.durationPerCall(), 8) << " ";

    // We have to deal with the possibility that the base has a larger address.
    // If that happens, we want to output a small negative number instead of
    // a 64-bit two's complement.
    if (ofs < base)
      OS << "-" << llvh::format_hex(base - ofs, 10);
    else
      OS << llvh::format_hex(ofs - base, 10);

    OS << " NativeCall\n";
  }
}

#endif // HERMESVM_PROFILER_NATIVECALL

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
