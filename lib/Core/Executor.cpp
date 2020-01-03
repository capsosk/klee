//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"

#include "../Expr/ArrayExprOptimizer.h"
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"

#include "klee/Common.h"
#include "klee/Config/Version.h"
#include "klee/ExecutionState.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprSMTLIBPrinter.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Support/FileHandling.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/System/MemoryUsage.h"
#include "klee/Internal/System/Time.h"
#include "klee/Interpreter.h"
#include "klee/OptionCategories.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/SolverStats.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/GetElementPtrTypeIterator.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cxxabi.h>
#include <fstream>
#include <iomanip>
#include <iosfwd>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

using namespace llvm;
using namespace klee;

namespace klee {
cl::OptionCategory DebugCat("Debugging options",
                            "These are debugging options.");

cl::OptionCategory ExtCallsCat("External call policy options",
                               "These options impact external calls.");

cl::OptionCategory SeedingCat(
    "Seeding options",
    "These options are related to the use of seeds to start exploration.");

cl::OptionCategory
    TerminationCat("State and overall termination options",
                   "These options control termination of the overall KLEE "
                   "execution and of individual states.");

cl::OptionCategory TestGenCat("Test generation options",
                              "These options impact test generation.");

cl::opt<std::string> MaxTime(
    "max-time",
    cl::desc("Halt execution after the specified duration.  "
             "Set to 0s to disable (default=0s)"),
    cl::init("0s"),
    cl::cat(TerminationCat));
} // namespace klee

namespace {

/*** Test generation options ***/

cl::opt<bool> DumpStatesOnHalt(
    "dump-states-on-halt",
    cl::init(true),
    cl::desc("Dump test cases for all active states on exit (default=true)"),
    cl::cat(TestGenCat));

cl::opt<bool> OnlyOutputStatesCoveringNew(
    "only-output-states-covering-new",
    cl::init(false),
    cl::desc("Only output test cases covering new code (default=false)"),
    cl::cat(TestGenCat));

cl::opt<bool> EmitAllErrors(
    "emit-all-errors", cl::init(false),
    cl::desc("Generate tests cases for all errors "
             "(default=false, i.e. one per (error,instruction) pair)"),
    cl::cat(TestGenCat));

cl::opt<bool> CheckLeaks(
    "check-leaks", cl::init(false),
    cl::desc("Check for memory leaks"),
    cl::cat(TestGenCat));

cl::opt<bool> CheckMemCleanup(
    "check-memcleanup", cl::init(false),
    cl::desc("Check for memory cleanup"),
    cl::cat(TestGenCat));


/* Constraint solving options */

cl::opt<unsigned> MaxSymArraySize(
    "max-sym-array-size",
    cl::desc(
        "If a symbolic array exceeds this size (in bytes), symbolic addresses "
        "into this array are concretized.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(SolvingCat));

cl::opt<bool>
    SimplifySymIndices("simplify-sym-indices",
                       cl::init(false),
                       cl::desc("Simplify symbolic accesses using equalities "
                                "from other constraints (default=false)"),
                       cl::cat(SolvingCat));

cl::opt<bool>
    EqualitySubstitution("equality-substitution", cl::init(true),
                         cl::desc("Simplify equality expressions before "
                                  "querying the solver (default=true)"),
                         cl::cat(SolvingCat));


/*** External call policy options ***/

enum class ExternalCallPolicy {
  None,     // No external calls allowed
  Pure,     // All external calls are taken as having no side-effects and
            // returning nondet value
  Concrete, // Only external calls with concrete arguments allowed
  All,      // All external calls allowed
};

cl::opt<ExternalCallPolicy> ExternalCalls(
    "external-calls",
    cl::desc("Specify the external call policy"),
    cl::values(
        clEnumValN(
            ExternalCallPolicy::None, "none",
            "No external function calls are allowed.  Note that KLEE always "
            "allows some external calls with concrete arguments to go through "
            "(in particular printf and puts), regardless of this option."),
        clEnumValN(ExternalCallPolicy::Pure, "pure",
                   "Allow all external function calls but assume that they have "
                   "no side-effects and return nondet values"),
        clEnumValN(ExternalCallPolicy::Concrete, "concrete",
                   "Only external function calls with concrete arguments are "
                   "allowed (default)"),
        clEnumValN(ExternalCallPolicy::All, "all",
                   "All external function calls are allowed.  This concretizes "
                   "any symbolic arguments in calls to external functions.")
            KLEE_LLVM_CL_VAL_END),
    cl::init(ExternalCallPolicy::Concrete),
    cl::cat(ExtCallsCat));

cl::opt<bool> SuppressExternalWarnings(
    "suppress-external-warnings",
    cl::init(false),
    cl::desc("Supress warnings about calling external functions."),
    cl::cat(ExtCallsCat));

cl::opt<bool> AllExternalWarnings(
    "all-external-warnings",
    cl::init(false),
    cl::desc("Issue a warning everytime an external call is made, "
             "as opposed to once per function (default=false)"),
    cl::cat(ExtCallsCat));


/*** Seeding options ***/

cl::opt<bool> AlwaysOutputSeeds(
    "always-output-seeds",
    cl::init(true),
    cl::desc(
        "Dump test cases even if they are driven by seeds only (default=true)"),
    cl::cat(SeedingCat));

cl::opt<bool> OnlyReplaySeeds(
    "only-replay-seeds",
    cl::init(false),
    cl::desc("Discard states that do not have a seed (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> OnlySeed("only-seed",
                       cl::init(false),
                       cl::desc("Stop execution after seeding is done without "
                                "doing regular search (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool>
    AllowSeedExtension("allow-seed-extension",
                       cl::init(false),
                       cl::desc("Allow extra (unbound) values to become "
                                "symbolic during seeding (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool> ZeroSeedExtension(
    "zero-seed-extension",
    cl::init(false),
    cl::desc(
        "Use zero-filled objects if matching seed not found (default=false)"),
    cl::cat(SeedingCat));

cl::opt<bool> AllowSeedTruncation(
    "allow-seed-truncation",
    cl::init(false),
    cl::desc("Allow smaller buffers than in seeds (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> NamedSeedMatching(
    "named-seed-matching",
    cl::init(false),
    cl::desc("Use names to match symbolic objects to inputs (default=false)."),
    cl::cat(SeedingCat));

cl::opt<std::string>
    SeedTime("seed-time",
             cl::desc("Amount of time to dedicate to seeds, before normal "
                      "search (default=0s (off))"),
             cl::cat(SeedingCat));


/*** Termination criteria options ***/

cl::list<Executor::TerminateReason> ExitOnErrorType(
    "exit-on-error-type",
    cl::desc(
        "Stop execution after reaching a specified condition (default=false)"),
    cl::values(
        clEnumValN(Executor::Abort, "Abort", "The program crashed"),
        clEnumValN(Executor::Assert, "Assert", "An assertion was hit"),
        clEnumValN(Executor::BadVectorAccess, "BadVectorAccess",
                   "Vector accessed out of bounds"),
        clEnumValN(Executor::Exec, "Exec",
                   "Trying to execute an unexpected instruction"),
        clEnumValN(Executor::External, "External",
                   "External objects referenced"),
        clEnumValN(Executor::Free, "Free", "Freeing invalid memory"),
        clEnumValN(Executor::Leak, "Leak", "Leaking heap-allocated memory"),
        clEnumValN(Executor::Model, "Model", "Memory model limit hit"),
        clEnumValN(Executor::Overflow, "Overflow", "An overflow occurred"),
        clEnumValN(Executor::Ptr, "Ptr", "Pointer error"),
        clEnumValN(Executor::ReadOnly, "ReadOnly", "Write to read-only memory"),
        clEnumValN(Executor::ReportError, "ReportError",
                   "klee_report_error called"),
        clEnumValN(Executor::User, "User", "Wrong klee_* functions invocation"),
        clEnumValN(Executor::Unhandled, "Unhandled",
                   "Unhandled instruction hit") KLEE_LLVM_CL_VAL_END),
    cl::ZeroOrMore,
    cl::cat(TerminationCat));

cl::opt<std::string>
    ErrorFun("error-fn",
             cl::desc("Call of this function is error (i.e., it is an alias "
                      "to __assert_fail"),
             cl::cat(TerminationCat));

cl::opt<unsigned long long> MaxInstructions(
    "max-instructions",
    cl::desc("Stop execution after this many instructions.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned>
    MaxForks("max-forks",
             cl::desc("Only fork this many times.  Set to -1 to disable (default=-1)"),
             cl::init(~0u),
             cl::cat(TerminationCat));

cl::opt<unsigned> MaxDepth(
    "max-depth",
    cl::desc("Only allow this many symbolic branches.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned> MaxMemory("max-memory",
                            cl::desc("Refuse to fork when above this amount of "
                                     "memory (in MB) (default=2000)"),
                            cl::init(2000),
                            cl::cat(TerminationCat));

cl::opt<bool> MaxMemoryInhibit(
    "max-memory-inhibit",
    cl::desc(
        "Inhibit forking at memory cap (vs. random terminate) (default=true)"),
    cl::init(true),
    cl::cat(TerminationCat));

cl::opt<unsigned> RuntimeMaxStackFrames(
    "max-stack-frames",
    cl::desc("Terminate a state after this many stack frames.  Set to 0 to "
             "disable (default=8192)"),
    cl::init(8192),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticForkPct(
    "max-static-fork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction forking out of the "
             "forking of all instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticSolvePct(
    "max-static-solve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction over total solving time for all instructions "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPForkPct(
    "max-static-cpfork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction of a call path "
             "forking out of the forking of all instructions in the call path "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPSolvePct(
    "max-static-cpsolve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction of a call path over total solving time for all "
             "instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<std::string> TimerInterval(
    "timer-interval",
    cl::desc("Minimum interval to check timers. "
             "Affects -max-time, -istats-write-interval, -stats-write-interval, and -uncovered-update-interval (default=1s)"),
    cl::init("1s"),
    cl::cat(TerminationCat));


/*** Debugging options ***/

/// The different query logging solvers that can switched on/off
enum PrintDebugInstructionsType {
  STDERR_ALL, ///
  STDERR_SRC,
  STDERR_COMPACT,
  FILE_ALL,    ///
  FILE_SRC,    ///
  FILE_COMPACT ///
};

llvm::cl::bits<PrintDebugInstructionsType> DebugPrintInstructions(
    "debug-print-instructions",
    llvm::cl::desc("Log instructions during execution."),
    llvm::cl::values(
        clEnumValN(STDERR_ALL, "all:stderr",
                   "Log all instructions to stderr "
                   "in format [src, inst_id, "
                   "llvm_inst]"),
        clEnumValN(STDERR_SRC, "src:stderr",
                   "Log all instructions to stderr in format [src, inst_id]"),
        clEnumValN(STDERR_COMPACT, "compact:stderr",
                   "Log all instructions to stderr in format [inst_id]"),
        clEnumValN(FILE_ALL, "all:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id, llvm_inst]"),
        clEnumValN(FILE_SRC, "src:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id]"),
        clEnumValN(FILE_COMPACT, "compact:file",
                   "Log all instructions to file instructions.txt in format "
                   "[inst_id]") KLEE_LLVM_CL_VAL_END),
    llvm::cl::CommaSeparated,
    cl::cat(DebugCat));

#ifdef HAVE_ZLIB_H
cl::opt<bool> DebugCompressInstructions(
    "debug-compress-instructions", cl::init(false),
    cl::desc(
        "Compress the logged instructions in gzip format (default=false)."),
    cl::cat(DebugCat));
#endif

cl::opt<bool> DebugCheckForImpliedValues(
    "debug-check-for-implied-values", cl::init(false),
    cl::desc("Debug the implied value optimization"),
    cl::cat(DebugCat));

} // namespace

namespace klee {
  RNG theRNG;
}

// XXX hack
extern "C" unsigned dumpStates, dumpPTree;
unsigned dumpStates = 0, dumpPTree = 0;

const char *Executor::TerminateReasonNames[] = {
  [ Abort ] = "abort",
  [ Assert ] = "assert",
  [ BadVectorAccess ] = "bad_vector_access",
  [ Exec ] = "exec",
  [ External ] = "external",
  [ Free ] = "free",
  [ Leak ] = "leak",
  [ Model ] = "model",
  [ Overflow ] = "overflow",
  [ Ptr ] = "ptr",
  [ ReadOnly ] = "readonly",
  [ ReportError ] = "reporterror",
  [ User ] = "user",
  [ Unhandled ] = "xxx",
};


Executor::Executor(LLVMContext &ctx, const InterpreterOptions &opts,
                   InterpreterHandler *ih)
    : Interpreter(opts), interpreterHandler(ih), searcher(0),
      externalDispatcher(new ExternalDispatcher(ctx)), statsTracker(0),
      pathWriter(0), symPathWriter(0), specialFunctionHandler(0), timers{time::Span(TimerInterval)},
      replayKTest(0), replayPath(0), usingSeeds(0),
      atMemoryLimit(false), inhibitForking(false), haltExecution(false),
      ivcEnabled(false), debugLogBuffer(debugBufferString) {


  const time::Span maxTime{MaxTime};
  if (maxTime) timers.add(
      std::move(std::make_unique<Timer>(maxTime, [&]{
        klee_message("HaltTimer invoked");
        setHaltExecution(true);
      })));

  coreSolverTimeout = time::Span{MaxCoreSolverTime};
  if (coreSolverTimeout) UseForkedCoreSolver = true;
  Solver *coreSolver = klee::createCoreSolver(CoreSolverToUse);
  if (!coreSolver) {
    klee_error("Failed to create core solver\n");
  }

  Solver *solver = constructSolverChain(
      coreSolver,
      interpreterHandler->getOutputFilename(ALL_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(ALL_QUERIES_KQUERY_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_KQUERY_FILE_NAME));

  this->solver = new TimingSolver(solver, EqualitySubstitution);

  memory = new MemoryManager(&arrayCache);

  initializeSearchOptions();

  if (OnlyOutputStatesCoveringNew && !StatsTracker::useIStats())
    klee_error("To use --only-output-states-covering-new, you need to enable --output-istats.");

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    std::string debug_file_name =
        interpreterHandler->getOutputFilename("instructions.txt");
    std::string error;
#ifdef HAVE_ZLIB_H
    if (!DebugCompressInstructions) {
#endif
      debugInstFile = klee_open_output_file(debug_file_name, error);
#ifdef HAVE_ZLIB_H
    } else {
      debug_file_name.append(".gz");
      debugInstFile = klee_open_compressed_output_file(debug_file_name, error);
    }
#endif
    if (!debugInstFile) {
      klee_error("Could not open file %s : %s", debug_file_name.c_str(),
                 error.c_str());
    }
  }
}

llvm::Module *
Executor::setModule(std::vector<std::unique_ptr<llvm::Module>> &modules,
                    const ModuleOptions &opts) {
  assert(!kmodule && !modules.empty() &&
         "can only register one module"); // XXX gross

  kmodule = std::unique_ptr<KModule>(new KModule());

  // Preparing the final module happens in multiple stages

  // Link with KLEE intrinsics library before running any optimizations
  SmallString<128> LibPath(opts.LibraryDir);
  llvm::sys::path::append(LibPath, "libkleeRuntimeIntrinsic.bca");
  std::string error;
  if (!klee::loadFile(LibPath.str(), modules[0]->getContext(), modules,
                      error)) {
    klee_error("Could not load KLEE intrinsic file %s", LibPath.c_str());
  }

  // 1.) Link the modules together
  while (kmodule->link(modules, opts.EntryPoint)) {
    // 2.) Apply different instrumentation
    kmodule->instrument(opts);
  }

  // 3.) Optimise and prepare for KLEE

  // Create a list of functions that should be preserved if used
  std::vector<const char *> preservedFunctions;
  specialFunctionHandler = new SpecialFunctionHandler(*this);
  specialFunctionHandler->prepare(preservedFunctions);

  preservedFunctions.push_back(opts.EntryPoint.c_str());

  // Preserve the free-standing library calls
  preservedFunctions.push_back("memset");
  preservedFunctions.push_back("memcpy");
  preservedFunctions.push_back("memcmp");
  preservedFunctions.push_back("memmove");

  kmodule->optimiseAndPrepare(opts, preservedFunctions);
  kmodule->checkModule();

  // 4.) Manifest the module
  kmodule->manifest(interpreterHandler, StatsTracker::useStatistics());

  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics() || userSearcherRequiresMD2U()) {
    statsTracker = 
      new StatsTracker(*this,
                       interpreterHandler->getOutputFilename("assembly.ll"),
                       userSearcherRequiresMD2U());
  }

  // Initialize the context.
  DataLayout *TD = kmodule->targetData.get();
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width)TD->getPointerSizeInBits());
  memory->useLowMemory(TD->getPointerSizeInBits() == 32);

  return kmodule->module.get();
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  delete specialFunctionHandler;
  delete statsTracker;
  delete solver;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c, 
                                      unsigned offset) {
  const auto targetData = kmodule->targetData.get();
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i), 
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t)0, (uint8_t)0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i), 
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i), 
			     offset + sl->getElementOffset(i));
  } else if (const ConstantDataSequential *cds =
               dyn_cast<ConstantDataSequential>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cds->getElementType());
    for (unsigned i=0, e=cds->getNumElements(); i != e; ++i)
      initializeGlobalObject(state, os, cds->getElementAsConstant(i),
                             offset + i*elementSize);
  } else if (!isa<UndefValue>(c) && !isa<MetadataAsValue>(c)) {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    KValue C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C.getWidth() && "Invalid store size!");
    if (StoreBits > C.getWidth())
      C = C.ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject *Executor::addExternalObject(ExecutionState &state, void *addr,
                                          unsigned size, bool isReadOnly,
                                          uint64_t specialSegment) {
  auto mo = memory->allocateFixed(size, nullptr, specialSegment);
  state.addressSpace.concreteAddressMap.insert(
      {reinterpret_cast<uint64_t>(addr), mo->segment});
  ObjectState *os = bindObjectInState(state, mo, false);
  for (unsigned i = 0; i < size; i++)
    os->write8(i, (uint8_t)mo->segment, ((uint8_t *)addr)[i]);
  if (isReadOnly)
    os->setReadOnly(true);
  return mo;
}

extern void *__dso_handle __attribute__((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module.get();

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");

  // illegal function (so that we won't collide with nullptr).
  // The legal functions are numbered from 1
  legalFunctions.emplace(0, nullptr);

  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = &*i;

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() && 
        !externalDispatcher->resolveSymbol(f->getName())) {
        // insert nullptr
        globalAddresses.emplace(f, KValue(Expr::createPointer(0)));
    } else {
      auto id = legalFunctions.size();
      legalFunctions.emplace(id, f);
      globalAddresses.emplace(f, KValue(FUNCTIONS_SEGMENT, Expr::createPointer(id)));
    }
  }

#ifndef WINDOWS
  int *errno_addr = getErrnoLocation(state);
  MemoryObject *errnoObj =
      addExternalObject(state, errno_addr, sizeof *errno_addr, false, ERRNO_SEGMENT);
  // Copy values from and to program space explicitly
  errnoObj->isUserSpecified = true;
#endif

  // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, const_cast<uint16_t*>(*addr-128),
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);
    
  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, const_cast<int32_t*>(*lower_addr-128),
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);
  
  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, const_cast<int32_t*>(*upper_addr-128),
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    const GlobalVariable *v = &*i;
    size_t globalObjectAlignment = getAllocationAlignment(v);
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      Type *ty = i->getType()->getElementType();
      uint64_t size = 0;
      if (ty->isSized()) {
	size = kmodule->targetData->getTypeStoreSize(ty);
      } else {
        klee_warning("Type for %.*s is not sized", (int)i->getName().size(),
			i->getName().data());
      }

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        klee_warning("Unable to find size for global variable: %.*s (use will result in out of bounds access)",
			(int)i->getName().size(), i->getName().data());
      }

      MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                                          /*isGlobal=*/true, /*allocSite=*/v,
                                          /*alignment=*/globalObjectAlignment);
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(v, mo));
      globalAddresses.insert(std::make_pair(v, mo->getPointer()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.", 
                     i->getName().data());

        for (unsigned offset=0; offset<size; offset++)
          os->write8(offset, 0, ((unsigned char*)addr)[offset]);
      }
    } else {
      Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                                          /*isGlobal=*/true, /*allocSite=*/v,
                                          /*alignment=*/globalObjectAlignment);
      if (!mo)
        llvm::report_fatal_error("out of memory");
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(v, mo));
      globalAddresses.insert(std::make_pair(v, mo->getPointer()));

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }
  
  // link aliases to their definitions (if bound)
  for (auto i = m->alias_begin(), ie = m->alias_end(); i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions.

    // Alias may refer to other alias, not necessarily known at this point.
    // Thus, resolve to real alias directly.
    const GlobalAlias *alias = &*i;
    while (const auto *ga = dyn_cast<GlobalAlias>(alias->getAliasee())) {
      assert(ga != alias && "alias pointing to itself");
      alias = ga;
    }

    globalAddresses.insert(std::make_pair(&*i, evalConstant(alias->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  // remember constant objects to initialise their counter part for external
  // calls
  std::vector<ObjectState *> constantObjects;
  SegmentAddressMap initializedMOs;

  for (Module::const_global_iterator i = m->global_begin(), e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      const GlobalVariable *v = &*i;
      MemoryObject *mo = globalObjects.find(v)->second;
      void *address = memory->allocateMemory(
          mo->allocatedSize, getAllocationAlignment(mo->allocSite));
      if (!address)
        klee_error("Couldn't allocate memory for external function");

      initializedMOs.insert({mo->segment, reinterpret_cast<uint64_t>(address)});
      state.addressSpace.concreteAddressMap.insert(
          {reinterpret_cast<uint64_t>(address), mo->getSegment()});
      state.addressSpace.segmentMap.replace(
          std::make_pair(mo->getSegment(), mo));

      const ObjectState *os = state.addressSpace.findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace.getWriteable(mo, os);

      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      if (i->isConstant())
        constantObjects.emplace_back(wos);
    }
  }

  // initialise constant memory that is potentially used with external calls
  if (!constantObjects.empty()) {
    // initialise the actual memory with constant values
    state.addressSpace.copyOutConcretes(initializedMOs);

    // mark constant objects as read-only
    for (auto obj : constantObjects)
      obj->setReadOnly(true);
  }
}

void Executor::branch(ExecutionState &state, 
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result) {
  TimerStatIncrementer timer(stats::forkTime);
  unsigned N = conditions.size();
  assert(N);

  if (MaxForks!=~0u && stats::forks >= MaxForks) {
    unsigned next = theRNG.getInt32() % N;
    for (unsigned i=0; i<N; ++i) {
      if (i == next) {
        result.push_back(&state);
      } else {
        result.push_back(NULL);
      }
    }
  } else {
    stats::forks += N-1;

    // XXX do proper balance or keep random?
    result.push_back(&state);
    for (unsigned i=1; i<N; ++i) {
      ExecutionState *es = result[theRNG.getInt32() % i];
      ExecutionState *ns = es->branch();
      addedStates.push_back(ns);
      result.push_back(ns);
      processTree->attach(es->ptreeNode, ns, es);
    }
  }

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).
  
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(state, siit->assignment.evaluate(conditions[i]), 
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }
      
      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      // Extra check in case we're replaying seeds with a max-fork
      if (result[i])
        seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (result[i] && !seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      } 
    }
  }

  for (unsigned i=0; i<N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair 
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal) {
  Solver::Validity res;
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  if (!isSeeding && !isa<ConstantExpr>(condition) && 
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > time::seconds(60)) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack.back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) > 
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) > 
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) > 
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) > 
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value; 
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }
  }

  time::Span timeout = coreSolverTimeout;
  if (isSeeding)
    timeout *= static_cast<unsigned>(it->second.size());
  solver->setTimeout(timeout);
  bool success = solver->evaluate(current, condition, res);
  solver->setTimeout(time::Span());
  if (!success) {
    current.pc = current.prevPC;
    terminateStateEarly(current, "Query timed out (fork).");
    return StatePair(0, 0);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition<replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];
      
      if (res==Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res==Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if(branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else  {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res==Solver::Unknown) {
      assert(!replayKTest && "in replay mode, only one branch can be true.");
      
      if ((MaxMemoryInhibit && atMemoryLimit) || 
          current.forkDisabled ||
          inhibitForking || 
          (MaxForks!=~0u && stats::forks >= MaxForks)) {

	if (MaxMemoryInhibit && atMemoryLimit)
	  klee_warning_once(0, "skipping fork (memory cap exceeded)");
	else if (current.forkDisabled)
	  klee_warning_once(0, "skipping fork (fork disabled on current path)");
	else if (inhibitForking)
	  klee_warning_once(0, "skipping fork (fork disabled globally)");
	else 
	  klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;        
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && 
      (current.forkDisabled || OnlyReplaySeeds) && 
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success = 
        solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      
      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
    }
  }


  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res==Solver::True) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }

    return StatePair(&current, 0);
  } else if (res==Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    return StatePair(0, &current);
  } else {
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.push_back(falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }
      
      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    processTree->attach(current.ptreeNode, falseState, trueState);

    if (pathWriter) {
      // Need to update the pathOS.id field of falseState, otherwise the same id
      // is used for both falseState and trueState.
      falseState->pathOS = pathWriter->open(current.pathOS);
      if (!isInternal) {
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }
    }
    if (symPathWriter) {
      falseState->symPathOS = symPathWriter->open(current.symPathOS);
      if (!isInternal) {
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth<=trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded.");
      terminateStateEarly(*falseState, "max-depth exceeded.");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    if (!CE->isTrue())
      llvm::report_fatal_error("attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success = 
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint"); 
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition, 
                                 ConstantExpr::alloc(1, Expr::Bool));
}

const Cell& Executor::eval(KInstruction *ki, unsigned index, 
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack.back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state,
                         const KValue &value) {
  getDestCell(state, target) = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index,
                            ExecutionState &state, const KValue &value) {
  getArgumentCell(state, kf, index) = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, 
                             const ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;
    auto expr = optimizer.optimizeExpr(e, true);
    solver->setTimeout(coreSolverTimeout);
    if (solver->getValue(state, expr, value)) {
      ref<Expr> cond = EqExpr::create(expr, value);
      cond = optimizer.optimizeExpr(cond, false);
      if (solver->mustBeTrue(state, cond, isTrue) && isTrue)
        result = value;
    }
    solver->setTimeout(time::Span());
  }
  
  return result;
}


/* Concretize the given expression, and return a possible constant value. 
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr> 
Executor::toConstant(ExecutionState &state, 
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;

  std::string str;
  llvm::raw_string_ostream os(str);
  os << "silently concretizing (reason: " << reason << ") expression " << e
     << " to value " << value << " (" << (*(state.pc)).info->file << ":"
     << (*(state.pc)).info->line << ")";

  if (AllExternalWarnings)
    klee_warning("%s", os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));
    
  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               const KValue& kval,
                               KInstruction *target) {
  ref<Expr> expr = state.constraints.simplifyExpr(kval.getValue());
  ref<Expr> segment = state.constraints.simplifyExpr(kval.getSegment());

  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it==seedMap.end() ||
      (isa<ConstantExpr>(expr) && isa<ConstantExpr>(segment))) {
    ref<ConstantExpr> off, seg;
    expr = optimizer.optimizeExpr(expr, true);
    bool success = solver->getValue(state, expr, off);
    assert(success && "FIXME: Unhandled solver failure");

    success = solver->getValue(state, segment, seg);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, state, KValue(seg, off));
  } else {
    // This does not work with segments yet
    assert(0 && "Not implemented with segments yet");
    abort();

    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<Expr> cond = siit->assignment.evaluate(expr);
      cond = optimizer.optimizeExpr(cond, true);
      ref<ConstantExpr> value;
      bool success = solver->getValue(state, cond, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }
    
    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(expr, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches);
    
    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es) {
        assert(0 && "Need segment");
        bindLocal(target, *es, KValue(*vit));
      }
      ++bit;
    }
  }
}

void Executor::printDebugInstructions(ExecutionState &state) {
  // check do not print
  if (DebugPrintInstructions.getBits() == 0)
	  return;

  llvm::raw_ostream *stream = 0;
  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(STDERR_SRC) ||
      DebugPrintInstructions.isSet(STDERR_COMPACT))
    stream = &llvm::errs();
  else
    stream = &debugLogBuffer;

  if (!DebugPrintInstructions.isSet(STDERR_COMPACT) &&
      !DebugPrintInstructions.isSet(FILE_COMPACT)) {
    (*stream) << "     " << state.pc->getSourceLocation() << ":";
  }

  (*stream) << state.pc->info->assemblyLine;

  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(FILE_ALL))
    (*stream) << ":" << *(state.pc->inst);
  (*stream) << "\n";

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    debugLogBuffer.flush();
    (*debugInstFile) << debugLogBuffer.str();
    debugBufferString = "";
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  printDebugInstructions(state);
  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  ++state.steppedInstructions;
  state.prevPC = state.pc;
  ++state.pc;

  if (stats::instructions == MaxInstructions)
    haltExecution = true;
}

void Executor::executeLifetimeIntrinsic(ExecutionState &state,
                                        KInstruction *ki,
                                        const std::vector<Cell> &arguments,
                                        bool isEnd) {
  llvm::Instruction *mem
    = llvm::dyn_cast<Instruction>(ki->inst->getOperand(1)->stripPointerCasts());

  if (!mem) {
    terminateStateOnExecError(state,
        "Unhandled argument for lifetime intrinsic (not an instruction).");
    return;
  }

  auto kinstMem = kmodule->getKInstruction(mem);

  if (!llvm::isa<llvm::AllocaInst>(kinstMem->inst)) {
    terminateStateOnExecError(state,
        "Unhandled argument for lifetime intrinsic (not alloca)");
    return;
  }

  executeLifetimeIntrinsic(state, ki, kinstMem, arguments[1], isEnd);
}

void Executor::executeLifetimeIntrinsic(ExecutionState &state,
                                        KInstruction *ki,
                                        KInstruction *allocSite,
                                        const KValue& address,
                                        bool isEnd) {
  ObjectPair op;
  bool success;
  llvm::Optional<uint64_t> temp;
  state.addressSpace.resolveOne(state, solver, address, op, success, temp);
  if (!success) {
    // the object is dead, create a new one
    // XXX: we should distringuish between resolve error and dead object...
    if (!isEnd) {
      executeAlloc(state, getSizeForAlloca(state, allocSite), true /* isLocal */,
          allocSite);
    } else {
      //klee_warning("Could not find allocation for lifetime end");
      terminateStateOnError(state, "Memory object is dead", Ptr);
    }
    return;
  }

  // FIXME: detect the cases where we do not mark lifetime of the whole memory.
  // We should also check that the object's state is empty (the object has not been
  // written before)

  if (isEnd) {
    state.removeAlloca(op.first);
    //bindLocal(allocSite, state, KValue(Expr::createPointer(0)));
  } else {
    // This is the first call to lifetime start, the object already exists.
    // We do not want to reallocate it as there may exist pointers to it

    //executeAlloc(state, getSizeForAlloca(state, allocSite), true /* isLocal */,
    //             allocSite, false /* zeroMem */, op.second /* realloc from */);
  }
}

