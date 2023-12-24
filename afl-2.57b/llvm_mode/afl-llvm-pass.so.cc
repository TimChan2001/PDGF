/*
  Copyright 2015 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.
*/

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include <cassert>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Instructions.h"
#include <queue>
#include <set>

using namespace llvm;

static std::string TargetsFile = "/root/pdgf-files/BBtargets.txt";
static std::string OutFile = "/root/pdgf-files/edge2bb.txt";
static std::string OutFile2 = "/root/pdgf-files/pbb.txt";

struct BlockInfo
{
  BlockAddress *BlockAddr;
  unsigned int BlockId;
  SmallVector<unsigned int, 16> BranchID;
};

namespace 
{

  class AFLCoverage : public ModulePass 
  {

    public:
      SmallVector<BlockInfo, 16> AFLBlockInfoVec;
      SmallVector<unsigned int, 16> PBBid;
      SmallVector<unsigned int, 16> TargetBBid;
      std::map<unsigned int, std::vector<std::string>> Id2target; 
      BlockInfo *getBlockInfo(BasicBlock *BB);

      static char ID;
      AFLCoverage() : ModulePass(ID) {}

      bool runOnModule(Module &M) override;

      void getAnalysisUsage(AnalysisUsage &AU) const override
      {
        AU.addRequired<DominatorTreeWrapperPass>();
        // AU.addRequiredTransitive<CallGraphWrapperPass>();
      }

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }
  };

} // namespace

char AFLCoverage::ID = 0;

static void getDebugLoc(const Instruction *I, std::string &Filename,
                        unsigned &Line)
{
  if (DILocation *Loc = I->getDebugLoc())
  {
    Line = Loc->getLine();
    Filename = Loc->getFilename().str();

    if (Filename.empty())
    {
      DILocation *oDILoc = Loc->getInlinedAt();
      if (oDILoc)
      {
        Line = oDILoc->getLine();
        Filename = oDILoc->getFilename().str();
      }
    }
  }
}

std::set<BasicBlock*> visited; // 已访问的基本块集合

void BFS(BasicBlock *Target) {
    std::queue<BasicBlock*> toVisit; // 待访问的基本块队列
    toVisit.push(Target);

    while (!toVisit.empty()) {
        BasicBlock *current = toVisit.front();
        toVisit.pop();

        // 如果这个基本块已经访问过，就跳过
        if (visited.find(current) != visited.end()) continue;

        // 处理当前基本块
        errs() << "访问基本块: " << current->getName() << "\n";

        // 标记为已访问
        visited.insert(current);

        // 将所有前驱基本块加入队列
        for (auto PI = pred_begin(current), E = pred_end(current); PI != E; ++PI) {
            BasicBlock *Pred = *PI;
            if (visited.find(Pred) == visited.end()) {
                toVisit.push(Pred);
            }
        }
    }
}

