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

  \file sycl_compile.cpp

\*****************************************************************************/

#include "sycl_compile.h"
#include "binary_result.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/VirtualFileSystem.h"

#include "llvm/SYCLLowerIR/SYCLJointMatrixTransform.h"
#include "llvm/SYCLPostLink/ComputeModuleRuntimeInfo.h"
#include "llvm/SYCLPostLink/ModuleSplitter.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/FrontendTool/Utils.h"

#ifdef USE_PREBUILT_LLVM
#include "LLVMSPIRVLib/LLVMSPIRVLib.h"
#else
#include "LLVMSPIRVLib.h"
#endif

#include <sstream>
#include <string>
#include <vector>

// Error codes (same as OpenCL)
#define SYCL_SUCCESS 0
#define SYCL_COMPILE_FAILURE -15
#define SYCL_INVALID_OPTIONS -43
#define SYCL_OUT_OF_HOST_MEMORY -6

using namespace Intel::OpenCL::ClangFE;

// Forward declaration from opencl_clang.cpp
void OpenCLClangInitialize();

namespace {

class SmallVectorBuffer : public std::streambuf {
  llvm::SmallVectorImpl<char> &OS;

  std::streamsize xsputn(const char *s, std::streamsize n) override {
    OS.append(s, s + n);
    return n;
  }

public:
  SmallVectorBuffer(llvm::SmallVectorImpl<char> &O) : OS(O) {}
};

/// Run a single LLVM module pass.
template <class PassClass> static bool runModulePass(llvm::Module &M) {
  llvm::ModulePassManager MPM;
  llvm::ModuleAnalysisManager MAM;
  MAM.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });
  MPM.addPass(PassClass{});
  llvm::PreservedAnalyses Res = MPM.run(M, MAM);
  return !Res.areAllPreserved();
}

/// Build the SPIR-V translator options with Intel SYCL extensions enabled.
static SPIRV::TranslatorOpts getSYCLSPIRVTranslatorOpts() {
  SPIRV::TranslatorOpts Opts(SPIRV::VersionNumber::SPIRV_1_5);
  Opts.enableGenArgNameMD();
  Opts.setMemToRegEnabled(true);
  Opts.setDesiredBIsRepresentation(
      SPIRV::BIsRepresentation::SPIRVFriendlyIR);
  Opts.setDebugInfoEIS(
      SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_200);
  Opts.setSPIRVAllowUnknownIntrinsics({"llvm.genx."});

  // Enable SYCL-relevant SPIR-V extensions
  std::vector<SPIRV::ExtensionID> Extensions = {
      SPIRV::ExtensionID::SPV_EXT_shader_atomic_float_add,
      SPIRV::ExtensionID::SPV_EXT_shader_atomic_float_min_max,
      SPIRV::ExtensionID::SPV_KHR_no_integer_wrap_decoration,
      SPIRV::ExtensionID::SPV_KHR_float_controls,
      SPIRV::ExtensionID::SPV_KHR_expect_assume,
      SPIRV::ExtensionID::SPV_KHR_linkonce_odr,
      SPIRV::ExtensionID::SPV_INTEL_subgroups,
      SPIRV::ExtensionID::SPV_INTEL_media_block_io,
      SPIRV::ExtensionID::SPV_INTEL_device_side_avc_motion_estimation,
      SPIRV::ExtensionID::SPV_INTEL_fpga_loop_controls,
      SPIRV::ExtensionID::SPV_INTEL_unstructured_loop_controls,
      SPIRV::ExtensionID::SPV_INTEL_fpga_reg,
      SPIRV::ExtensionID::SPV_INTEL_blocking_pipes,
      SPIRV::ExtensionID::SPV_INTEL_function_pointers,
      SPIRV::ExtensionID::SPV_INTEL_kernel_attributes,
      SPIRV::ExtensionID::SPV_INTEL_io_pipes,
      SPIRV::ExtensionID::SPV_INTEL_inline_assembly,
      SPIRV::ExtensionID::SPV_INTEL_arbitrary_precision_integers,
      SPIRV::ExtensionID::SPV_INTEL_float_controls2,
      SPIRV::ExtensionID::SPV_INTEL_vector_compute,
      SPIRV::ExtensionID::SPV_INTEL_arbitrary_precision_fixed_point,
      SPIRV::ExtensionID::SPV_INTEL_arbitrary_precision_floating_point,
      SPIRV::ExtensionID::SPV_INTEL_variable_length_array,
      SPIRV::ExtensionID::SPV_INTEL_fp_fast_math_mode,
      SPIRV::ExtensionID::SPV_INTEL_long_composites,
      SPIRV::ExtensionID::SPV_INTEL_arithmetic_fence,
      SPIRV::ExtensionID::SPV_INTEL_global_variable_decorations,
      SPIRV::ExtensionID::SPV_INTEL_cache_controls,
      SPIRV::ExtensionID::SPV_INTEL_fpga_buffer_location,
      SPIRV::ExtensionID::SPV_INTEL_fpga_argument_interfaces,
      SPIRV::ExtensionID::SPV_INTEL_fpga_invocation_pipelining_attributes,
      SPIRV::ExtensionID::SPV_INTEL_fpga_latency_control,
      SPIRV::ExtensionID::SPV_KHR_shader_clock,
      SPIRV::ExtensionID::SPV_INTEL_bindless_images,
      SPIRV::ExtensionID::SPV_INTEL_task_sequence,
      SPIRV::ExtensionID::SPV_INTEL_bfloat16_conversion,
      SPIRV::ExtensionID::SPV_INTEL_joint_matrix,
      SPIRV::ExtensionID::SPV_INTEL_hw_thread_queries,
      SPIRV::ExtensionID::SPV_KHR_uniform_group_instructions,
      SPIRV::ExtensionID::SPV_INTEL_masked_gather_scatter,
      SPIRV::ExtensionID::SPV_INTEL_tensor_float32_conversion,
      SPIRV::ExtensionID::SPV_INTEL_optnone,
      SPIRV::ExtensionID::SPV_KHR_non_semantic_info,
      SPIRV::ExtensionID::SPV_KHR_cooperative_matrix,
      SPIRV::ExtensionID::SPV_EXT_shader_atomic_float16_add,
      SPIRV::ExtensionID::SPV_INTEL_fp_max_error,
  };
  for (auto &Ext : Extensions)
    Opts.setAllowedToUseExtension(Ext, true);

  return Opts;
}