static inline const llvm::fltSemantics *fpWidthToSemantics(unsigned width) {
  switch (width) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle();
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble();
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended();
#else
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
#endif
  default:
    return 0;
  }
}


static inline bool isErrorCall(const llvm::StringRef& name) {
    return name.equals(ErrorFun);
}

void Executor::executeCall(ExecutionState &state, 
                           KInstruction *ki,
                           Function *f,
                           const std::vector<Cell> &arguments) {
  Instruction *i = ki->inst;
  if (i && isa<DbgInfoIntrinsic>(i))
    return;

  // FIXME: hack!
  if (f->getName().equals("__INSTR_check_nontermination")) {
    state.lastLoopCheck = ki->inst;
    // fall-through
  } else if (f->getName().equals("__INSTR_fail")) {
    state.lastLoopFail = ki->inst;
    // fall-through
  } else if (isErrorCall(f->getName())) {
      terminateStateOnError(state,
                            "ASSERTION FAIL: " + ErrorFun + " called",
				            Executor::Assert);
      return;
  }

  if (f->getName().equals("__INSTR_check_nontermination_header")) {
    state.lastLoopHead = ki->inst;
    state.lastLoopHeadId = state.nondetValues.size();
    return;
  }

  if (f && f->isDeclaration()) {
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;
    case Intrinsic::fabs: {
      ref<ConstantExpr> arg =
          toConstant(state, eval(ki, 0, state).value, "floating point");
      if (!fpWidthToSemantics(arg->getWidth()))
        return terminateStateOnExecError(
            state, "Unsupported intrinsic llvm.fabs call");

      llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()),
                        arg->getAPValue());
      Res = llvm::abs(Res);

      bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
      break;
    }
    // va_arg is handled by caller and intrinsic lowering, see comment for
    // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = state.stack.back();

      // varargs can be zero if no varargs were provided
      if (!sf.varargs)
        return;

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work for x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        // TODO value segment
        executeMemoryWrite(state, arguments[0], sf.varargs->getPointer());
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // x86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        KValue address = arguments[0];
        executeMemoryWrite(state, address,
                           KValue(ConstantExpr::create(48, 32))); // gp_offset
        address = arguments[0].Add(ConstantExpr::create(4, 64));
        executeMemoryWrite(state, address,
                           KValue(ConstantExpr::create(304, 32))); // fp_offset
        address = arguments[0].Add(ConstantExpr::create(8, 64));
        executeMemoryWrite(state, address,
                           sf.varargs->getPointer()); // overflow_arg_area
        address = arguments[0].Add(ConstantExpr::create(16, 64));
        executeMemoryWrite(state, address,
                           KValue(ConstantExpr::create(0, 64))); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with va_end, however (like call it twice).
      break;
        
    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
        break;
   case Intrinsic::lifetime_start:
      executeLifetimeIntrinsic(state, ki, arguments, false /* is end */);
      break;
   case Intrinsic::lifetime_end:
      executeLifetimeIntrinsic(state, ki, arguments, true /* is end */);
      break;
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
  } else {
    // Check if maximum stack size was reached.
    // We currently only count the number of stack frames
    if (RuntimeMaxStackFrames && state.stack.size() > RuntimeMaxStackFrames) {
      terminateStateEarly(state, "Maximum stack size reached.");
      klee_warning("Maximum stack size reached.");
      return;
    }

    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    KFunction *kf = kmodule->functionMap[f];

    state.pushFrame(state.prevPC, kf);
    state.pc = kf->instructions;

    if (statsTracker)
      statsTracker->framePushed(state, &state.stack[state.stack.size()-2]);

     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes

    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.", 
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }
    } else {
      Expr::Width WordSize = Context::get().getPointerWidth();

      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }

      StackFrame &sf = state.stack.back();
      unsigned size = 0;
      bool requires16ByteAlignment = false;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work for x86-32 and x86-64, however.
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i].getWidth());
        } else {
          Expr::Width argWidth = arguments[i].getWidth();
          // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a
          // 16 byte boundary if alignment needed by type exceeds 8 byte
          // boundary.
          //
          // Alignment requirements for scalar types is the same as their size
          if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
             size = llvm::alignTo(size, 16);
#else
             size = llvm::RoundUpToAlignment(size, 16);
#endif
             requires16ByteAlignment = true;
          }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
          size += llvm::alignTo(argWidth, WordSize) / 8;
#else
          size += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
        }
      }

      MemoryObject *mo = sf.varargs =
          memory->allocate(size, true, false, state.prevPC->inst,
                           (requires16ByteAlignment ? 16 : 8));
      if (!mo && size) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }

      if (mo) {
        if ((WordSize == Expr::Int64) && requires16ByteAlignment) {
          // Both 64bit Linux/Glibc and 64bit MacOSX should align to 16 bytes.
          klee_warning_once(
              0, "While allocating varargs: malloc did not align to 16 bytes.");
        }

        ObjectState *os = bindObjectInState(state, mo, true);
        unsigned offset = 0;
        for (unsigned i = funcArgs; i < callingArgs; i++) {
          // FIXME: This is really specific to the architecture, not the pointer
          // size. This happens to work for x86-32 and x86-64, however.
          if (WordSize == Expr::Int32) {
            os->write(offset, arguments[i]);
            offset += Expr::getMinBytesForWidth(arguments[i].getWidth());
          } else {
            assert(WordSize == Expr::Int64 && "Unknown word size!");

            Expr::Width argWidth = arguments[i].getWidth();
            if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
              offset = llvm::alignTo(offset, 16);
#else
              offset = llvm::RoundUpToAlignment(offset, 16);
#endif
            }
            KValue toWrite{arguments[i].getSegment(), arguments[i].getOffset()};
            os->write(offset, toWrite);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
            offset += llvm::alignTo(argWidth, WordSize) / 8;
#else
            offset += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
          }
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i) 
      bindArgument(kf, i, state, arguments[i]);
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src, 
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.
  
  // XXX this lookup has to go ?
  KFunction *kf = state.stack.back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc = &kf->instructions[entry];
  if (state.pc->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc->inst);
    state.incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

/// Compute the true target of a function call, resolving LLVM aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue*, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv).second)
        return 0;

      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode()==Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

