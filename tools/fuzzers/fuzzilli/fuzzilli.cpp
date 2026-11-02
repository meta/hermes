/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI
#include <jsi/jsi.h>

#include <cstring>

using facebook::hermes::HermesRuntime;
using facebook::hermes::makeHermesRuntime;
using facebook::jsi::Function;
using facebook::jsi::HostObject;
using facebook::jsi::JSIException;
using facebook::jsi::Object;
using facebook::jsi::PropNameID;
using facebook::jsi::Runtime;
using facebook::jsi::String;
using facebook::jsi::Value;

//
// BEGIN FUZZING CODE
//
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103

#define SHM_SIZE 0x100000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)

#define CHECK(cond)                            \
  if (!(cond)) {                               \
    fprintf(stderr, "\"" #cond "\" failed\n"); \
    _exit(-1);                                 \
  }

struct SharedMemData {
  uint32_t numEdges;
  unsigned char edges[];
};

struct SharedMemData *shmem;

uint32_t *__edges_start, *__edges_stop;
void __sanitizer_cov_reset_edgeguards() {
  uint64_t N = 0;
  for (uint32_t *x = __edges_start; x < __edges_stop && N < MAX_EDGES; x++)
    *x = ++N;
}

extern "C" void __sanitizer_cov_trace_pc_guard_init(
    uint32_t *start,
    uint32_t *stop) {
  // Avoid duplicate initialization
  if (start == stop || *start)
    return;

  if (__edges_start != nullptr || __edges_stop != nullptr) {
    fprintf(
        stderr,
        "Coverage instrumentation is only supported for a single module\n");
    _exit(-1);
  }

  __edges_start = start;
  __edges_stop = stop;

  // Map the shared memory region
  const char *shm_key = getenv("SHM_ID");
  if (!shm_key) {
    puts("[COV] no shared memory bitmap available, skipping");
    shmem = (struct SharedMemData *)malloc(SHM_SIZE);
  } else {
    int fd = shm_open(shm_key, O_RDWR, S_IREAD | S_IWRITE);
    if (fd <= -1) {
      fprintf(
          stderr, "Failed to open shared memory region: %s\n", strerror(errno));
      _exit(-1);
    }

    shmem = (struct SharedMemData *)mmap(
        nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmem == MAP_FAILED) {
      fprintf(stderr, "Failed to mmap shared memory region\n");
      _exit(-1);
    }
  }

  __sanitizer_cov_reset_edgeguards();

  shmem->numEdges = stop - start;
  printf(
      "[COV] edge counters initialized. Shared memory: %s with %u edges\n",
      shm_key,
      shmem->numEdges);
}

extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
  // There's a small race condition here: if this function executes in two
  // threads for the same edge at the same time, the first thread might disable
  // the edge (by setting the guard to zero) before the second thread fetches
  // the guard value (and thus the index). However, our instrumentation ignores
  // the first edge (see libcoverage.c) and so the race is unproblematic.
  uint32_t index = *guard;
  // If this function is called before coverage instrumentation is properly
  // initialized we want to return early.
  if (!index)
    return;
  shmem->edges[index / 8] |= 1 << (index % 8);
  *guard = 0;
}

//
// END FUZZING CODE
//

Value CrashFunction(
    Runtime &rt,
    const Value &thisArg,
    const Value *args,
    size_t numArgs) {
  // crashes when calling FuzzilliCrash(1) and FuzzilliCrash(2)
  if (numArgs != 2 || !args[0].isString())
    return Value();
  String funcString = args[0].getString(rt);
  if (String::strictEquals(
          rt, funcString, String::createFromAscii(rt, "FUZZILLI_PRINT"))) {
    // The Fuzzilli Print operation can take either a number or a string
    static FILE *fzliout = fdopen(REPRL_DWFD, "w");
    if (!fzliout) {
      fprintf(
          stderr,
          "Fuzzer output channel not available, printing to stdout instead\n");
      fzliout = stdout;
    }
    if (args[1].isNumber()) {
      double num = args[1].asNumber();
      fprintf(fzliout, "%.0f\n", num);
    } else if (args[1].isString()) {
      std::string outputString = args[1].getString(rt).utf8(rt);
      fprintf(fzliout, "%s\n", outputString.c_str());
    }
    fflush(fzliout);
  } else if (
      String::strictEquals(
          rt, funcString, String::createFromAscii(rt, "FUZZILLI_CRASH")) &&
      args[1].isNumber()) {
    double crashCode = args[1].getNumber();
    if (crashCode == 0.0) {
      *((int *)0x1) = 2;
      exit(-1);
    } else if (crashCode == 1.0) {
      assert(1 == 0);
    }
  }
  return Value();
}

class MemoryBuffer : public facebook::jsi::Buffer {
 public:
  MemoryBuffer(const uint8_t *buf, size_t size) : buf_(buf), size_(size) {}
  size_t size() const override {
    return size_;
  }
  const uint8_t *data() const override {
    return buf_;
  }

 private:
  const uint8_t *buf_;
  size_t size_;
};

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--replr") != 0)
    return 0;
  // get fuzzer arguments
  char helo[] = "HELO";
  if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
    printf("Invalid HELO response from parent\n");
    exit(-1);
  }

  if (memcmp(helo, "HELO", 4) != 0) {
    printf("Invalid response from parent\n");
    exit(-1);
  }
  // fuzzing loop
  while (true) {
    auto runtime = makeHermesRuntime(
        ::hermes::vm::RuntimeConfig::Builder()
            .withES6Proxy(true)
            .withEnableGenerator(true)
            .withEnableHermesInternal(true)
            .build());
    auto crashFunctionName = "fuzzilli";
    auto crashFunctionProp =
        facebook::jsi::PropNameID::forAscii(*runtime, crashFunctionName);
    auto crashFunctionDef = Function::createFromHostFunction(
        *runtime, crashFunctionProp, 1, CrashFunction);
    runtime->global().setProperty(
        *runtime, crashFunctionName, crashFunctionDef);
    size_t script_size = 0;
    unsigned action;
    CHECK(read(REPRL_CRFD, &action, 4) == 4);
    if (action == 'cexe') {
      CHECK(read(REPRL_CRFD, &script_size, 8) == 8);
    } else {
      printf("Unknown Action\n");
      exit(-1);
    }
    uint8_t *script_src = (uint8_t *)(malloc(script_size + 1));
    if (script_src == nullptr)
      return 0;
    uint8_t *ptr = script_src;
    size_t remaining = script_size;
    while (remaining > 0) {
      ssize_t rv = read(REPRL_DRFD, ptr, remaining);
      if (rv <= 0) {
        fprintf(stderr, "Failed to load script\n");
        exit(-1);
      }
      remaining -= rv;
      ptr += rv;
    }
    script_src[script_size] = '\0';
    bool exceptionThrew(false);
    try {
      runtime->evaluateJavaScript(
          std::make_unique<MemoryBuffer>(script_src, script_size), "");
    } catch (const JSIException &) {
      exceptionThrew = true;
    }
    free(script_src);
    fflush(stdout);
    fflush(stderr);
    auto status = ((exceptionThrew ? 1 : 0) & 0xff) << 8;
    CHECK(write(REPRL_CWFD, &status, 4) == 4);
    __sanitizer_cov_reset_edgeguards();
  }
}
