This directory contains a slightly modified partial copy of Boost needed by Boost.Context.

1. Downloaded https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.
bz2.
2. Copied only the parts needed by Boost.Context.
3. Deleted `boost/context/continuation*.hpp` since only `fiber` is needed.
4. Deleted `boost/context/pooled_fixedsize_stack.hpp` because of its 
   dependency on `intrusive_ptr`.
5. Removed unused include of `intrusive_ptr.hpp` from 
   `boost/context/fiber_fcontext.hpp`.
6. Minimal updates to `libs/context/CMakeLists.txt` to remove other library 
   dependencies, remove library alias, support `CMAKE_OSX_ARCHITECTURES`
7. Rename the `boost` namespace to `hoost` using a preprocessor define.
8. Rename the `xxx_fcontext()` globals to have a `hoost_` prefix.
9. Change the include path from `boost/` to `hoost/`.
10. Add a BTI landing pad (`hint #34`) at each `hoost_*_fcontext` entry and a
    `GNU_PROPERTY_AARCH64_FEATURE_1_BTI` note to the AArch64 ELF assembly
    (`*_arm64_aapcs_elf_gas.S`), both only when `__ARM_FEATURE_BTI_DEFAULT` is
    set (`-mbranch-protection=bti` or `standard`).