ref<Expr> Executor::getSizeForAlloca(ExecutionState& state, KInstruction *ki) const {
  AllocaInst *ai = cast<AllocaInst>(ki->inst);
  unsigned elementSize =
    kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
  ref<Expr> size = Expr::createPointer(elementSize);
  if (ai->isArrayAllocation()) {
    ref<Expr> count = eval(ki, 0, state).value;
    count = Expr::createZExtToPointerWidth(count);
    size = MulExpr::create(size, count);
  }
  return size;
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;
  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack.back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    KValue result(ConstantExpr::alloc(0, Expr::Bool));
    
    if (!isVoidReturn) {
      result = eval(ki, 0, state);
    }
    
    if (state.stack.size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      // there is no other instruction to execute
      state.pc = {0};
      terminateStateOnExit(state);
    } else {
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.pc = kcaller;
        ++state.pc;
      }

      if (!isVoidReturn) {
        Type *t = caller->getType();
        if (t != Type::getVoidTy(i->getContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result.getWidth();
          Expr::Width to = getWidthForLLVMType(t);
            
          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) : 
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
            bool isSExt = cs.hasRetAttr(llvm::Attribute::SExt);
#else
            bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#endif
            if (isSExt) {
              result = result.SExt(to);
            } else {
              result = result.ZExt(to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }
    }      
    break;
  }
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;

      cond = optimizer.optimizeExpr(cond, false);
      Executor::StatePair branches = fork(state, cond, false);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack.back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
    }
    break;
  }
  case Instruction::IndirectBr: {
    // implements indirect branch to a label within the current function
    const auto bi = cast<IndirectBrInst>(i);
    auto address = eval(ki, 0, state).value;
    address = toUnique(state, address);

    // concrete address
    if (const auto CE = dyn_cast<ConstantExpr>(address.get())) {
      const auto bb_address = (BasicBlock *) CE->getZExtValue(Context::get().getPointerWidth());
      transferToBasicBlock(bb_address, bi->getParent(), state);
      break;
    }

    // symbolic address
    const auto numDestinations = bi->getNumDestinations();
    std::vector<BasicBlock *> targets;
    targets.reserve(numDestinations);
    std::vector<ref<Expr>> expressions;
    expressions.reserve(numDestinations);

    ref<Expr> errorCase = ConstantExpr::alloc(1, Expr::Bool);
    SmallPtrSet<BasicBlock *, 5> destinations;
    // collect and check destinations from label list
    for (unsigned k = 0; k < numDestinations; ++k) {
      // filter duplicates
      const auto d = bi->getDestination(k);
      if (destinations.count(d)) continue;
      destinations.insert(d);

      // create address expression
      const auto PE = Expr::createPointer(reinterpret_cast<std::uint64_t>(d));
      ref<Expr> e = EqExpr::create(address, PE);

      // exclude address from errorCase
      errorCase = AndExpr::create(errorCase, Expr::createIsZero(e));

      // check feasibility
      bool result;
      bool success __attribute__ ((unused)) = solver->mayBeTrue(state, e, result);
      assert(success && "FIXME: Unhandled solver failure");
      if (result) {
        targets.push_back(d);
        expressions.push_back(e);
      }
    }
    // check errorCase feasibility
    bool result;
    bool success __attribute__ ((unused)) = solver->mayBeTrue(state, errorCase, result);
    assert(success && "FIXME: Unhandled solver failure");
    if (result) {
      expressions.push_back(errorCase);
    }

    // fork states
    std::vector<ExecutionState *> branches;
    branch(state, expressions, branches);

    // terminate error state
    if (result) {
      terminateStateOnExecError(*branches.back(), "indirectbr: illegal label address");
      branches.pop_back();
    }

    // branch states to resp. target blocks
    assert(targets.size() == branches.size());
    for (std::vector<ExecutionState *>::size_type k = 0; k < branches.size(); ++k) {
      if (branches[k]) {
        transferToBasicBlock(targets[k], bi->getParent(), *branches[k]);
      }
    }

    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    BasicBlock *bb = si->getParent();

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      llvm::IntegerType *Ty = cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
      unsigned index = si->findCaseValue(ci)->getSuccessorIndex();
#else
      unsigned index = si->findCaseValue(ci).getSuccessorIndex();
#endif
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);
    } else {
      // Handle possible different branch targets

      // We have the following assumptions:
      // - each case value is mutual exclusive to all other values
      // - order of case branches is based on the order of the expressions of
      //   the case values, still default is handled last
      std::vector<BasicBlock *> bbOrder;
      std::map<BasicBlock *, ref<Expr> > branchTargets;

      std::map<ref<Expr>, BasicBlock *> expressionOrder;

      // Iterate through all non-default cases and order them by expressions
      for (auto i : si->cases()) {
        ref<Expr> value = evalConstant(i.getCaseValue()).getValue();
        BasicBlock *caseSuccessor = i.getCaseSuccessor();
        expressionOrder.insert(std::make_pair(value, caseSuccessor));
      }

      // Track default branch values
      ref<Expr> defaultValue = ConstantExpr::alloc(1, Expr::Bool);

      // iterate through all non-default cases but in order of the expressions
      for (std::map<ref<Expr>, BasicBlock *>::iterator
               it = expressionOrder.begin(),
               itE = expressionOrder.end();
           it != itE; ++it) {
        ref<Expr> match = EqExpr::create(cond, it->first);

        // skip if case has same successor basic block as default case
        // (should work even with phi nodes as a switch is a single terminating instruction)
        if (it->second == si->getDefaultDest()) continue;

        // Make sure that the default value does not contain this target's value
        defaultValue = AndExpr::create(defaultValue, Expr::createIsZero(match));

        // Check if control flow could take this case
        bool result;
        match = optimizer.optimizeExpr(match, false);
        bool success = solver->mayBeTrue(state, match, result);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (result) {
          BasicBlock *caseSuccessor = it->second;

          // Handle the case that a basic block might be the target of multiple
          // switch cases.
          // Currently we generate an expression containing all switch-case
          // values for the same target basic block. We spare us forking too
          // many times but we generate more complex condition expressions
          // TODO Add option to allow to choose between those behaviors
          std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> res =
              branchTargets.insert(std::make_pair(
                  caseSuccessor, ConstantExpr::alloc(0, Expr::Bool)));

          res.first->second = OrExpr::create(match, res.first->second);

          // Only add basic blocks which have not been target of a branch yet
          if (res.second) {
            bbOrder.push_back(caseSuccessor);
          }
        }
      }

      // Check if control could take the default case
      defaultValue = optimizer.optimizeExpr(defaultValue, false);
      bool res;
      bool success = solver->mayBeTrue(state, defaultValue, res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> ret =
            branchTargets.insert(
                std::make_pair(si->getDefaultDest(), defaultValue));
        if (ret.second) {
          bbOrder.push_back(si->getDefaultDest());
        }
      }

      // Fork the current state with each state having one of the possible
      // successors of this switch
      std::vector< ref<Expr> > conditions;
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        conditions.push_back(branchTargets[*it]);
      }
      std::vector<ExecutionState*> branches;
      branch(state, conditions, branches);

      std::vector<ExecutionState*>::iterator bit = branches.begin();
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es)
          transferToBasicBlock(*it, bb, *es);
        ++bit;
      }
    }
    break;
  }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    // Ignore debug intrinsic calls
    if (isa<DbgInfoIntrinsic>(i))
      break;
    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();
    Function *f = getTargetFunction(fp, state);

    if (isa<InlineAsm>(fp)) {
      terminateStateOnExecError(state, "inline assembly is unsupported");
      break;
    }
    // evaluate arguments
    std::vector<Cell> arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j)
      arguments.push_back(eval(ki, j+1, state));

    if (f) {
      const FunctionType *fType = 
        dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType =
        dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector<Cell>::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = ai->value->getWidth();
            
          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
              bool isSExt = cs.paramHasAttr(i, llvm::Attribute::SExt);
#else
              bool isSExt = cs.paramHasAttr(i+1, llvm::Attribute::SExt);
#endif
              if (isSExt) {
                arguments[i] = arguments[i].SExt(to);
              } else {
                arguments[i] = arguments[i].ZExt(to);
              }
            }
          }
            
          i++;
        }
      }

      executeCall(state, ki, f, arguments);
    } else {
      auto pointer = eval(ki, 0, state);
      // We handle constant segments for now
      assert((cast<ConstantExpr>(pointer.getSegment())->getZExtValue()
                == FUNCTIONS_SEGMENT) && "Invalid function pointer");
      ref<Expr> v = optimizer.optimizeExpr(pointer.getValue(), true);

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        StatePair res = fork(*free, EqExpr::create(v, value), true);
        if (res.first) {
          uint64_t id = value->getZExtValue();
          auto it = legalFunctions.find(id);
          if (it != legalFunctions.end()) {
            f = it->second;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once(reinterpret_cast<void*>(f),
                                "resolved symbolic function pointer to id %lu: %s",
                                id, f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
    const Cell &cell = eval(ki, state.incomingBBIndex, state);
    bindLocal(ki, state, cell);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    // NOTE: It is not required that operands 1 and 2 be of scalar type.
    KValue cond = eval(ki, 0, state);
    const Cell &tCell = eval(ki, 1, state);
    const Cell &fCell = eval(ki, 2, state);
    bindLocal(ki, state, cond.Select(tCell, fCell));
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Add(right));
    break;
  }

  case Instruction::Sub: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Sub(right));
    break;
  }
 
  case Instruction::Mul: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Mul(right));
    break;
  }

  case Instruction::UDiv: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.UDiv(right));
    break;
  }

  case Instruction::SDiv: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.SDiv(right));
    break;
  }

  case Instruction::URem: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.URem(right));
    break;
  }

  case Instruction::SRem: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.SRem(right));
    break;
  }

  case Instruction::And: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);

    auto newCell = left.And(right);
    newCell.pointerSegment = left.getSegment();
    bindLocal(ki, state, newCell);
    break;
  }

  case Instruction::Or: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Or(right));
    break;
  }

  case Instruction::Xor: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Xor(right));
    break;
  }

  case Instruction::Shl: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.Shl(right));
    break;
  }

  case Instruction::LShr: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.LShr(right));
    break;
  }

  case Instruction::AShr: {
    const Cell &left = eval(ki, 0, state);
    const Cell &right = eval(ki, 1, state);
    bindLocal(ki, state, left.AShr(right));
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);

    const Cell &leftOriginal = eval(ki, 0, state);
    const Cell &rightOriginal = eval(ki, 1, state);

    const auto leftSegment =
        dyn_cast<ConstantExpr>(leftOriginal.getSegment().get());
    const auto rightSegment =
        dyn_cast<ConstantExpr>(rightOriginal.getSegment().get());
    const auto leftValue =
        dyn_cast<ConstantExpr>(leftOriginal.getValue().get());
    const auto rightValue =
        dyn_cast<ConstantExpr>(rightOriginal.getValue().get());

    ref<Expr> leftArray;
    ref<Expr> rightArray;

    /// Only use symbolics with Constant values(offsets)
    bool useOriginalValues = true;

    if (leftValue && rightValue) {
      useOriginalValues = false;
    }

    bool success = false;
    const auto &pointerWidth = Context::get().getPointerWidth();

    if (!useOriginalValues && leftSegment && rightSegment &&
        leftSegment->getWidth() == pointerWidth &&
        rightSegment->getWidth() == pointerWidth && !leftSegment->isZero() &&
        !rightSegment->isZero() &&
        rightSegment->getZExtValue() != leftSegment->getZExtValue()) {

      ObjectPair op;
      bool successRight = false;
      bool successLeft =
          state.addressSpace.resolveOneConstantSegment(leftOriginal, op);

      if (successLeft) {
        leftArray = const_cast<MemoryObject *>(op.first)->getSymbolicAddress(
            arrayCache);
        successRight =
            state.addressSpace.resolveOneConstantSegment(rightOriginal, op);
      }
      if (successRight) {
        rightArray = const_cast<MemoryObject *>(op.first)->getSymbolicAddress(
            arrayCache);
        success = true;
      }
    }
    KValue left;
    KValue right;

    if (!success) {
      left = static_cast<KValue>(leftOriginal);
      right = static_cast<KValue>(rightOriginal);
    } else {
      klee_warning("Comparing pointers, using symbolic values instead of "
                   "segment for comparison");
      left = KValue(leftOriginal.getSegment(), leftArray);
      right = KValue(rightOriginal.getSegment(), rightArray);
    }

    switch (ii->getPredicate()) {
    case ICmpInst::ICMP_EQ:
      bindLocal(ki, state, left.Eq(right));
      break;
    case ICmpInst::ICMP_NE:
      bindLocal(ki, state, left.Ne(right));
      break;
    case ICmpInst::ICMP_UGT:
      bindLocal(ki, state, left.Ugt(right));
      break;
    case ICmpInst::ICMP_UGE:
      bindLocal(ki, state, left.Uge(right));
      break;
    case ICmpInst::ICMP_ULT:
      bindLocal(ki, state, left.Ult(right));
      break;
    case ICmpInst::ICMP_ULE:
      bindLocal(ki, state, left.Ule(right));
      break;
    case ICmpInst::ICMP_SGT:
      bindLocal(ki, state, left.Sgt(right));
      break;
    case ICmpInst::ICMP_SGE:
      bindLocal(ki, state, left.Sge(right));
      break;
    case ICmpInst::ICMP_SLT:
      bindLocal(ki, state, left.Slt(right));
      break;
    case ICmpInst::ICMP_SLE:
      bindLocal(ki, state, left.Sle(right));
      break;
    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }

    // Memory instructions...
  case Instruction::Alloca: {
    executeAlloc(state, getSizeForAlloca(state, ki), true, ki);
    break;
  }

  case Instruction::Load: {
    const Cell &baseCell = eval(ki, 0, state);
    executeMemoryRead(state, baseCell, ki);
    break;
  }
  case Instruction::Store: {
    const Cell &baseCell = eval(ki, 1, state);
    const Cell &valueCell = eval(ki, 0, state);
    executeMemoryWrite(state, baseCell, valueCell);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    KValue base = eval(ki, 0, state);
    Expr::Width pointerWidth = Context::get().getPointerWidth();

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator 
           it = kgepi->indices.begin(), ie = kgepi->indices.end(); 
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      KValue index = eval(ki, it->first, state);
      base = base.Add(
          index.SExt(pointerWidth)
          .Mul(ConstantExpr::create(elementSize, pointerWidth)));
    }
    if (kgepi->offset)
      base = base.Add(ConstantExpr::create(kgepi->offset, pointerWidth));
    bindLocal(ki, state, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    const Cell &cell = eval(ki, 0, state);
    KValue result = cell.Extract(0, getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    const Cell &cell = eval(ki, 0, state);
    bindLocal(ki, state, cell.SExt(getWidthForLLVMType(ci->getType())));
    break;
  }

  case Instruction::ZExt:
  case Instruction::IntToPtr:
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    const Cell &cell = eval(ki, 0, state);
    bindLocal(ki, state, cell.ZExt(getWidthForLLVMType(ci->getType())));
    break;
  }

  case Instruction::BitCast: {
    const Cell &cell = eval(ki, 0, state);
    bindLocal(ki, state, cell);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.add(APFloat(*fpWidthToSemantics(right->getWidth()),right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.subtract(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FMul: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.multiply(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.divide(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.mod(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()));
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
    auto valueRef = makeMutableArrayRef(value);
#else
    uint64_t *valueRef = &value;
#endif
    Arg.convertToInteger(valueRef, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());

    uint64_t value = 0;
    bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
    auto valueRef = makeMutableArrayRef(value);
#else
    uint64_t *valueRef = &value;
#endif
    Arg.convertToInteger(valueRef, resultType, true,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

    APFloat LHS(*fpWidthToSemantics(left->getWidth()),left->getAPValue());
    APFloat RHS(*fpWidthToSemantics(right->getWidth()),right->getAPValue());
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch( fi->getPredicate() ) {
      // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = (CmpRes != APFloat::cmpUnordered);
      break;

    case FCmpInst::FCMP_UNO:
      Result = (CmpRes == APFloat::cmpUnordered);
      break;

      // Ordered comparisons return false if either operand is NaN.  Unordered
      // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpEqual);
      break;
    case FCmpInst::FCMP_OEQ:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpEqual);
      break;

    case FCmpInst::FCMP_UGT:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpGreaterThan);
      break;
    case FCmpInst::FCMP_OGT:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpGreaterThan);
      break;

    case FCmpInst::FCMP_UGE:
      Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
      break;
    case FCmpInst::FCMP_OGE:
      Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
      break;

    case FCmpInst::FCMP_ULT:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpLessThan);
      break;
    case FCmpInst::FCMP_OLT:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpLessThan);
      break;

    case FCmpInst::FCMP_ULE:
      Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
      break;
    case FCmpInst::FCMP_OLE:
      Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
      break;

    case FCmpInst::FCMP_UNE:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual);
      break;
    case FCmpInst::FCMP_ONE:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual);
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
      break;
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    bindLocal(ki, state, ConstantExpr::alloc(Result, Expr::Bool));
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    KValue agg = eval(ki, 0, state);
    KValue val = eval(ki, 1, state);

    KValue l, r;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val.getWidth();
    bool hasL = lOffset > 0;
    bool hasR = rOffset < agg.getWidth();

    if (hasL)
      l = agg.Extract(0, lOffset);
    if (hasR)
      r = agg.Extract(rOffset, agg.getWidth() - rOffset);

    KValue result;
    if (hasL && hasR)
      result = r.Concat(val.Concat(l));
    else if (hasL)
      result = val.Concat(l);
    else if (hasR)
      result = r.Concat(val);
    else
      result = val;

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    KValue agg = eval(ki, 0, state);

    KValue result = agg.Extract(kgepi->offset*8, getWidthForLLVMType(i->getType()));

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::Fence: {
    // Ignore for now
    break;
  }
  case Instruction::InsertElement: {
    InsertElementInst *iei = cast<InsertElementInst>(i);
    KValue vec = eval(ki, 0, state);
    KValue newElt = eval(ki, 1, state);
    ref<Expr> idx = eval(ki, 2, state).value;

    ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
    if (cIdx == NULL) {
      terminateStateOnError(
          state, "InsertElement, support for symbolic index not implemented",
          Unhandled);
      return;
    }
    uint64_t iIdx = cIdx->getZExtValue();
    const llvm::VectorType *vt = iei->getType();
    unsigned EltBits = getWidthForLLVMType(vt->getElementType());

    if (iIdx >= vt->getNumElements()) {
      // Out of bounds write
      terminateStateOnError(state, "Out of bounds write when inserting element",
                            BadVectorAccess);
      return;
    }

    const unsigned elementCount = vt->getNumElements();
    llvm::SmallVector<KValue, 8> elems;
    elems.reserve(elementCount);
    for (unsigned i = elementCount; i != 0; --i) {
      auto of = i - 1;
      unsigned bitOffset = EltBits * of;
      elems.push_back(
          of == iIdx ? newElt : vec.Extract(bitOffset, EltBits));
    }

    assert(Context::get().isLittleEndian() && "FIXME:Broken for big endian");
    KValue Result = KValue::concatValues(elems);
    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::ExtractElement: {
    ExtractElementInst *eei = cast<ExtractElementInst>(i);
    KValue vec = eval(ki, 0, state);
    ref<Expr> idx = eval(ki, 1, state).value;

    ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
    if (cIdx == NULL) {
      terminateStateOnError(
          state, "ExtractElement, support for symbolic index not implemented",
          Unhandled);
      return;
    }
    uint64_t iIdx = cIdx->getZExtValue();
    const llvm::VectorType *vt = eei->getVectorOperandType();
    unsigned EltBits = getWidthForLLVMType(vt->getElementType());

    if (iIdx >= vt->getNumElements()) {
      // Out of bounds read
      terminateStateOnError(state, "Out of bounds read when extracting element",
                            BadVectorAccess);
      return;
    }

    unsigned bitOffset = EltBits * iIdx;
    KValue Result = vec.Extract(bitOffset, EltBits);
	bindLocal(ki, state, Result);
    break;
  }
  case Instruction::ShuffleVector:
    // Should never happen due to Scalarizer pass removing ShuffleVector
    // instructions.
    terminateStateOnExecError(state, "Unexpected ShuffleVector instruction");
    break;
  case Instruction::AtomicRMW:
    terminateStateOnExecError(state, "Unexpected Atomic instruction, should be "
                                     "lowered by LowerAtomicInstructionPass");
    break;
  case Instruction::AtomicCmpXchg:
    terminateStateOnExecError(state,
                              "Unexpected AtomicCmpXchg instruction, should be "
                              "lowered by LowerAtomicInstructionPass");
    break;
  // Other instructions...
  // Unhandled
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }
  
  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();

  for (std::vector<ExecutionState *>::iterator it = removedStates.begin(),
                                               ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    delete es;
  }
  removedStates.clear();

  if (searcher) {
    searcher->update(nullptr, continuedStates, pausedStates);
    pausedStates.clear();
    continuedStates.clear();
  }
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else if (const auto set = dyn_cast<SequentialType>(*ii)) {
      uint64_t elementSize = 
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index = 
          cast<ConstantExpr>(evalConstant(c).getValue())->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = 
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
    } else if (const auto ptr = dyn_cast<PointerType>(*ii)) {
      auto elementSize =
        kmodule->targetData->getTypeStoreSize(ptr->getElementType());
      auto operand = ii.getOperand();
      if (auto c = dyn_cast<Constant>(operand)) {
        auto index
            = cast<ConstantExpr>(evalConstant(c).getValue())->SExt(
                                    Context::get().getPointerWidth());
        auto addend = index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#endif
    } else
      assert("invalid type" && 0);
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (auto &kfp : kmodule->functions) {
    KFunction *kf = kfp.get();
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable =
      std::unique_ptr<Cell[]>(new Cell[kmodule->constants.size()]);
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c = evalConstant(kmodule->constants[i]);
  }
}

