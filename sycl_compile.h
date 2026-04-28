/*****************************************************************************\

Copyright (c) Intel Corporation (2009-2026).

    INTEL MAKES NO WARRANTY OF ANY KIND REGARDING THE CODE.  THIS CODE IS
    LICENSED ON AN "AS IS" BASIS AND INTEL WILL NOT PROVIDE ANY SUPPORT,
    ASSISTANCE, INSTALLATION, TRAINING OR OTHER SERVICES.  INTEL DOES NOT
    PROVIDE ANY UPDATES, ENHANCEMENTS OR EXTENSIONS.  INTEL SPECIFICALLY
    DISCLAIMS ANY WARRANTY OF MERCHANTABILITY, NONINFRINGEMENT, FITNESS FOR ANY
    PARTICULAR PURPOSE, OR ANY OTHER WARRANTY.  Intel disclaims all liability,
    including liability for infringement of any proprietary rights, relating to
    use of the code. No license, express or implied, by estoppel or otherwise,
    to any intellectual property rights is granted herein.

  \file sycl_compile.h

\*****************************************************************************/

#pragma once
#include "opencl_clang.h"

//
// Compiles the given SYCL program source to SPIR-V binary.
//
// This function provides a SYCL source-to-SPIR-V compilation path by
// leveraging the Clang frontend (with -fsycl-device-only), SYCL-specific
// LLVM passes (SYCLLowerIR, SYCLPostLink), device library linking, and
// SPIR-V translation via LLVMSPIRVLib.
//
// Params:
//    pszProgramSource - SYCL source program to compile (null terminated)
//    pInputHeaders - array of additional header buffers (each null terminated)
//    uiNumInputHeaders - size of the pInputHeaders array
//    pszInputHeadersNames - array of header names corresponding to pInputHeaders
//    pszOptions - user-supplied compilation options (e.g. "-O2 -DFOO")
//    pszOptionsEx - optional extra options string, may be NULL
//    pBinaryResult - outbound pointer to the compilation results
//
// Returns:
//    0 on success, negative error code on failure.
//    On success, *pBinaryResult contains the SPIR-V binary and any
//    diagnostics. The caller must release the result via Release().
//
extern "C" CC_DLL_EXPORT int CompileSYCL(
    // A pointer to main program's SYCL source (null terminated string)
    const char *pszProgramSource,
    // array of additional input headers to be passed in memory
    const char **pInputHeaders,
    // the number of input headers in pInputHeaders
    unsigned int uiNumInputHeaders,
    // array of input headers names corresponding to pInputHeaders
    const char **pInputHeadersNames,
    // user-supplied compilation options string
    const char *pszOptions,
    // optional extra options string, may be NULL
    const char *pszOptionsEx,
    // outbound pointer to the compilation results
    Intel::OpenCL::ClangFE::IOCLFEBinaryResult **pBinaryResult);
