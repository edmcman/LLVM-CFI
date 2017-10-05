//===-- SafeDispatchReturnRange.cpp - SafeDispatch ReturnRange code ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SDReturnRange class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/SafeDispatchReturnRange.h"

#include "llvm/IR/DebugInfo.h"
#include "llvm/Transforms/IPO/SafeDispatchTools.h"

#include <cxxabi.h>
#include <fstream>
#include <sstream>

using namespace llvm;

char SDReturnRange::ID = 0;

INITIALIZE_PASS(SDReturnRange,
"sdRetRange", "Build return ranges", false, false)

ModulePass *llvm::createSDReturnRangePass() {
  return new SDReturnRange();
}

//TODO MATT: format properly / code duplication
static StringRef sd_getClassNameFromMD(llvm::MDNode *MDNode, unsigned operandNo = 0) {
  llvm::MDTuple *mdTuple = cast<llvm::MDTuple>(MDNode);
  assert(mdTuple->getNumOperands() > operandNo + 1);

  llvm::MDNode *nameMdNode = cast<llvm::MDNode>(mdTuple->getOperand(operandNo).get());
  llvm::MDString *mdStr = cast<llvm::MDString>(nameMdNode->getOperand(0));

  StringRef strRef = mdStr->getString();
  assert(sd_isVtableName_ref(strRef));
  return strRef;
}

static StringRef sd_getFunctionNameFromMD(llvm::MDNode *MDNode, unsigned operandNo = 0) {
  assert(MDNode->getNumOperands() > operandNo);

  llvm::MDString *mdStr = cast<llvm::MDString>(MDNode->getOperand(operandNo));

  StringRef strRef = mdStr->getString();
  return strRef;
}

bool SDReturnRange::runOnModule(Module &M) {
  // Get the results from the class hierarchy analysis pass.
  CHA = &getAnalysis<SDBuildCHA>();

  sdLog::blankLine();
  sdLog::stream() << "P7a. Started running the SDReturnRange pass ..." << sdLog::newLine << "\n";

  CHA->buildFunctionInfo();

  locateCallSites(M);
  locateStaticCallSites(M);

  // Store the data generated by this pass.
  storeCallSites(M);

  sdLog::stream() << sdLog::newLine << "P7a. Finished running the SDReturnRange pass ..." << "\n";
  sdLog::blankLine();
  return false;
}

void SDReturnRange::locateCallSites(Module &M) {
  Function *IntrinsicFunction = M.getFunction(Intrinsic::getName(Intrinsic::sd_get_checked_vptr));

  if (IntrinsicFunction == nullptr) {
    sdLog::warn() << "Intrinsic not found.\n";
    return;
  }

  int count = 0;
  for (const Use &U : IntrinsicFunction->uses()) {

    // get the intrinsic call instruction
    CallInst *IntrinsicCall = dyn_cast<CallInst>(U.getUser());
    assert(IntrinsicCall && "Intrinsic was not wrapped in a CallInst?");

    // Find the CallSite that is associated with the intrinsic call.
    User *User = *(IntrinsicCall->users().begin());
    for (int i = 0; i < 3; ++i) {
      // User was not found, this should not happen...
      if (User == nullptr)
        break;

      for (auto *NextUser : User->users()) {
        User = NextUser;
        break;
      }
    }

    CallSite CallSite(User);
    if (CallSite.getInstruction()) {
      // valid CallSite
      addCallSite(IntrinsicCall, CallSite, M);
    } else {
      sdLog::warn() << "CallSite for intrinsic was not found.\n";
      IntrinsicCall->getParent()->dump();
    }
    ++count;
    sdLog::log() << "\n";
  }
  sdLog::stream() << count << " virtual function Callsites\n";
}

static bool isRelevantStaticFunction(const Function &F) {
  return !(F.getName().startswith("llvm.")
           || F.getName() == "_Znwm"
  );
}