bool AFLCoverage::runOnModule(Module &M) 
{

  // getAnalysis<CallGraphWrapperPass>().print(errs(), &M);

  /* Set output file */
  // std::ofstream fdom(OutFile);
  std::vector<BasicBlock *> BlockList;
  std::ofstream fdom;
  fdom.open(OutFile, std::ios::app);
  std::ofstream fdom2;
  fdom2.open(OutFile2, std::ios::app);
  // fdom << "[#] This file is generated by afl-llvm-pass to acquire dominators
  // of given targets.\n"; fdom << "[#] Author: zhuwy19@mails.tsinghua.edu.cn\n";

  /* ************************************************
   * Load intra-procedural targets
   * ************************************************/
  // bool targets_flag = false;
  std::list<std::string> targets;
  if (!TargetsFile.empty())
  {

    std::ifstream targetsfile(TargetsFile);
    std::string line;
    while (std::getline(targetsfile, line))
    {
      // a valid line must have a ":"
      std::size_t found = line.find_last_of(":");
      if (found != std::string::npos)
      {
        targets.push_back(line);
        // errs() << "target: " << line << "\n";
      }
    }
    targetsfile.close();

    // targets_flag = true;
  }
  else
  {
    errs() << "BB TargetsFile empty!\n";
  }

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty = IntegerType::getInt8Ty(C);
  IntegerType *Int32Ty = IntegerType::getInt32Ty(C);
  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) 
  {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } 
  else 
    be_quiet = 1;

  /* Decide instrumentation ratio */

  char *inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");
  ConstantInt *One = ConstantInt::get(Int8Ty, 1);

  // GlobalVariable *AFLPrevLoc = new GlobalVariable(
  //     M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
  //     0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M)
  {
    if (!F.isDeclaration() && !F.empty())
    {
      std::vector<BasicBlock *> InsBlocks;
      for (auto &BB : F) 
      {

        if (F.size() == 1)
        {
          InsBlocks.push_back(&BB);
        }
        else
        {
          uint32_t succ = 0;
          for (succ_iterator SI = succ_begin(&BB), SE = succ_end(&BB); SI != SE;
               ++SI)
            if ((*SI)->size() > 0)
              succ++;
          if (succ < 1) // no need to instrument
            continue;
          InsBlocks.push_back(&BB);
        }
        unsigned int cur_loc = AFL_R(MAP_SIZE);
        BasicBlock *curr_bb = &BB;

        BlockInfo CurBlockInfo;
        CurBlockInfo.BlockAddr = BlockAddress::get(&BB);
        CurBlockInfo.BlockId = cur_loc;
        AFLBlockInfoVec.push_back(CurBlockInfo);

        std::string curr_filename = "";
        unsigned curr_line = 0;
        static const std::string Xlibs("/usr/");

        // /* Debug */
        // for (auto &I : BB) {
        //   getDebugLoc(&I, curr_filename, curr_line);
        //   /* Remove path prefix such as "./" */
        //   std::size_t found = curr_filename.find_last_of("/\\");
        //   if (found != std::string::npos)
        //     curr_filename = curr_filename.substr(found + 1);
        //   errs()<<"[debug] Current instruction:"<<curr_filename<<":"<<curr_line<<"\n";
        // }
        // curr_filename = "";
        // curr_line = 0;

        /* Find the first valid instruction and name the bb with the
         * corresponding filename and line number*/
        for (auto &I : BB)
        {
          getDebugLoc(&I, curr_filename, curr_line);

          /* Remove path prefix such as "./" */
          std::size_t found = curr_filename.find_last_of("/\\");
          if (found != std::string::npos)
            curr_filename = curr_filename.substr(found + 1);

          // errs()<<"[debug] Current instruction:"<<curr_filename<<":"<<curr_line<<" | Random number: "<<cur_loc<< "| Level: "<<curr_level<<"\n";

          if (curr_filename.empty() || curr_line == 0 || !curr_filename.compare(0, Xlibs.size(), Xlibs))
            continue;
          for (auto &target : targets)
          {
            std::size_t found = target.find_last_of("/\\");
            if (found != std::string::npos)
              target = target.substr(found + 1);
            std::size_t pos = target.find_last_of(":");
            std::string target_file = target.substr(0, pos);
            unsigned int target_line = atoi(target.substr(pos + 1).c_str());

            // errs()<<"[debug] Current target: "<<target_file<<":"<<target_line<<"\n";

            if (target_file == curr_filename && target_line == curr_line)
            {
              // is_target = true;
              errs() << cGRN "[*] Found target bb: " << curr_filename << ":"
                      << curr_line << " | bb id: " << cur_loc
                      << "\n" cRST;
              TargetBBid.push_back(cur_loc);
              Id2target[cur_loc].push_back(curr_filename + ":" + std::to_string(curr_line));
              BFS(&BB);
            }
          }
        }
      }
      
      if (InsBlocks.size() > 0)
      {

        uint32_t i = InsBlocks.size();

        do
        {

          --i;
          BasicBlock *newBB = NULL;
          BlockInfo *ptrSuccessorBBInfo = NULL;
          BasicBlock *origBB = &(*InsBlocks[i]);
          std::vector<BasicBlock *> Successors;
          Instruction *TI = origBB->getTerminator();
          uint32_t fs = origBB->getParent()->size();
          uint32_t countto;
          BlockInfo *ptrCurrentBBInfo = getBlockInfo(origBB);
          unsigned int CurrentBBid = ptrCurrentBBInfo->BlockId;

          uint32_t num_succ = 0;

          for (succ_iterator SI = succ_begin(origBB), SE = succ_end(origBB);
               SI != SE; ++SI)
          {
            if ((*SI)->size() > 0)
              num_succ++;

            BasicBlock *succ = *SI;
            Successors.push_back(succ);
          }
          if (num_succ == 1)
          {
            continue;
          }

          if (fs == 1)
          {

            newBB = origBB;
            countto = 1;
          }
          else
          {

            if (TI == NULL || TI->getNumSuccessors() < 1)
              continue;
            if (TI->getNumSuccessors() == 1)
              continue;
            countto = Successors.size();
          }

          // if (Successors.size() != TI->getNumSuccessors())
          //  FATAL("Different successor numbers %lu <-> %u\n", Successors.size(),
          //        TI->getNumSuccessors());

          for (uint32_t j = 0; j < countto; j++)
          {

            if (fs != 1){
              newBB = llvm::SplitEdge(origBB, Successors[j]);
              ptrSuccessorBBInfo = getBlockInfo(Successors[j]);
            }

            if (!newBB)
            {

              if (!be_quiet)
                WARNF("Split failed!");
              continue;
            }
            unsigned int SuccessorBBId = 0;
            if(ptrSuccessorBBInfo){
              SuccessorBBId = ptrSuccessorBBInfo->BlockId;
            }
            BasicBlock::iterator IP = newBB->getFirstInsertionPt();
            IRBuilder<> IRB(&(*IP));

            /* Set the ID of the inserted basic block */

            unsigned int cur_edge = AFL_R(MAP_SIZE);
            ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_edge);
            
            fdom << cur_edge
                << ":" << CurrentBBid
                << "," << SuccessorBBId
                << "\n";


            /* Load SHM pointer */

            Value *MapPtrIdx;

            LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
            MapPtr->setMetadata(M.getMDKindID("nosanitize"),
                                MDNode::get(C, None));
            MapPtrIdx = IRB.CreateGEP(MapPtr, CurLoc);

            /* Update bitmap */

            LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
            Counter->setMetadata(M.getMDKindID("nosanitize"),
                                 MDNode::get(C, None));

            Value *Incr = IRB.CreateAdd(Counter, One);

            IRB.CreateStore(Incr, MapPtrIdx)
                ->setMetadata(M.getMDKindID("nosanitize"),
                              MDNode::get(C, None));

            // done :)

            inst_blocks++;
          }

        } while (i > 0);
      }
    }
    else
    {
      errs() << cRED "[*] Empty function!" << F.getName() << "\n" cRST;
    }
  }

  IRBuilder<> Builder(C);

  // 创建printf和exit函数的声明
  FunctionCallee printfFunc = M.getOrInsertFunction("printf", llvm::FunctionType::get(llvm::IntegerType::getInt32Ty(C), llvm::PointerType::get(llvm::Type::getInt8Ty(C), 0), true));
  FunctionCallee exitFunc = M.getOrInsertFunction("exit", llvm::FunctionType::get(llvm::Type::getVoidTy(C), llvm::IntegerType::getInt32Ty(C), false));

  Constant *str = ConstantDataArray::getString(C, "1\n", true);
  GlobalVariable *printStrVar = new GlobalVariable(M, str->getType(), true, GlobalValue::PrivateLinkage, str, ".str");
  Value *printStr = Builder.CreatePointerCast(printStrVar, PointerType::getUnqual(Type::getInt8Ty(C)));

  // 创建要打印的字符串
  // Value *printStr = Builder.CreateGlobalStringPtr("1\n");
  for (auto &BB : visited){
    BlockInfo *pbb = NULL;
    pbb = getBlockInfo(BB);
    if(pbb) fdom2 << pbb->BlockId << "\n";
    BasicBlock::iterator IP2 = BB->getFirstInsertionPt();
    IRBuilder<> Builder(&(*IP2));
    // Builder.SetInsertPoint(BB);

    // 插入printf调用
    Builder.CreateCall(printfFunc, printStr);

    // 插入exit调用
    Builder.CreateCall(exitFunc, ConstantInt::get(Type::getInt32Ty(C), 0));
  } 

  /* Say something nice. */

  if (!be_quiet) 
  {

    if (!inst_blocks) 
      WARNF("No instrumentation targets found.");
    else 
      OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);

  }

  fdom.close();
  fdom2.close();

  return true;
}

//改函数传入BB变量，获取AFLBlockInfoVec中的BB块信息。(根据BB块地址获取对应的BB块信息)
// iterate the vec to get the corresponding random BBlockId by BB class
// rvalue is BBlockId
BlockInfo *AFLCoverage::getBlockInfo(BasicBlock *BB)
{
  // get the addr of this preBlock in order to get the BlockId of this addr
  BlockAddress *BlcAddr = BlockAddress::get(BB);
  // iterate the vec to get the corresponding BlockId
  for (auto &I : AFLBlockInfoVec) {
    if (I.BlockAddr == BlcAddr) {
      return &I;
    }
  }
  errs() << "There is no corresponding BB in the array\n";
  return nullptr;
}

static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) 
{

  PM.add(new AFLCoverage());
}

static RegisterStandardPasses 
    RegisterAFLPass(PassManagerBuilder::EP_OptimizerLast, 
                    registerAFLPass);

static RegisterStandardPasses 
    RegisterAFLPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                     registerAFLPass);
