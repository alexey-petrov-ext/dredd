Third-party code in this package
================================

bin/dredd.exe statically links the following libraries. lib/clang/*/include
contains Clang's built-in headers.

  LLVM and Clang   Apache License v2.0 with LLVM Exceptions
                   https://llvm.org - see LLVM-LICENSE.TXT

  Protocol Buffers BSD 3-Clause License
                   https://github.com/protocolbuffers/protobuf
                   - see protobuf-LICENSE

The Microsoft C/C++ runtime is linked statically (/MT).

Dredd itself is licensed under the Apache License, Version 2.0; see LICENSE
at the top of this package.