void Executor::checkMemoryUsage() {
  if (!MaxMemory)
    return;
  if ((stats::instructions & 0xFFFF) == 0) {
    // We need to avoid calling GetTotalMallocUsage() often because it
    // is O(elts on freelist). This is really bad since we start
    // to pummel the freelist once we hit the memory cap.
    unsigned mbs = (util::GetTotalMallocUsage() >> 20) +
                   (memory->getUsedDeterministicSize() >> 20);

    if (mbs > MaxMemory) {
      if (mbs > MaxMemory + 100) {
        // just guess at how many to kill
        unsigned numStates = states.size();
        unsigned toKill = std::max(1U, numStates - numStates * MaxMemory / mbs);
        klee_warning("killing %d states (over memory cap)", toKill);
        std::vector<ExecutionState *> arr(states.begin(), states.end());
        for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
          unsigned idx = rand() % N;
          // Make two pulls to try and not hit a state that
          // covered new code.
          if (arr[idx]->coveredNew)
            idx = rand() % N;

          std::swap(arr[idx], arr[N - 1]);
          terminateStateEarly(*arr[N - 1], "Memory limit exceeded.");
        }
      }
      atMemoryLimit = true;
    } else {
      atMemoryLimit = false;
    }
  }
}

void Executor::doDumpStates() {
  if (!DumpStatesOnHalt || states.empty())
    return;

  klee_message("halting execution, dumping remaining states");
  for (const auto &state : states)
    terminateStateEarly(*state, "Execution halting.");
  updateStates(nullptr);
}

void Executor::run(ExecutionState &initialState) {
  bindModuleConstants();

  // Delay init till now so that ticks don't accrue during optimization and such.
  timers.reset();

  states.insert(&initialState);

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    
    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(), 
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    time::Point lastTime, startTime = lastTime = time::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) {
        doDumpStates();
        return;
      }

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it = 
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc;
      stepInstruction(state);

      executeInstruction(state, ki);
      timers.invoke();
      if (::dumpStates) dumpStates();
      if (::dumpPTree) dumpPTree();
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        const auto time = time::getWallTime();
        const time::Span seedTime(SeedTime);
        if (seedTime && time > startTime + seedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time - lastTime >= time::seconds(10)) {
          lastTime = time;
          lastNumSeeds = numSeeds;          
          klee_message("%d seeds remaining over: %d states", 
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed) {
      doDumpStates();
      return;
    }
  }

  searcher = constructUserSearcher(*this);

  std::vector<ExecutionState *> newStates(states.begin(), states.end());
  searcher->update(0, newStates, std::vector<ExecutionState *>());

  while (!states.empty() && !haltExecution) {
    ExecutionState &state = searcher->selectState();
    KInstruction *ki = state.pc;
    stepInstruction(state);

    executeInstruction(state, ki);
    timers.invoke();
    if (::dumpStates) dumpStates();
    if (::dumpPTree) dumpPTree();

    checkMemoryUsage();

    updateStates(&state);
  }

  delete searcher;
  searcher = 0;

  doDumpStates();
}

std::string Executor::getKValueInfo(ExecutionState &state,
                                     const KValue &address) const{
  std::string Str;
  llvm::raw_string_ostream info(Str);
  info << "\taddress: " << address.getSegment() << ":" << address.getOffset() << "\n";
  ref<ConstantExpr> segmentValue;
  ref<ConstantExpr> offsetValue;
  if (address.isConstant()) {
    segmentValue = cast<ConstantExpr>(address.getSegment());
    offsetValue = cast<ConstantExpr>(address.getOffset());
  } else {
    bool success = solver->getValue(state, address, segmentValue, offsetValue);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    info << "\texample: " << segmentValue->getZExtValue()
        << ":" << offsetValue->getZExtValue() << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address.getSegment());
    info << "\tsegment range: [" << res.first << ", " << res.second <<"]\n";
    res = solver->getRange(state, address.getOffset());
    info << "\toffset range: [" << res.first << ", " << res.second <<"]\n";
  }
  
  ObjectPair op;
  bool success = state.addressSpace.resolveOneConstantSegment(
      KValue(segmentValue, offsetValue), op);
  info << "\tpointing to: ";
  if (!success) {
    info << "none\n";
  } else {
    const MemoryObject *mo = op.first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->getSegmentString() << " of size "
         << mo->getSizeString() << "\n"
         << "\t\t" << alloc_info << "\n";
  }

  return info.str();
}

void Executor::pauseState(ExecutionState &state) {
  auto it = std::find(continuedStates.begin(), continuedStates.end(), &state);
  // If the state was to be continued, but now gets paused again
  if (it != continuedStates.end()) {
    // ...just don't continue it
    std::swap(*it, continuedStates.back());
    continuedStates.pop_back();
  } else {
    pausedStates.push_back(&state);
  }
}

void Executor::continueState(ExecutionState &state){
  auto it = std::find(pausedStates.begin(), pausedStates.end(), &state);
  // If the state was to be paused, but now gets continued again
  if (it != pausedStates.end()){
    // ...don't pause it
    std::swap(*it, pausedStates.back());
    pausedStates.pop_back();
  } else {
    continuedStates.push_back(&state);
  }
}

void Executor::terminateState(ExecutionState &state) {
  if (replayKTest && replayPosition!=replayKTest->numObjects) {
    klee_warning_once(replayKTest,
                      "replay did not consume all objects in test input.");
  }

  interpreterHandler->incPathsExplored();

  std::vector<ExecutionState *>::iterator it =
      std::find(addedStates.begin(), addedStates.end(), &state);
  if (it==addedStates.end()) {
    state.pc = state.prevPC;

    removedStates.push_back(&state);
  } else {
    // never reached searcher, just delete immediately
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(&state);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    addedStates.erase(it);
    processTree->remove(state.ptreeNode);
    delete &state;
  }
}

void Executor::terminateStateEarly(ExecutionState &state, 
                                   const Twine &message) {
  if (ExitOnErrorType.empty() &&
      (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state))))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  terminateState(state);
}

static bool hasMemoryLeaks(ExecutionState &state) {
    for (auto& object : state.addressSpace.objects) {
        if (!object.first->isLocal && !object.first->isGlobal
            && !object.first->isFixed) {
            // => is heap-allocated
            return true;
        }
    }

    return false;
}

static std::vector<const MemoryObject *> getMemoryLeaks(ExecutionState &state) {
    std::vector<const MemoryObject *> leaks;
    for (auto& object : state.addressSpace.objects) {
        if (!object.first->isLocal && !object.first->isGlobal
            && !object.first->isFixed) {
            // => is heap-allocated
            leaks.push_back(object.first);
        }
    }

    return leaks;
}

static void getPointers(const llvm::Type *type,
                        const llvm::DataLayout& DL,
                        const ObjectState *os,
                        std::set<ref<Expr>>& objects,
                        unsigned off=0) {
  using namespace llvm;

  const auto ptrWidth = Context::get().getPointerWidth();

  // XXX: we ignore integer types which is wrong since
  // we can cast pointer to integer... we should actually search
  // any object that has segment plane set
  for (auto *Ty : type->subtypes()) {
      if (Ty->isStructTy()) {
          getPointers(Ty, DL, os, objects, off);
      } else if (auto AT = dyn_cast<ArrayType>(Ty)) {
          if (AT->getElementType()->isIntegerTy())
              continue; // we cannot find anything here

          // we must search on all indices in the array,
          // so just artificially shift offsets
          for (unsigned idx = 0; idx < AT->getNumElements(); ++idx) {
              getPointers(Ty, DL, os, objects,
                          off + idx*DL.getTypeAllocSize(AT->getElementType()));
          }
      } if (Ty->isPointerTy()) {
        KValue ptr = os->read(off, ptrWidth);
       //llvm::errs() << "TY @ " << off << ": " << *Ty << "\n";
       //llvm::errs() << "  --> POINTER!: " << *Ty << "\n";
       //llvm::errs() << "  --> " << ptr << "\n";
        objects.insert(ptr.getSegment());
      }
      // FIXME: is this always enough? Does this cover padding
      // in any structure?
      off += DL.getTypeAllocSize(Ty);
  }
}

std::set<const MemoryObject *>
Executor::getReachableMemoryObjects(ExecutionState &state) {
    std::set<const MemoryObject *> reachable;
    std::set<ObjectPair> queue;

    DataLayout& DL = *kmodule->targetData.get();

    for (auto& object : state.addressSpace.objects) {
      // the only objects that are still left are those that
      // are either local to main or global (or heap-allocated,
      // but we do not care about those while initializing queue)
      if (object.first->isLocal || object.first->isGlobal) {

        reachable.insert(object.first);

        if (!object.first->allocSite ||
            (!llvm::isa<llvm::AllocaInst>(object.first->allocSite) &&
            !llvm::isa<llvm::GlobalValue>(object.first->allocSite))){
          continue;
        }

        queue.insert(object);
      }
    }

    // iterate the search until we searched all the reachable objects
    while (!queue.empty()) {
      ObjectPair object = *queue.begin();
      queue.erase(queue.begin());

      if (!object.first->allocSite ||
          (!llvm::isa<llvm::AllocaInst>(object.first->allocSite) &&
          !llvm::isa<llvm::GlobalValue>(object.first->allocSite))){
        continue;
      }

      std::set<ref<Expr>> segments;
      getPointers(object.first->allocSite->getType(), DL,
                  &*object.second, segments);

      for (auto segment : segments) {
        segment = toUnique(state, segment);
        if (auto C = dyn_cast<ConstantExpr>(segment)) {
          if (C->getZExtValue() < FIRST_ORDINARY_SEGMENT)
              continue; // ignore functions and special objects

          ObjectPair result;
          bool success =
          state.addressSpace.resolveOneConstantSegment(
              KValue(segment, ConstantExpr::alloc(0, Expr::Int64)), result);
          if (success) {
              if (reachable.insert(result.first).second) {
                  // if we haven't found this memory before,
                  // add it to queue for processing
                  queue.insert(result);
              }
          } else {
              klee_warning("Failed resolving segment in memcleanup check");
          }
        } else {
          klee_warning("Cannot resolve non-constant segment in memcleanup check");
        }
      }
    }

    return reachable;
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if ((CheckLeaks || CheckMemCleanup) && hasMemoryLeaks(state)) {
    if (CheckMemCleanup) {
      auto leaks = getMemoryLeaks(state);
      assert(!leaks.empty() && "hasMemoryLeaks() bug");
      std::string info = "";
      for (const auto mo : leaks) {
      info += getKValueInfo(state, mo->getPointer());
      }
      terminateStateOnError(state, "memory error: memory not cleaned up",
                            Leak, nullptr, info);
    } else {
      assert(CheckLeaks);
      auto leaks = getMemoryLeaks(state);
      assert(!leaks.empty() && "hasMemoryLeaks() bug");

      klee_warning("Found unfreed memory, checking if it still can be freed.");

      auto reach = getReachableMemoryObjects(state);
      for (auto *leak : leaks) {
        if (reach.count(leak) == 0) {
          std::string info = getKValueInfo(state, leak->getPointer());
          terminateStateOnError(state, "memory error: memory leak detected",
                                Leak, nullptr, info);
          return;
        }
      }

      // all good, just terminate the state
      terminateState(state);
    }
  } else {
    if (ExitOnErrorType.empty() &&
        (!OnlyOutputStatesCoveringNew || state.coveredNew ||
        (AlwaysOutputSeeds && seedMap.count(&state))))
      interpreterHandler->processTestCase(state, 0, 0);
    terminateState(state);
  }
}

