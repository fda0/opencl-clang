/*****************************************************************************\

Copyright (c) Intel Corporation (2009-2026).

  \file embedded_headers.cpp

  Parse the binary blob produced by pack_headers.py (linked via .incbin
  in sycl_headers_blob.S) and populate an llvm InMemoryFileSystem.

\*****************************************************************************/

#include "embedded_headers.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cstdint>

// Symbols defined by the assembly file (sycl_headers_blob.S).
// The blob lives in .rodata and is valid for the lifetime of the process.
extern "C" {
extern const unsigned char sycl_headers_blob[];
extern const unsigned char sycl_headers_blob_end[];
}

static uint32_t readU32LE(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int populateEmbeddedHeaders(llvm::vfs::InMemoryFileSystem &FS) {
  const unsigned char *ptr = sycl_headers_blob;
  const unsigned char *end = sycl_headers_blob_end;

  if (ptr + 4 > end)
    return 0;

  uint32_t numFiles = readU32LE(ptr);
  ptr += 4;

  int count = 0;
  for (uint32_t i = 0; i < numFiles && ptr < end; ++i) {
    if (ptr + 4 > end)
      break;
    uint32_t pathLen = readU32LE(ptr);
    ptr += 4;

    if (ptr + pathLen > end)
      break;
    std::string path(reinterpret_cast<const char *>(ptr), pathLen);
    ptr += pathLen;

    if (ptr + 4 > end)
      break;
    uint32_t contentLen = readU32LE(ptr);
    ptr += 4;

    if (ptr + contentLen > end)
      break;
    // Create a non-owning MemoryBuffer that points into the .rodata blob.
    // The blob is valid for the lifetime of the .so, so the buffer content
    // will remain valid.
    llvm::StringRef content(reinterpret_cast<const char *>(ptr), contentLen);
    ptr += contentLen;

    FS.addFile(path, /*ModificationTime=*/0,
               llvm::MemoryBuffer::getMemBuffer(content, path,
                                                 /*RequiresNullTerminator=*/false));
    ++count;
  }

  return count;
}