void SDReturnRange::locateStaticCallSites(Module &M) {
  int totalDirect = 0;
  int totalIndirect = 0;

  int countDirect = 0;
  int countIndirect = 0;

  for (auto &F : M) {
    for(auto &MBB : F) {
      for (auto &I : MBB) {
        CallSite Call(&I);
        if (Call.getInstruction()) {
          if (Function *Callee = Call.getCalledFunction()) {
            if (isRelevantStaticFunction(*Callee)){
              addStaticCallSite(Call, M);
              ++countDirect;
            }
          } else if (CallSite(Call).isIndirectCall() && VirtualCallsites.find(Call) == VirtualCallsites.end()) {
            addStaticCallSite(Call, M);
            ++countIndirect;
          }
        }
      }
    }
    sdLog::stream() << F.getName() << "(direct: " << countDirect << ", indirect:"<< countIndirect << ")...\n";
    totalDirect += countDirect;
    totalIndirect += countIndirect;
    countDirect = countIndirect = 0;
  }
  sdLog::stream() << totalDirect << " direct static Callsites\n";
  sdLog::stream() << totalIndirect << " indirect static Callsites\n";
}

void SDReturnRange::addStaticCallSite(CallSite CallSite, Module &M) {
  const DebugLoc &Loc = CallSite.getInstruction()->getDebugLoc();
  if (!Loc) {
    // Minor hack: We generate our own DebugLoc using a dummy MDSubprogram.
    // pseudoDebugLoc is the unique ID for this CallSite.
    llvm::LLVMContext &C = M.getContext();
    auto DummyProgram = MDSubprogram::getDistinct(C, nullptr, "", "", nullptr, 0,
                                                  nullptr, false, false, 0, nullptr, 0, 0, 0,
                                                  0);
    MDLocation *Location = MDLocation::getDistinct(C, pseudoDebugLoc / 65536, pseudoDebugLoc % 65536, DummyProgram);
    ++pseudoDebugLoc;
    DebugLoc newLoc(Location);
    CallSite->setDebugLoc(Location);
  }

  // write DebugLoc to map (is written to file in storeCallSites)
  auto *Scope = cast<MDScope>(Loc.getScope());
  std::string FunctionName = "__UNDEFINED__";
  if (CallSite.getCalledFunction()) {
    FunctionName = CallSite.getCalledFunction()->getName().str();
    CalledFunctions.insert(FunctionName);
  } else if (CallSite.isTailCall()) {
    FunctionName = "__TAIL__";
  }

  std::stringstream Stream;
  Stream << Scope->getFilename().str() << ":" << Loc.getLine() << ":" << Loc.getCol()
         << "," << FunctionName;

  CallSiteDebugLocsStatic.push_back(Stream.str());

  sdLog::log() << "CallSite " << CallSite->getParent()->getParent()->getName()
                  << " @" << Scope->getFilename().str() << ":" << Loc.getLine() << ":" << Loc.getCol()
                  << " for callee " << FunctionName << "\n";
}