const InstructionInfo & Executor::getLastNonKleeInternalInstruction(const ExecutionState &state,
    Instruction ** lastInstruction) {
  // unroll the stack of the applications state and find
  // the last instruction which is not inside a KLEE internal function
  ExecutionState::stack_ty::const_reverse_iterator it = state.stack.rbegin(),
      itE = state.stack.rend();

  // don't check beyond the outermost function (i.e. main())
  itE--;

  const InstructionInfo * ii = 0;
  if (kmodule->internalFunctions.count(it->kf->function) == 0){
    ii =  state.prevPC->info;
    *lastInstruction = state.prevPC->inst;
    //  Cannot return yet because even though
    //  it->function is not an internal function it might of
    //  been called from an internal function.
  }

  // Wind up the stack and check if we are in a KLEE internal function.
  // We visit the entire stack because we want to return a CallInstruction
  // that was not reached via any KLEE internal functions.
  for (;it != itE; ++it) {
    // check calling instruction and if it is contained in a KLEE internal function
    const Function * f = (*it->caller).inst->getParent()->getParent();
    if (kmodule->internalFunctions.count(f)){
      ii = 0;
      continue;
    }
    if (!ii){
      ii = (*it->caller).info;
      *lastInstruction = (*it->caller).inst;
    }
  }

  if (!ii) {
    // something went wrong, play safe and return the current instruction info
    *lastInstruction = state.prevPC->inst;
    return *state.prevPC->info;
  }
  return *ii;
}

bool Executor::shouldExitOn(enum TerminateReason termReason) {
  std::vector<TerminateReason>::iterator s = ExitOnErrorType.begin();
  std::vector<TerminateReason>::iterator e = ExitOnErrorType.end();

  for (; s != e; ++s)
    if (termReason == *s)
      return true;

  return false;
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     enum TerminateReason termReason,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;
  Instruction * lastInst;
  const InstructionInfo &ii = getLastNonKleeInternalInstruction(state, &lastInst);

  if (shouldExitOn(termReason))
    haltExecution = true;

  bool notemitted = emittedErrors.insert(std::make_pair(lastInst, message)).second;

  // give a message about found error
  if (EmitAllErrors || notemitted) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: (location information missing) %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");
  }

  // process the testcase if we either should emit all errors, or if we search
  // for a specific error and this is the error (haltExecution is set to true),
  // or if we do not search for a specific error and we haven't emitted this error yet
  if (EmitAllErrors || haltExecution || (ExitOnErrorType.empty() && notemitted)) {
    std::string MsgString;
    llvm::raw_string_ostream msg(MsgString);
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
      msg << "assembly.ll line: " << ii.assemblyLine << "\n";
    }
    msg << "Stack: \n";
    state.dumpStack(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;

    std::string suffix_buf;
    if (!suffix) {
      suffix_buf = TerminateReasonNames[termReason];
      suffix_buf += ".err";
      suffix = suffix_buf.c_str();
    }

    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }

  terminateState(state);
}

// XXX shoot me
static const char *okExternalsList[] = { "printf",
                                         "fprintf",
                                         "puts",
                                         "strstr",
                                         "putchar",
                                         "__ctype_b_loc",
                                         "rint",
                                         "rintf",
                                         "rintl",
                                         "lrint",
                                         "lrintf",
                                         "lrintl",
                                         "llrint",
                                         "llrintf",
                                         "llrintl",
                                         "nearbyint",
                                         "nearbyintf",
                                         "nearbyintl",
                                         "remainder",
                                         "remainderf",
                                         "remainderl",
                                         "drem",
                                         "dremf",
                                         "dreml",
                                         "trunc",
                                         "truncf",
                                         "truncl",
                                         "ceil",
                                         "ceill",
                                         "ceilf",
                                         "floor",
                                         "floorf",
                                         "floorl",
                                         "trunc",
                                         "truncl",
                                         "truncf",
                                         "nan",
                                         "nanf",
                                         "nanl",
                                         "fmax",
                                         "fmaxf",
                                         "fmaxl",
                                         "frexp",
                                         "ldexp",
                                         "fabsf",
                                         "fdim",
                                         "fdiml",
                                         "fdimf",
                                         "fmin",
                                         "fminf",
                                         "fminl",
                                         "fmaxf",
                                         "fmaxl",
                                         "modf",
                                         "modff",
                                         "modfl",
                                         "copysign",
                                         "copysignf",
                                         "copysignl",
                                         "__isnan",
                                         "__isnanf",
                                         "__isnanl",
                                         "__isinf",
                                         "__isinff",
                                         "__isinfl",
                                         "__fpclassify",
                                         "__fpclassifyf",
                                         "__fpclassifyl",
                                         "__signbit",
                                         "__signbitf",
                                         "__signbitl",
                                         "__finite",
                                         "__finite1",
                                         "__finitef",
                                         "lround",
                                         "lroundf",
                                         "lroundl",
                                         "llround",
                                         "llroundf",
                                         "llroundl",
                                         "round",
                                         "roundf",
                                         "roundl",
                                         "fmod",
                                         "fmodf",
                                         "memcpy",
                                         "memmove",
                                         "memcmp",
                                         "memset",
                                         "fmodl",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList + 
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

// these are not and may introduce incorrect results.
// Fail on these if the policy is Pure (for None, the call will fail anyway
// and for All... well, the user wanted that...)
static std::set<std::string> nokExternals({"fesetround", "fesetenv",
                                           "feenableexcept", "fedisableexcept",
                                           "feupdateenv", "fesetexceptflag",
                                           "feclearexcept", "feraiseexcept"});

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    const std::vector<Cell> &arguments) {
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;

  if (ExternalCalls == ExternalCallPolicy::Pure &&
      nokExternals.count(function->getName()) > 0) {
    terminateStateOnError(state, "failed external call", User);
    return;
  }

  if (ExternalCalls == ExternalCallPolicy::None
      && !okExternals.count(function->getName())) {
    klee_warning("Disallowed call to external function: %s\n",
               function->getName().str().c_str());
    terminateStateOnError(state, "external calls disallowed", User);
    return;
  }

  if (ExternalCalls == ExternalCallPolicy::Pure
      && !okExternals.count(function->getName())) {

    auto retTy = function->getReturnType();
    if (retTy->isVoidTy()) {
        //klee_warning_once(target, "Skipping call of undefined function: %s",
        //                  function->getName().str().c_str());
        return;
    }

    // the function returns something

    DataLayout& DL = *kmodule->targetData.get();
    auto size = DL.getTypeAllocSizeInBits(retTy);
    if (size > 64) {
        klee_warning_once(target, "Undefined function returns > 64bit object: %s",
                          function->getName().str().c_str());
        terminateStateOnError(state, "failed external call", User);
    } else {
        bool isPointer = false;
        if (retTy->isPointerTy()) {
            isPointer = true;
            klee_warning_once(target, "Returning nondet pointer: %s",
                             function->getName().str().c_str());
        }
        auto nv = createNondetValue(state, size, false,
                                    target, function->getName().str(),
                                    isPointer);
        bindLocal(target, state, nv);
        klee_warning_once(target, "Assume that the undefined function %s is pure",
                          function->getName().str().c_str());
    }
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  SegmentAddressMap resolvedMOs;
  for (std::vector<Cell>::const_iterator ai = arguments.begin(),
       ae = arguments.end(); ai!=ae; ++ai) {
    uint64_t address = 0;
    if (ExternalCalls == ExternalCallPolicy::All) { // don't bother checking uniqueness
      auto value = optimizer.optimizeExpr(ai->getValue(), true);
      ref<ConstantExpr> ce;
      // TODO segment
      bool success = solver->getValue(state, value, ce);
      assert(success && "FIXME: Unhandled solver failure");
      ce->toMemory(&args[wordIndex]);
      ObjectPair op;
      // Checking to see if the argument is a pointer to something
      if (ce->getWidth() == Context::get().getPointerWidth()) {
        Optional<uint64_t> temp;
        state.addressSpace.resolveOne(state, solver, *ai, op, success, temp);
        if (success) {
          auto found = state.addressSpace.resolveInConcreteMap(
              op.first->segment, address);
          if (!found) {
            void *addr = memory->allocateMemory(
                op.first->allocatedSize,
                getAllocationAlignment(op.first->allocSite));
            if (!addr)
              klee_error("Couldn't allocate memory for external function");
            address = reinterpret_cast<uint64_t>(addr);
          }
          resolvedMOs.insert({op.first->segment, address});

          if (op.second->getSizeBound() == 0 ||
              (op.second->getSizeBound() > op.first->allocatedSize)) {
            terminateStateOnExecError(
                state, "external call with symbolic-sized object that "
                       "has no real virtual process memory: " +
                           function->getName());
            return;
          }
          op.second->flushToConcreteStore(solver, state);
        }
      }
      wordIndex += (ce->getWidth() + 63) / 64;
    } else {
      // we are allowed external calls with concrete arguments only
      auto segmentExpr = toUnique(state, ai->getSegment());
      if (!isa<ConstantExpr>(segmentExpr)) {
        terminateStateOnExecError(
            state, "external call with symbolic segment argument: " +
                       function->getName());
        return;
      }
      ObjectPair op;
      if (!segmentExpr->isZero() ||
          ai->getOffset()->getWidth() == Context::get().getPointerWidth()) {

        bool success;
        Optional<uint64_t> temp;
        state.addressSpace.resolveOne(state, solver, *ai, op, success, temp);
        if (success) {
          auto found = state.addressSpace.resolveInConcreteMap(
              op.first->segment, address);
          if (!found) {
            void *addr = memory->allocateMemory(
                op.first->allocatedSize,
                getAllocationAlignment(op.first->allocSite));
            if (!addr)
              klee_error("Couldn't allocate memory for external function");
            address = reinterpret_cast<uint64_t>(addr);
          }

          resolvedMOs.insert({op.first->segment, address});

          if (op.second->getSizeBound() == 0 ||
              (op.second->getSizeBound() > op.first->allocatedSize)) {
            terminateStateOnExecError(
                state, "external call with symbolic-sized object that "
                       "has no real virtual process memory: " +
                           function->getName());
            return;
          }

          klee_warning(
              "passing pointer to external call, may not work properly");
        }
      }

      ref<Expr> arg;
      // if no MO was found, use ai value
      if (address) {
        arg = toUnique(state,
                       ConstantExpr::create(reinterpret_cast<uint64_t>(address),
                                            Context::get().getPointerWidth()));
      } else {
        arg = toUnique(state, ai->getValue());
      }
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth() + 63) / 64;
      } else {
        terminateStateOnExecError(state,
                                  "external call with symbolic argument: " +
                                      function->getName());
        return;
      }
    }
  }

  // Prepare external memory for invoking the function
  state.addressSpace.copyOutConcretes(resolvedMOs, true);
#ifndef WINDOWS
  // Update external errno state with local state value
  int *errno_addr = getErrnoLocation(state);

  ObjectPair result;
  auto segment = ConstantExpr::create(ERRNO_SEGMENT, Expr::Int64);
  auto offset = ConstantExpr::create(0, Context::get().getPointerWidth());
  Optional<uint64_t> temp;
  bool resolved;
  state.addressSpace.resolveOne(state, solver, KValue(segment, offset), result,
                                resolved, temp);
  if (temp)
    offset =
        ConstantExpr::create(temp.getValue(), Context::get().getPointerWidth());
  if (!resolved)
    klee_error("Could not resolve memory object for errno");

  auto errnoValue = ConstantExpr::create(errno, sizeof(*errno_addr) * 8);

  externalDispatcher->setLastErrno(
      errnoValue->getZExtValue(sizeof(*errno_addr) * 8));
#endif

  if (!SuppressExternalWarnings) {

    std::string TmpStr;
    llvm::raw_string_ostream os(TmpStr);
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i = 0; i < arguments.size(); i++) {
      if (arguments[i].value->isZero()) {
        os << "segment: " << arguments[i].pointerSegment;
      } else {
        os << "value/address: " << arguments[i].value;
      }
      if (i != arguments.size() - 1)
        os << ", ";
    }
    os << ") at " << state.pc->getSourceLocation();

    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }

  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          External);
    return;
  }

  if (!state.addressSpace.copyInConcretes(resolvedMOs, state, solver)) {
    terminateStateOnError(state, "external modified read-only object",
                          External);
    return;
  }

#ifndef WINDOWS
  // Update errno memory object with the errno value from the call
  int error = externalDispatcher->getLastErrno();
  state.addressSpace.copyInConcrete(result.first, result.second,
                                    (uint64_t)&error, state, solver);