/// Detect system GCC C++ include paths at runtime.
/// Mirrors clang::driver::toolchains::Generic_GCC::addGCCLibStdCxxIncludePaths
/// which adds -internal-isystem for:
///   1. /usr/include/c++/<ver>
///   2. /usr/include/<triple>/c++/<ver>  (Debian multiarch)
///   3. /usr/include/c++/<ver>/backward
/// See clang/lib/Driver/ToolChains/Gnu.cpp:addLibStdCXXIncludePaths()
/// and clang/lib/Driver/ToolChain.cpp:addSystemInclude() which emits
/// -internal-isystem.
static void addSystemCXXIncludePaths(std::vector<std::string> &Args) {
  namespace fs = llvm::sys::fs;
  namespace path = llvm::sys::path;

  // Scan /usr/include/c++/ for the highest GCC version directory
  std::string BestVersion;
  std::string BaseDir = "/usr/include/c++";
  std::error_code EC;
  for (fs::directory_iterator Dir(BaseDir, EC), End; !EC && Dir != End;
       Dir.increment(EC)) {
    llvm::StringRef DirName = path::filename(Dir->path());
    if (BestVersion.empty() || DirName > BestVersion)
      BestVersion = DirName.str();
  }
  if (BestVersion.empty())
    return;

  // GPLUSPLUS_INCLUDE_DIR
  Args.push_back("-internal-isystem");
  Args.push_back(BaseDir + "/" + BestVersion);

  // GPLUSPLUS_TOOL_INCLUDE_DIR (Debian multiarch)
  Args.push_back("-internal-isystem");
  Args.push_back("/usr/include/x86_64-linux-gnu/c++/" + BestVersion);

  // GPLUSPLUS_BACKWARD_INCLUDE_DIR
  Args.push_back("-internal-isystem");
  Args.push_back(BaseDir + "/" + BestVersion + "/backward");

  // GCC internal headers
  std::string GccInternal =
      "/usr/lib/gcc/x86_64-linux-gnu/" + BestVersion + "/include";
  if (fs::is_directory(GccInternal)) {
    Args.push_back("-internal-isystem");
    Args.push_back(GccInternal);
  }

  // System include dirs
  Args.push_back("-internal-isystem");
  Args.push_back("/usr/local/include");
  Args.push_back("-internal-isystem");
  Args.push_back("/usr/include/x86_64-linux-gnu");
  Args.push_back("-internal-isystem");
  Args.push_back("/usr/include");
}