void SDReturnRange::addCallSite(const CallInst *CheckedVptrCall, CallSite CallSite, Module &M) {
  // get ClassName metadata
  MetadataAsValue *Arg2 = dyn_cast<MetadataAsValue>(CheckedVptrCall->getArgOperand(1));
  assert(Arg2);
  MDNode *ClassNameNode = dyn_cast<MDNode>(Arg2->getMetadata());
  assert(ClassNameNode);

  // get PreciseName metadata
  MetadataAsValue *Arg3 = dyn_cast<MetadataAsValue>(CheckedVptrCall->getArgOperand(2));
  assert(Arg3);
  MDNode *PreciseNameNode = dyn_cast<MDNode>(Arg2->getMetadata());
  assert(PreciseNameNode);

  // get PreciseName metadata
  MetadataAsValue *Arg4 = dyn_cast<MetadataAsValue>(CheckedVptrCall->getArgOperand(3));
  assert(Arg4);
  MDNode *FunctionNameNode = dyn_cast<MDNode>(Arg4->getMetadata());
  assert(FunctionNameNode);

  // Arg2 is the tuple that contains the class name and the corresponding global var.
  // note that the global variable isn't always emitted

  const StringRef ClassName = sd_getClassNameFromMD(ClassNameNode);
  const StringRef PreciseName = sd_getClassNameFromMD(PreciseNameNode);
  const StringRef FunctionName = sd_getFunctionNameFromMD(FunctionNameNode);

  const DebugLoc &Loc = CallSite.getInstruction()->getDebugLoc();
  if (!Loc) {
    // Minor hack: We generate our own DebugLoc using a dummy MDSubprogram.
    // pseudoDebugLoc is the unique ID for this CallSite.
    llvm::LLVMContext &C = M.getContext();
    auto DummyProgram = MDSubprogram::getDistinct(C, nullptr, "", "", nullptr, 0,
                                                  nullptr, false, false, 0, nullptr, 0, 0, 0,
                                                  0);
    MDLocation *Location = MDLocation::getDistinct(C, pseudoDebugLoc / 65536, pseudoDebugLoc % 65536, DummyProgram);
    ++pseudoDebugLoc;
    DebugLoc newLoc(Location);
    CallSite.getInstruction()->setDebugLoc(Location);
  }

  std::vector<SDBuildCHA::range_t> ranges = CHA->getFunctionRange(FunctionName, ClassName);

  if (ranges.empty()) {
    sdLog::errs() << "Call for " << FunctionName << " (" << ClassName << "," << PreciseName << ") has no range!?\n";
    return;
  }

  sdLog::log() << ranges[0].first << "-" << ranges[0].second << "\n";

  // write DebugLoc to map (is written to file in storeCallSites)
  auto *Scope = cast<MDScope>(Loc.getScope());
  std::stringstream Stream;
  Stream << Scope->getFilename().str() << ":" << Loc.getLine() << ":" << Loc.getCol()
         << "," << ClassName.str() << "," << PreciseName.str() << "," << FunctionName.str()
         << "," << ranges[0].first << "," << ranges[0].second;

  CallSiteDebugLocs.push_back(Stream.str());
  VirtualCallsites.insert(CallSite);

  sdLog::log() << "CallSite @" << Scope->getFilename().str() << ":" << Loc.getLine() << ":" << Loc.getCol()
                  << " for class " << ClassName << "(" << PreciseName << ")::" << FunctionName << "\n";
}

void SDReturnRange::storeCallSites(Module &M) {
  sdLog::stream() << "Store all callsites for module: " << M.getName() << "\n";
  std::ofstream Outfile("./_SD_CallSites.txt");
  std::ostream_iterator <std::string> OutIterator(Outfile, "\n");
  std::copy(CallSiteDebugLocs.begin(), CallSiteDebugLocs.end(), OutIterator);
  Outfile.close();

  int number = 0;
  auto outName = "./_SD_CallSites" + std::to_string(number);
  std::ifstream infile(outName);
  while(infile.good()) {
    number++;
    outName = "./_SD_CallSites" + std::to_string(number);
    infile = std::ifstream(outName);
  }

  std::ifstream  src("./_SD_CallSites.txt", std::ios::binary);
  std::ofstream  dst(outName, std::ios::binary);
  dst << src.rdbuf();

  std::ofstream Outfile2("./_SD_CallSitesStatic.txt");
  std::ostream_iterator <std::string> OutIterator2(Outfile2, "\n");
  std::copy(CallSiteDebugLocsStatic.begin(), CallSiteDebugLocsStatic.end(), OutIterator2);
  Outfile2.close();

  outName = "./_SD_CallSitesStatic" + std::to_string(number);
  std::ifstream  src2("./_SD_CallSitesStatic.txt", std::ios::binary);
  std::ofstream  dst2(outName, std::ios::binary);
  dst2 << src2.rdbuf();
}