/*****************************************************************************\

Copyright (c) Intel Corporation (2009-2026).

  \file embedded_headers.h

  Declare the API for populating a clang InMemoryFileSystem from headers
  that are embedded (via .incbin) in the shared library.

\*****************************************************************************/

#ifndef EMBEDDED_HEADERS_H
#define EMBEDDED_HEADERS_H

namespace llvm {
namespace vfs {
class InMemoryFileSystem;
}
} // namespace llvm

/// Populate an InMemoryFileSystem with all embedded SYCL, libc++, and
/// clang resource headers.  Returns the number of files added.
int populateEmbeddedHeaders(llvm::vfs::InMemoryFileSystem &FS);

#endif // EMBEDDED_HEADERS_H