/// Build the Clang cc1-level arguments for SYCL device compilation.
/// Target: spir64 (SPIR-V), EmitLLVMOnly action to get an llvm::Module.
/// Note: these are cc1 flags (not driver flags) since we call
/// CompilerInvocation::CreateFromArgs directly, bypassing the driver.
static std::vector<std::string>
buildSYCLCompileArgs(const char *pszOptions, const char *pszOptionsEx,
                     const char *sourceName) {
  std::vector<std::string> Args;

  // cc1 mode — no argv[0] needed for CreateFromArgs

  // Language: C++ with SYCL device mode
  Args.push_back("-x");
  Args.push_back("c++");
  Args.push_back("-fsycl-is-device");
  Args.push_back("-fno-sycl-instrument-device-code");

  // Target triple for SPIR-V 64-bit
  Args.push_back("-triple");
  Args.push_back("spir64-unknown-unknown");

  // Emit LLVM IR only (no codegen)
  Args.push_back("-emit-llvm");

  // Disable LLVM passes — we'll do post-link ourselves
  Args.push_back("-disable-llvm-passes");

  // C++ standard
  Args.push_back("-std=c++17");

  // SYCL runtime headers
#ifdef SYCL_INCLUDE_DIR
  Args.push_back("-isystem");
  Args.push_back(SYCL_INCLUDE_DIR);
#endif

  // Generated SYCL headers (feature_test.hpp, etc.)
#ifdef SYCL_GENERATED_INCLUDE_DIR
  Args.push_back("-isystem");
  Args.push_back(SYCL_GENERATED_INCLUDE_DIR);
#endif

  // Clang resource headers (stddef.h, etc.)
#ifdef CLANG_RESOURCE_DIR
  Args.push_back("-internal-isystem");
  Args.push_back(CLANG_RESOURCE_DIR);
#endif

  // System C++ standard library headers (runtime-detected GCC paths)
  addSystemCXXIncludePaths(Args);

  // Append user options
  if (pszOptions && pszOptions[0] != '\0') {
    // Simple splitting of user options by spaces
    // (does not handle quoted strings — production code should use a proper
    // argument parser)
    std::string optsStr(pszOptions);
    std::istringstream iss(optsStr);
    std::string tok;
    while (iss >> tok)
      Args.push_back(tok);
  }

  // Append extra options
  if (pszOptionsEx && pszOptionsEx[0] != '\0') {
    std::string optsStr(pszOptionsEx);
    std::istringstream iss(optsStr);
    std::string tok;
    while (iss >> tok)
      Args.push_back(tok);
  }

  // Source file
  Args.push_back(sourceName);

  return Args;
}

/// Translate an LLVM Module to SPIR-V binary, appending to the output buffer.
static bool translateToSPIRV(llvm::Module &M,
                             llvm::SmallVectorImpl<char> &OutputBuffer,
                             std::string &ErrLog) {
  SPIRV::TranslatorOpts SPIRVOpts = getSYCLSPIRVTranslatorOpts();

  SmallVectorBuffer StreamBuf(OutputBuffer);
  std::ostream OS(&StreamBuf);
  std::string Err;

  bool Success = llvm::writeSpirv(&M, SPIRVOpts, OS, Err);
  if (!Success)
    ErrLog += Err;

  return Success;
}

/// Run SYCL post-link passes on the module and translate each resulting
/// split to SPIR-V. The first split's SPIR-V is placed in OutputBuffer.
/// Returns true on success.
static bool runPostLinkAndTranslate(std::unique_ptr<llvm::Module> Module,
                                    llvm::SmallVectorImpl<char> &OutputBuffer,
                                    std::string &ErrLog) {
  // Transform Joint Matrix builtin calls
  runModulePass<llvm::SYCLJointMatrixTransformPass>(*Module);

  // Split by device code split mode (default: auto)
  auto Splitter = llvm::module_split::getDeviceCodeSplitter(
      std::make_unique<llvm::module_split::ModuleDesc>(std::move(Module)),
      llvm::module_split::SPLIT_AUTO,
      /*IROutputOnly=*/false,
      /*EmitOnlyKernelsAsEntryPoints=*/true,
      /*AllowDeviceImageDependencies=*/false);

  if (!Splitter->hasMoreSplits()) {
    ErrLog += "error: no device code found after splitting\n";
    return false;
  }

  if (auto Err = Splitter->verifyNoCrossModuleDeviceGlobalUsage()) {
    llvm::raw_string_ostream OS(ErrLog);
    OS << Err;
    llvm::consumeError(std::move(Err));
    return false;
  }

  bool FirstSplit = true;
  while (Splitter->hasMoreSplits()) {
    auto MDesc = Splitter->nextSplit();
    MDesc->saveSplitInformationAsMetadata();

    // Translate to SPIR-V
    // For the first split, write to the main output buffer.
    // For subsequent splits, we still translate but only keep the first
    // (the caller can extend this to handle multiple device images).
    if (FirstSplit) {
      if (!translateToSPIRV(MDesc->getModule(), OutputBuffer, ErrLog))
        return false;
      FirstSplit = false;
    } else {
      llvm::SmallVector<char, 4096> ExtraBuf;
      if (!translateToSPIRV(MDesc->getModule(), ExtraBuf, ErrLog))
        return false;
      // Additional splits are translated but not returned to caller.
      // TODO: extend API to return multiple device images if needed.
    }
  }

  return !FirstSplit; // at least one split was produced
}

} // anonymous namespace