#endif

  Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(function->getContext())) {
    KValue value;
    ref<Expr> returnVal =
        ConstantExpr::fromMemory((void *)args, getWidthForLLVMType(resultType));
    if (returnVal->getWidth() == Context::get().getPointerWidth()) {
      ResolutionList rl;
      Optional<uint64_t> calculatedOffset;
      state.addressSpace.resolveAddressWithOffset(state, solver, returnVal, rl,
                                                  calculatedOffset);

      if (rl.size() == 1) {
        value = KValue(rl[0].first->getSegmentExpr(),
                       ConstantExpr::alloc(calculatedOffset.getValue(),
                                           Context::get().getPointerWidth()));
      } else {
        value = returnVal;
      }

    } else {
      value = returnVal;
    }
    bindLocal(target, state, value);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state,
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayKTest || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason to?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() % n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.
  
  static unsigned id;
  const Array *array =
      arrayCache.CreateArray("rrws_arr" + llvm::utostr(++id),
                             Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  llvm::errs() << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state, 
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack.back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom,
                            size_t allocationAlignment) {
  size = optimizer.optimizeExpr(size, true);
  const llvm::Value *allocSite = state.prevPC->inst;
  if (allocationAlignment == 0) {
    allocationAlignment = getAllocationAlignment(allocSite);
  }
  MemoryObject *mo =
      memory->allocate(size, isLocal, /*isGlobal=*/false,
                       allocSite, allocationAlignment);
  if (!mo) {
    bindLocal(target, state, 
              KValue(ConstantExpr::alloc(0, Context::get().getPointerWidth())));
  } else {
    bindLocal(target, state, mo->getPointer());
    if (!reallocFrom) {
      ObjectState *os = bindObjectInState(state, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
    } else {
      ObjectState *os = new ObjectState(*reallocFrom, mo);
      state.addressSpace.unbindObject(reallocFrom->getObject());
      state.addressSpace.bindObject(mo, os);
    }
  }
}

void Executor::executeFree(ExecutionState &state,
                           const KValue &address,
                           KInstruction *target) {
  auto addressOptim
    = KValue(address.getSegment(),
             optimizer.optimizeExpr(address.getOffset(), true));

  StatePair zeroPointer = fork(state, addressOptim.createIsZero(), true);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, KValue(Expr::createPointer(0)));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, addressOptim, rl, "free");
    
    for (Executor::ExactResolutionList::iterator it = rl.begin(), 
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, "free of alloca", Free, NULL,
                              getKValueInfo(*it->second, addressOptim));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, "free of global", Free, NULL,
                              getKValueInfo(*it->second, addressOptim));
      } else {
        it->second->addressSpace.unbindObject(mo);
        if (target)
          bindLocal(target, *it->second, KValue(Expr::createPointer(0)));
      }
    }
  }
}

void Executor::resolveExact(ExecutionState &state, const KValue &address,
                            ExactResolutionList &results,
                            const std::string &name) {
  auto optimAddress = KValue(address.getSegment(),
                             optimizer.optimizeExpr(address.getOffset(), true));
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace.resolve(state, solver, optimAddress, rl);
  
  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); 
       it != ie; ++it) {
    ref<Expr> inBounds = optimAddress.Eq(it->first->getPointer()).getValue();
    
    StatePair branches = fork(*unbound, inBounds, true);
    
    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    terminateStateOnError(*unbound, "memory error: invalid pointer: " + name,
                          Ptr, NULL, getKValueInfo(*unbound, optimAddress));
  }
}

void Executor::executeMemoryRead(ExecutionState &state, const KValue &address,
                                 KInstruction *target) {
  executeMemoryOperation(state, false, address, KValue(), target);
}

void Executor::executeMemoryWrite(ExecutionState &state, const KValue &address,
                                  const KValue &value) {
  executeMemoryOperation(state, true, address, value, 0);
}
void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      KValue address,
                                      KValue value, /* undef if read */
                                      KInstruction *target /* undef if write */) {
  Expr::Width type = (isWrite ? value.getWidth() :
                     getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    address = KValue(state.constraints.simplifyExpr(address.getSegment()),
                     state.constraints.simplifyExpr(address.getOffset()));
    if (isWrite) {
      value = KValue(state.constraints.simplifyExpr(value.getSegment()),
                     state.constraints.simplifyExpr(value.getOffset()));
    }
  }

  address = KValue(address.getSegment(),
                   optimizer.optimizeExpr(address.getOffset(), true));

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(coreSolverTimeout);
  llvm::Optional<uint64_t> offsetVal;
  if (!state.addressSpace.resolveOne(state, solver, address, op, success,
                                     offsetVal)) {
    address =
        KValue(toConstant(state, address.getSegment(), "resolveOne failure"),
               toConstant(state, address.getOffset(), "resolveOne failure"));
    success = state.addressSpace.resolveOneConstantSegment(address, op);
  }
  solver->setTimeout(time::Span());

  if (success) {
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize &&
        (!isa<ConstantExpr>(mo->size) ||
         cast<ConstantExpr>(mo->size)->getZExtValue() >= MaxSymArraySize)) {
      address =
          KValue(toConstant(state, address.getSegment(), "max-sym-array-size"),
                 toConstant(state, address.getOffset(), "max-sym-array-size"));
    }
    ref<Expr> offset;
    ref<Expr> segment;
    if (offsetVal) {
      segment = ConstantExpr::alloc(mo->segment, Expr::Int64);
      offset = ConstantExpr::alloc(offsetVal.getValue(),
                                   Context::get().getPointerWidth());
    } else {
      segment = address.getSegment();
      offset = address.getOffset();
    }

    ref<Expr> isEqualSegment = EqExpr::create(mo->getSegmentExpr(), segment);

    ref<Expr> isOffsetInBounds = mo->getBoundsCheckOffset(offset, bytes);
    isOffsetInBounds = optimizer.optimizeExpr(isOffsetInBounds, true);

    bool inBoundsOffset;
    bool inBoundsSegment;
    solver->setTimeout(coreSolverTimeout);
    bool successSegment =
        solver->mustBeTrue(state, isEqualSegment, inBoundsSegment);
    bool success = solver->mustBeTrue(state, isOffsetInBounds, inBoundsOffset);
    solver->setTimeout(time::Span());
    if (!success || !successSegment) {
      state.pc = state.prevPC;
      terminateStateEarly(state, "Query timed out (bounds check).");
      return;
    }

    if (inBoundsSegment && inBoundsOffset) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = state.addressSpace.getWriteable(mo, os);
          wos->write(offset, value);
        }          
      } else {
        KValue result = os->read(offset, type);
        
        if (interpreterOpts.MakeConcreteSymbolic) {
          result = KValue(replaceReadWithSymbolic(state, result.getSegment()),
                          replaceReadWithSymbolic(state, result.getOffset()));
        }

        bindLocal(target, state, result);
      }

      return;
    }
  } 

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)

  auto optimAddress = KValue(address.getSegment(),
                             optimizer.optimizeExpr(address.getOffset(), true));
  ResolutionList rl;  
  solver->setTimeout(coreSolverTimeout);
  bool incomplete = state.addressSpace.resolve(state, solver, optimAddress, rl,
                                               0, coreSolverTimeout);
  solver->setTimeout(time::Span());
  
  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;
  
  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(optimAddress, bytes);
    
    StatePair branches = fork(*unbound, inBounds, true);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped 
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
          // TODO segment
          wos->write(optimAddress.getOffset(), value);
        }
      } else {
        KValue result = os->read(optimAddress.getOffset(), type);
        bindLocal(target, *bound, result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }

  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "Query timed out (resolve).");
    } else {
      terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr,
                            NULL, getKValueInfo(*unbound, optimAddress));
    }
  }
}

KValue Executor::createNondetValue(ExecutionState &state,
                                   unsigned size, bool isSigned,
                                   KInstruction *kinst,
                                   const std::string &name,
                                   bool isPointer) {
  assert(!replayKTest);
  // Find a unique name for this array.  First try the original name,
  // or if that fails try adding a unique identifier.
  unsigned id = 0;
  std::string uniqueName = name;
  while (!state.arrayNames.insert(uniqueName).second) {
    uniqueName = name + "_" + llvm::utostr(++id);
  }

  KValue kval;
  const Array *array = arrayCache.CreateArray(uniqueName, size);
  auto expr = Expr::createTempRead(array, size);

  if (isPointer) {
    assert(!isSigned && "Got signed pointer");
    std::string offName = uniqueName + "_off";
    bool had = state.arrayNames.insert(offName).second;
    assert(had && "Already had a unique name");
    (void)had;

    const Array *offarray
        = arrayCache.CreateArray(offName, Context::get().getPointerWidth());
    auto offexpr = Expr::createTempRead(offarray, size);
    kval = {expr, offexpr};
  } else {
    kval = expr;
  }

  auto& nv = state.addNondetValue(kval, isSigned, name);
  nv.kinstruction = kinst;

  return kval;
}


void Executor::executeMakeSymbolic(ExecutionState &state, 
                                   const MemoryObject *mo,
                                   const std::string &name) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayKTest) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
    // TODO fix seeding fo symbolic sizes
    unsigned size = 0;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
      size = CE->getZExtValue();
    }
    const Array *array = arrayCache.CreateArray(uniqueName, size);
    bindObjectInState(state, mo, false, array);
    state.addSymbolic(mo, array);
    
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, "ran out of inputs during seeding",
                                  User);
            break;
          }
        } else {
          if (obj->numBytes != size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < size) ||
               (!AllowSeedTruncation && obj->numBytes > size))) {
	    std::stringstream msg;
	    msg << "replace size mismatch: "
		<< mo->name << "[" << size << "]"
		<< " vs " << obj->name << "[" << obj->numBytes << "]"
		<< " in test\n";

            terminateStateOnError(state, msg.str(), User);
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes, 
                          obj->bytes + std::min(obj->numBytes, size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, mo, false);
    if (replayPosition >= replayKTest->numObjects) {
      terminateStateOnError(state, "replay count mismatch", User);
    } else {
      KTestObject *obj = &replayKTest->objects[replayPosition++];
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
        unsigned size = CE->getZExtValue();
        if (obj->numBytes != size) {
          terminateStateOnError(state, "replay size mismatch", User);
        } else {
          for (unsigned i=0; i<size; i++)
            // TODO segment
            os->write8(i, 0, obj->bytes[i]);
        }
      } else {
        terminateStateOnError(state, "symbolic size object in replay", User);
      }
    }
  }
}

void Executor::executeMakeConcrete(ExecutionState &state, 
                                   const MemoryObject *mo,
                                   const std::vector<unsigned char>& data) {
  // Create a new object state for the memory object (instead of a copy).
  ObjectState *os = bindObjectInState(state, mo, false);
  // FIXME: check size of the object
  unsigned i = 0;
  for (unsigned char byte : data) 
    os->write8(i++, 0, byte);
}


/***/

void Executor::runFunctionAsMain(Function *f,
				 int argc,
				 char **argv,
				 char **envp) {
  std::vector<KValue> arguments;

  // force deterministic initialization of memory objects
  srand(1);
  srandom(1);
  
  MemoryObject *argvMO = 0;

  // In order to make uclibc happy and be closer to what the system is
  // doing we lay out the environments at the end of the argv array
  // (both are terminated by a null). There is also a final terminating
  // null that uclibc seems to expect, possibly the ELF header?

  int envc;
  for (envc=0; envp[envc]; ++envc) ;

  unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
  KFunction *kf = kmodule->functionMap[f];
  assert(kf);
  Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
  if (ai!=ae) {
    arguments.push_back(KValue(ConstantExpr::alloc(argc, Expr::Int32)));
    if (++ai!=ae) {
      Instruction *first = &*(f->begin()->begin());
      argvMO =
          memory->allocate((argc + 1 + envc + 1 + 1) * NumPtrBytes,
                           /*isLocal=*/false, /*isGlobal=*/true,
                           /*allocSite=*/first, /*alignment=*/8);

      if (!argvMO)
        klee_error("Could not allocate memory for function arguments");

      arguments.push_back(argvMO->getPointer());

      if (++ai!=ae) {
        arguments.push_back(argvMO->getPointer((argc + 1) * NumPtrBytes));

        if (++ai!=ae)
          klee_error("invalid main function (expect 0-3 arguments)");
      }
    }
  }

  ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);
  
  if (pathWriter) 
    state->pathOS = pathWriter->open();
  if (symPathWriter) 
    state->symPathOS = symPathWriter->open();


  if (statsTracker)
    statsTracker->framePushed(*state, 0);

  assert(arguments.size() == f->arg_size() && "wrong number of arguments");
  for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
    bindArgument(kf, i, *state, arguments[i]);

  if (argvMO) {
    ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

    for (int i=0; i<argc+1+envc+1+1; i++) {
      if (i==argc || i>=argc+1+envc) {
        // Write NULL pointer
        argvOS->write(i * NumPtrBytes, KValue(Expr::createPointer(0)));
      } else {
        char *s = i<argc ? argv[i] : envp[i-(argc+1)];
        int j, len = strlen(s);

        MemoryObject *arg =
            memory->allocate(len + 1, /*isLocal=*/false, /*isGlobal=*/true,
                             /*allocSite=*/state->pc->inst, /*alignment=*/8);
        if (!arg)
          klee_error("Could not allocate memory for function arguments");
        ObjectState *os = bindObjectInState(*state, arg, false);
        for (j=0; j<len+1; j++)
          os->write8(j, 0, s[j]);

        // Write pointer to newly allocated and initialised argv/envp c-string
        argvOS->write(i * NumPtrBytes, arg->getPointer());
      }
    }
  }
  
  initializeGlobals(*state);

  processTree = std::make_unique<PTree>(state);
  run(*state);
  processTree = nullptr;

  // hack to clear memory objects
  delete memory;
  memory = new MemoryManager(nullptr, NumPtrBytes * 8);

  globalObjects.clear();
  globalAddresses.clear();

  if (statsTracker)
    statsTracker->done();
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state, std::string &res,
                                Interpreter::LogType logFormat) {

  switch (logFormat) {
  case STP: {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } break;

  case KQUERY: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprPPrinter::printConstraints(info, state.constraints);
    res = info.str();
  } break;

  case SMTLIB2: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprSMTLIBPrinter printer;
    printer.setOutput(info);
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    printer.setQuery(query);
    printer.generateOutput();
    res = info.str();
  } break;

  default:
    klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {

  solver->setTimeout(coreSolverTimeout);

  ExecutionState tmp(state);

  // Go through each byte in every test case and attempt to restrict
  // it to the constraints contained in cexPreferences.  (Note:
  // usually this means trying to make it an ASCII character (0-127)
  // and therefore human readable. It is also possible to customize
  // the preferred constraints.  See test/Features/PreferCex.c for
  // an example) While this process can be very expensive, it can
  // also make understanding individual test cases much easier.
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    std::vector< ref<Expr> >::const_iterator pi = 
      mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
    for (; pi != pie; ++pi) {
      bool mustBeTrue;
      // Attempt to bound byte to constraints held in cexPreferences
      bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), 
					mustBeTrue);
      // If it isn't possible to constrain this particular byte in the desired
      // way (normally this would mean that the byte can't be constrained to
      // be between 0 and 127 without making the entire constraint list UNSAT)
      // then just continue on to the next byte.
      if (!success) break;
      // If the particular constraint operated on in this iteration through
      // the loop isn't implied then add it to the list of constraints.
      if (!mustBeTrue) tmp.addConstraint(*pi);
    }
    if (pi!=pie) break;
  }

  // try to minimize sizes of symbolic-size objects
  std::vector<uint64_t> sizes;
  sizes.reserve(state.symbolics.size());
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
      sizes.push_back(CE->getZExtValue());
    } else {
      auto pair = solver->getRange(tmp, mo->size);
      sizes.push_back(pair.first->getZExtValue());
      tmp.addConstraint(EqExpr::create(mo->size, pair.first));
    }
  }

  std::vector< std::vector<unsigned char> > values;
  std::shared_ptr<const Assignment> assignment(0);
  if (!state.symbolics.empty()) {
    bool success = solver->getInitialValues(tmp, assignment);
    solver->setTimeout(time::Span());
    if (!success) {
      klee_warning("unable to compute initial values (invalid constraints?)!");
      ExprPPrinter::printQuery(llvm::errs(), state.constraints,
                               ConstantExpr::alloc(0, Expr::Bool));
      return false;
    }
  }
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    const Array *array = state.symbolics[i].second;
    std::vector<uint8_t> data;
    data.reserve(sizes[i]);
    if (auto vals = assignment->getBindingsOrNull(array)) {
      data = vals->asVector();
    }
    data.resize(sizes[i]);
    res.push_back(std::make_pair(mo->name, data));
  }

  // try to minimize the found values
  // We cannot use getTestVector(), as the values in .ktest
  // have different endiandness (byte 0 goes first, then byte 1, etc.)
  for (auto& it : tmp.nondetValues) {
    auto pair = solver->getRange(tmp, it.value.getValue());
    auto value = pair.first;
    tmp.addConstraint(EqExpr::create(it.value.getValue(), value));

    pair = solver->getRange(tmp, it.value.getSegment());
    auto segment = pair.first;
    tmp.addConstraint(EqExpr::create(it.value.getSegment(), segment));

    std::string descr = it.name;
    if (it.kinstruction) {
      auto *info = it.kinstruction->info;
      if (!info->file.empty()) {
          descr += ":" + llvm::sys::path::filename(info->file).str() +
                   ":" + std::to_string(info->line) +
                   ":" + std::to_string(info->column);
      }
    }

    std::vector<uint8_t> data;

    // FIXME: store the pointers as pairs too, not in two objects
    if (auto seg = segment->getZExtValue()) {
        auto w = Context::get().getPointerWidth();
        auto size = static_cast<unsigned>(w)/8;
        data.resize(size);
        memcpy(data.data(), &seg, size);
        res.push_back(std::make_pair(descr, data));
        descr += " (offset)";
    }

    auto size = std::max(static_cast<unsigned>(it.value.getValue()->getWidth()/8), 1U);
    assert(size > 0 && "Invalid size");
    assert(size <= 8 && "Does not support size > 8");
    data.clear();
    data.resize(size);
    uint64_t val = value->getZExtValue();
    memcpy(data.data(), &val, size);

    res.push_back(std::make_pair(descr, data));
  }
  return true;
}
  // get a sequence of inputs that drive the program to this state
std::vector<NamedConcreteValue>
Executor::getTestVector(const ExecutionState &state) {
  std::vector<NamedConcreteValue> res;
  res.reserve(state.nondetValues.size());

  for (auto& it : state.nondetValues) {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, it.value.getValue(), value);
    assert(success && "FIXME: Unhandled solver failure");

    ref<ConstantExpr> segment;
    success = solver->getValue(state, it.value.getSegment(), segment);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;

    auto size = it.value.getValue()->getWidth();
    assert(size <= 64 && "Does not support bitwidth > 64");
    // XXX: SExtValue for signed types?
    uint64_t val = value->getZExtValue();
    uint64_t seg = segment->getZExtValue();

    if (seg > 0) {
        auto w = Context::get().getPointerWidth();
        res.emplace_back(APInt(w, seg), APInt(w, val), it.name);
    } else {
        res.emplace_back(size, val, it.isSigned, it.name);
    }
    if (it.kinstruction) {
        const auto& D = it.kinstruction->inst->getDebugLoc();
        if (D) {
            res.back().line = D.getLine();
            res.back().col = D.getCol();
        }
    }
  }
  return res;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly && 
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, KValue(it->second));
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

size_t Executor::getAllocationAlignment(const llvm::Value *allocSite) const {
  // FIXME: 8 was the previous default. We shouldn't hard code this
  // and should fetch the default from elsewhere.
  const size_t forcedAlignment = 8;
  size_t alignment = 0;
  llvm::Type *type = NULL;
  std::string allocationSiteName(allocSite->getName().str());
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(allocSite)) {
    alignment = GV->getAlignment();
    if (const GlobalVariable *globalVar = dyn_cast<GlobalVariable>(GV)) {
      // All GlobalVariables's have pointer type
      llvm::PointerType *ptrType =
          dyn_cast<llvm::PointerType>(globalVar->getType());
      assert(ptrType && "globalVar's type is not a pointer");
      type = ptrType->getElementType();
    } else {
      type = GV->getType();
    }
  } else if (const AllocaInst *AI = dyn_cast<AllocaInst>(allocSite)) {
    alignment = AI->getAlignment();
    type = AI->getAllocatedType();
  } else if (isa<InvokeInst>(allocSite) || isa<CallInst>(allocSite)) {
    // FIXME: Model the semantics of the call to use the right alignment
    llvm::Value *allocSiteNonConst = const_cast<llvm::Value *>(allocSite);
    const CallSite cs = (isa<InvokeInst>(allocSiteNonConst)
                             ? CallSite(cast<InvokeInst>(allocSiteNonConst))
                             : CallSite(cast<CallInst>(allocSiteNonConst)));
    llvm::Function *fn =
        klee::getDirectCallTarget(cs, /*moduleIsFullyLinked=*/true);
    if (fn)
      allocationSiteName = fn->getName().str();

    if (allocationSiteName.compare(0, 17, "__VERIFIER_nondet") == 0) {
        type = cast<CallInst>(cs.getInstruction())->getType();
        alignment = 0;
    } else {
      klee_warning_once(fn != NULL ? fn : allocSite,
                        "Alignment of memory from call \"%s\" is not "
                        "modelled. Using alignment of %zu.",
                        allocationSiteName.c_str(), forcedAlignment);
      alignment = forcedAlignment;
    }
  } else {
    llvm_unreachable("Unhandled allocation site");
  }

  if (alignment == 0) {
    assert(type != NULL);
    // No specified alignment. Get the alignment for the type.
    if (type->isSized()) {
      alignment = kmodule->targetData->getPrefTypeAlignment(type);
    } else {
      klee_warning_once(allocSite, "Cannot determine memory alignment for "
                                   "\"%s\". Using alignment of %zu.",
                        allocationSiteName.c_str(), forcedAlignment);
      alignment = forcedAlignment;
    }
  }

  // Currently we require alignment be a power of 2
  if (!bits64::isPowerOfTwo(alignment)) {
    klee_warning_once(allocSite, "Alignment of %zu requested for %s but this "
                                 "not supported. Using alignment of %zu",
                      alignment, allocSite->getName().str().c_str(),
                      forcedAlignment);
    alignment = forcedAlignment;
  }
  assert(bits64::isPowerOfTwo(alignment) &&
         "Returned alignment must be a power of two");
  return alignment;
}

void Executor::prepareForEarlyExit() {
  if (statsTracker) {
    // Make sure stats get flushed out
    statsTracker->done();
  }
}

/// Returns the errno location in memory
int *Executor::getErrnoLocation(const ExecutionState &state) const {
#if !defined(__APPLE__) && !defined(__FreeBSD__)
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  return __errno_location();
#else
  return __error();
#endif
}


void Executor::dumpPTree() {
  if (!::dumpPTree) return;

  char name[32];
  snprintf(name, sizeof(name),"ptree%08d.dot", (int) stats::instructions);
  auto os = interpreterHandler->openOutputFile(name);
  if (os) {
    processTree->dump(*os);
  }

  ::dumpPTree = 0;
}

void Executor::dumpStates() {
  if (!::dumpStates) return;

  auto os = interpreterHandler->openOutputFile("states.txt");

  if (os) {
    for (ExecutionState *es : states) {
      *os << "(" << es << ",";
      *os << "[";
      auto next = es->stack.begin();
      ++next;
      for (auto sfIt = es->stack.begin(), sf_ie = es->stack.end();
           sfIt != sf_ie; ++sfIt) {
        *os << "('" << sfIt->kf->function->getName().str() << "',";
        if (next == es->stack.end()) {
          *os << es->prevPC->info->line << "), ";
        } else {
          *os << next->caller->info->line << "), ";
          ++next;
        }
      }
      *os << "], ";

      StackFrame &sf = es->stack.back();
      uint64_t md2u = computeMinDistToUncovered(es->pc,
                                                sf.minDistToUncoveredOnReturn);
      uint64_t icnt = theStatisticManager->getIndexedValue(stats::instructions,
                                                           es->pc->info->id);
      uint64_t cpicnt = sf.callPathNode->statistics.getValue(stats::instructions);

      *os << "{";
      *os << "'depth' : " << es->depth << ", ";
      *os << "'weight' : " << es->weight << ", ";
      *os << "'queryCost' : " << es->queryCost << ", ";
      *os << "'coveredNew' : " << es->coveredNew << ", ";
      *os << "'instsSinceCovNew' : " << es->instsSinceCovNew << ", ";
      *os << "'md2u' : " << md2u << ", ";
      *os << "'icnt' : " << icnt << ", ";
      *os << "'CPicnt' : " << cpicnt << ", ";
      *os << "}";
      *os << ")\n";
    }
  }

  ::dumpStates = 0;
}

static std::tuple<std::string, unsigned, unsigned>
parseNondetName(const std::string& name) {
    std::string fun;
    unsigned line = 0, col = 0;//, seq = 0;

    int num = 0;
    int last_semicol = 0;
    for (size_t i = 0; i < name.size(); ++i) {
      if (name[i] != ':')
          continue;

      switch (++num) {
        case 1: // function/obj name
          fun = name.substr(0, i);
          break;
        case 2: break; // file name
        case 3: // line
          line = stoi(name.substr(last_semicol + 1, i));
          break;
          line = stoi(name.substr(last_semicol + 1, i));
          break;
        default:
          klee_warning("Invalid nondet object name: %s", name.c_str());
          return std::make_tuple(fun, line, col);//, seq);
      };

      last_semicol = i;
    }

    if (num != 3) {
        if (num == 0) {
            // we got just name and no information,
            // this is probably a nondet global
            fun = name;
        }
        return std::make_tuple(fun, line, col);
    }
    // parse the column and instance number
    unsigned inst_start = 0, inst_end = 0;
    for (size_t i = last_semicol + 1; i < name.size(); ++i) {
        if (name[i] == '(')
            inst_start = i + 1;
        else if (name[i] == ')')
            inst_end = i;
    }

    if (inst_start > 0) {
        assert(inst_end > 0);
        //seq = stoi(name.substr(inst_start, inst_end));
        col = stoi(name.substr(last_semicol + 1, inst_start));
    } else {
        col = stoi(name.substr(last_semicol + 1));
    }
    return std::make_tuple(fun, line, col);
}

static ConcreteValue getConcreteValue(unsigned bytesNum,
                                      const unsigned char *bytes) {

  // create it as unsigned value
  llvm::APInt val(bytesNum*8, 0, false);
  for (unsigned n = 0; n < bytesNum; ++n) {
      val <<= 8;
      val |= bytes[bytesNum - n - 1];
  }

  return ConcreteValue(std::move(val), false);
}

///
// FIXME: we completely ignore pointers here
void Executor::setReplayNondet(const struct KTest *out) {
  assert(out && "No ktest file given");
  assert(!replayPath && !replayKTest && "cannot replay both nondets and path");

  replayNondet.reserve(out->numObjects);

  for (unsigned i = 0; i < out->numObjects; ++i) {
      std::string name = out->objects[i].name;
      std::string fun;
      unsigned line, col;
      std::tie(fun, line, col) = parseNondetName(name);

      auto val = getConcreteValue(out->objects[i].numBytes,
                                  out->objects[i].bytes);

      if (name.size() > 8 && name.compare(name.size() - 8, 8, "(offset)") == 0 ) {
        // this is an offset of previous nondet pointer,
        // so instead of creating a new record, just update the previous one
        auto& lastNv = replayNondet.back();
        auto& concreteVal = std::get<3>(lastNv);
        concreteVal.setPointer(std::move(concreteVal.getValue()));
        concreteVal.setValue(std::move(val.getValue()));
      } else {
        replayNondet.emplace_back(std::move(fun), line, col, std::move(val));
      }
  }

  for (auto& nv : replayNondet) {
    auto& val = std::get<3>(nv);
    if (val.isPointer()) {
      klee_message("Input vector: %s:%u:%u = (%lu:%lu)",
                    std::get<0>(nv).c_str(), std::get<1>(nv),
                    std::get<2>(nv),
                    val.getPointer().getZExtValue(),
                    val.getValue().getZExtValue());
    } else {
      klee_message("Input vector: %s:%u:%u = %lu",
                    std::get<0>(nv).c_str(), std::get<1>(nv),
                    std::get<2>(nv), val.getValue().getZExtValue());
    }
  }
}

///

Interpreter *Interpreter::create(LLVMContext &ctx, const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(ctx, opts, ih);
}