extern "C" CC_DLL_EXPORT int
CompileSYCL(const char *pszProgramSource, const char **pInputHeaders,
            unsigned int uiNumInputHeaders, const char **pInputHeadersNames,
            const char *pszOptions, const char *pszOptionsEx,
            IOCLFEBinaryResult **pBinaryResult) {

  OpenCLClangInitialize();

  try {
    std::unique_ptr<OCLFEBinaryResult> pResult(new OCLFEBinaryResult());
    llvm::raw_string_ostream err_ostream(pResult->getLogRef());

    const char *sourceName = "sycl_input.cpp";

    // ------------------------------------------------------------------
    // Phase 1: Clang frontend — compile SYCL source to LLVM IR Module
    // ------------------------------------------------------------------
    llvm::LLVMContext Context;

    // Set up LLVM diagnostic handler to capture diagnostics
    Context.setDiagnosticHandler(
        std::make_unique<llvm::DiagnosticHandler>());

    // Build Clang arguments
    auto ArgsVec = buildSYCLCompileArgs(pszOptions, pszOptionsEx, sourceName);
    std::vector<const char *> ArgsCStr;
    ArgsCStr.reserve(ArgsVec.size());
    for (const auto &A : ArgsVec)
      ArgsCStr.push_back(A.c_str());

    // Prepare diagnostics
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagID(
        new clang::DiagnosticIDs());
    clang::DiagnosticOptions DiagOpts;
    DiagOpts.ShowPresumedLoc = true;
    clang::TextDiagnosticPrinter *DiagsPrinter =
        new clang::TextDiagnosticPrinter(err_ostream, DiagOpts);
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> Diags(
        new clang::DiagnosticsEngine(DiagID, DiagOpts, DiagsPrinter));

    // Create CompilerInstance
    auto compiler = std::make_unique<clang::CompilerInstance>();
    compiler->setDiagnostics(&*Diags);

    // Set up virtual file system with source and headers
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFS(
        new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS(
        new llvm::vfs::InMemoryFileSystem);
    OverlayFS->pushOverlay(MemFS);

    compiler->setVirtualFileSystem(std::move(OverlayFS));
    compiler->createFileManager();
    compiler->createSourceManager();

    // Map the SYCL source into the VFS
    MemFS->addFile(sourceName, (time_t)0,
                   llvm::MemoryBuffer::getMemBuffer(
                       llvm::StringRef(pszProgramSource), sourceName));

    // Map user-provided headers
    for (unsigned int i = 0; i < uiNumInputHeaders; ++i) {
      MemFS->addFile(pInputHeadersNames[i], (time_t)0,
                     llvm::MemoryBuffer::getMemBuffer(pInputHeaders[i],
                                                      pInputHeadersNames[i]));
    }

    // Create CompilerInvocation from our arguments
    llvm::ArrayRef<const char *> ArgsRef(ArgsCStr);
    clang::CompilerInvocation::CreateFromArgs(compiler->getInvocation(),
                                              ArgsRef, *Diags);

    // Use EmitLLVMOnlyAction to get an in-memory LLVM Module
    clang::EmitLLVMOnlyAction EmitAction(&Context);
    bool FrontendSuccess = false;
    try {
      FrontendSuccess = compiler->ExecuteAction(EmitAction);
    } catch (const std::exception &) {
      FrontendSuccess = false;
    }

    err_ostream.flush();

    if (!FrontendSuccess) {
      pResult->setIRType(IR_TYPE_COMPILED_OBJECT);
      pResult->setIRName(sourceName);
      if (pBinaryResult)
        *pBinaryResult = pResult.release();
      return SYCL_COMPILE_FAILURE;
    }

    std::unique_ptr<llvm::Module> Module = EmitAction.takeModule();
    if (!Module) {
      err_ostream << "error: failed to obtain LLVM module from frontend\n";
      err_ostream.flush();
      if (pBinaryResult)
        *pBinaryResult = pResult.release();
      return SYCL_COMPILE_FAILURE;
    }

    // ------------------------------------------------------------------
    // Phase 2: SYCL post-link passes + SPIR-V translation
    // ------------------------------------------------------------------
    std::string postLinkLog;
    bool success = runPostLinkAndTranslate(std::move(Module),
                                           pResult->getIRBufferRef(),
                                           postLinkLog);
    if (!postLinkLog.empty())
      err_ostream << postLinkLog;
    err_ostream.flush();

    pResult->setIRType(IR_TYPE_COMPILED_OBJECT);
    pResult->setIRName(sourceName);

    if (pBinaryResult)
      *pBinaryResult = pResult.release();

    return success ? SYCL_SUCCESS : SYCL_COMPILE_FAILURE;

  } catch (std::bad_alloc &) {
    if (pBinaryResult)
      *pBinaryResult = nullptr;
    return SYCL_OUT_OF_HOST_MEMORY;
  }
}
