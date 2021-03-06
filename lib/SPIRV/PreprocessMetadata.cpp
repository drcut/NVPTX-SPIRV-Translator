//===- PreprocessMetadata.cpp -                                   - C++ -*-===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
// This file implements preprocessing of LLVM IR metadata in order to perform
// further translation to SPIR-V.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "clmdtospv"

#include "OCLUtil.h"
#include "SPIRVInternal.h"
#include "SPIRVMDBuilder.h"
#include "SPIRVMDWalker.h"
#include "VectorComputeUtil.h"

#include "llvm/ADT/Triple.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace SPIRV;
using namespace OCLUtil;

namespace SPIRV {

cl::opt<bool> EraseOCLMD("spirv-erase-cl-md", cl::init(true),
                         cl::desc("Erase OpenCL metadata"));

class PreprocessMetadata : public ModulePass {
public:
  PreprocessMetadata() : ModulePass(ID), M(nullptr), Ctx(nullptr) {
    initializePreprocessMetadataPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void visit(Module *M);
  void preprocessNVPTXMetadata(Module *M, SPIRVMDBuilder *B, SPIRVMDWalker *W);
  void preprocessVectorComputeMetadata(Module *M, SPIRVMDBuilder *B,
                                       SPIRVMDWalker *W);

  static char ID;

private:
  Module *M;
  LLVMContext *Ctx;
};

char PreprocessMetadata::ID = 0;

bool PreprocessMetadata::runOnModule(Module &Module) {
  M = &Module;
  Ctx = &M->getContext();

  LLVM_DEBUG(dbgs() << "Enter PreprocessMetadata:\n");
  visit(M);

  LLVM_DEBUG(dbgs() << "After PreprocessMetadata:\n" << *M);
  std::string Err;
  raw_string_ostream ErrorOS(Err);
  if (verifyModule(*M, &ErrorOS)) {
    LLVM_DEBUG(errs() << "Fails to verify module: " << ErrorOS.str());
  }
  return true;
}

void PreprocessMetadata::visit(Module *M) {
  SPIRVMDBuilder B(*M);
  SPIRVMDWalker W(*M);

  preprocessNVPTXMetadata(M, &B, &W);
  preprocessVectorComputeMetadata(M, &B, &W);

  // Add MetaData for SPIR
  // TODO: modify the magic number
  B.addNamedMD("opencl.spir.version")
      .addOp()
      .add(unsigned(1))
      .add(unsigned(2))
      .done();
  B.addNamedMD("opencl.ocl.version")
      .addOp()
      .add(unsigned(1))
      .add(unsigned(0))
      .done();
  // Create metadata representing (empty so far) list
  // of OpExecutionMode instructions
  auto EM = B.addNamedMD(kSPIRVMD::ExecutionMode); // !spirv.ExecutionMode = {}

  // Add execution modes for kernels. We take it from metadata attached to
  // the kernel functions.
  for (Function &Kernel : *M) {
    if (Kernel.getCallingConv() != CallingConv::SPIR_KERNEL)
      continue;

    // Specifing execution modes for the Kernel and adding it to the list
    // of ExecutionMode instructions.

    // !{void (i32 addrspace(1)*)* @kernel, i32 17, i32 X, i32 Y, i32 Z}
    if (MDNode *WGSize = Kernel.getMetadata(kSPIR2MD::WGSize)) {
      unsigned X, Y, Z;
      decodeMDNode(WGSize, X, Y, Z);
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeLocalSize)
          .add(X)
          .add(Y)
          .add(Z)
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 18, i32 X, i32 Y, i32 Z}
    if (MDNode *WGSizeHint = Kernel.getMetadata(kSPIR2MD::WGSizeHint)) {
      unsigned X, Y, Z;
      decodeMDNode(WGSizeHint, X, Y, Z);
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeLocalSizeHint)
          .add(X)
          .add(Y)
          .add(Z)
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 30, i32 hint}
    if (MDNode *VecTypeHint = Kernel.getMetadata(kSPIR2MD::VecTyHint)) {
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeVecTypeHint)
          .add(transVecTypeHint(VecTypeHint))
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 35, i32 size}
    if (MDNode *ReqdSubgroupSize = Kernel.getMetadata(kSPIR2MD::SubgroupSize)) {
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeSubgroupSize)
          .add(getMDOperandAsInt(ReqdSubgroupSize, 0))
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 max_work_group_size, i32 X,
    //         i32 Y, i32 Z}
    if (MDNode *MaxWorkgroupSizeINTEL =
            Kernel.getMetadata(kSPIR2MD::MaxWGSize)) {
      unsigned X, Y, Z;
      decodeMDNode(MaxWorkgroupSizeINTEL, X, Y, Z);
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeMaxWorkgroupSizeINTEL)
          .add(X)
          .add(Y)
          .add(Z)
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 max_global_work_dim, i32 dim}
    if (MDNode *MaxWorkDimINTEL = Kernel.getMetadata(kSPIR2MD::MaxWGDim)) {
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeMaxWorkDimINTEL)
          .add(getMDOperandAsInt(MaxWorkDimINTEL, 0))
          .done();
    }

    // !{void (i32 addrspace(1)*)* @kernel, i32 num_simd_work_items, i32 num}
    if (MDNode *NumSIMDWorkitemsINTEL = Kernel.getMetadata(kSPIR2MD::NumSIMD)) {
      EM.addOp()
          .add(&Kernel)
          .add(spv::ExecutionModeNumSIMDWorkitemsINTEL)
          .add(getMDOperandAsInt(NumSIMDWorkitemsINTEL, 0))
          .done();
    }
  }
}


void PreprocessMetadata::preprocessNVPTXMetadata(Module *M, SPIRVMDBuilder *B,
                                                 SPIRVMDWalker *W) {
  // Preprocess NVPTX-specific metadata
  // !nvvmir.version = !{!x}
  // !{x} = !{i32 1, i32 4}
  B->eraseNamedMD("nvvmir.version");

  // !spirv.MemoryModel = !{!x}
  // !{x} = !{i32 1, i32
  Triple TT(M->getTargetTriple());
  assert(isSupportedTriple(TT) && "Invalid triple");
  B->addNamedMD(kSPIRVMD::MemoryModel)
      .addOp()
      .add(TT.isArch32Bit() ? spv::AddressingModelPhysical32
                            : spv::AddressingModelPhysical64)
      .add(spv::MemoryModelOpenCL)
      .done();

  // Add source extensions
  // !spirv.SourceExtension = !{!x, !y, ...}
  // !x = {!"cl_khr_..."}
  // !y = {!"cl_khr_..."}
  auto Exts = getNamedMDAsStringSet(M, kSPIR2MD::Extensions);

  if (!Exts.empty()) {
    auto N = B->addNamedMD(kSPIRVMD::SourceExtension);
    for (auto &I : Exts)
      N.addOp().add(I).done();
  }
  if (EraseOCLMD)
    B->eraseNamedMD(kSPIR2MD::Extensions).eraseNamedMD(kSPIR2MD::OptFeatures);

  if (EraseOCLMD)
    B->eraseNamedMD(kSPIR2MD::FPContract);
  // add kernel_arg_access_qual for kernels
  // get kernels
  NamedMDNode *NamedMD = M->getNamedMetadata("nvvm.annotations");
  std::set<Function *> kernels;
  if (!NamedMD) {
    printf("there must be nvvm.annotations!\n");
    exit(1);
  }
  // !nvvm.annotations = !{!3, !4, !5, !4, !6, !6, !6, !6, !7, !7, !6}
  // !3 = !{void (i32*, i32*, i32*)* @_Z6vecaddPiS_S_, !"kernel", i32 1}
  for (unsigned I = 0, E = NamedMD->getNumOperands(); I != E; ++I) {
    MDNode *MD = NamedMD->getOperand(I);
    if (!MD || MD->getNumOperands() == 0)
      continue;
    if (MD->getNumOperands() != 3)
      continue;
    Metadata *Op = MD->getOperand(1);
    if (auto Str = dyn_cast<MDString>(Op)) {
      if (Str->getString().str() != "kernel")
        continue;
      // S = Str->getString().str();
      Function *F = mdconst::dyn_extract<Function>(MD->getOperand(0));
      std::cout << F->getName().str() << std::endl;
      kernels.insert(F);

      // construct kernel_arg_access_qual
      std::vector<Metadata *> kernel_arg_access_qual_Vec;
      auto A = F->arg_begin(), E = F->arg_end();
      for (unsigned I = 0; A != E; ++I, ++A) {
        kernel_arg_access_qual_Vec.push_back(MDString::get(*Ctx, "none"));
      }
      F->setMetadata("kernel_arg_access_qual",
                     MDNode::get(*Ctx, kernel_arg_access_qual_Vec));
      kernel_arg_access_qual_Vec.clear();

      // construct kernel_arg_type
      std::vector<Metadata *> kernel_arg_type_Vec;
      A = F->arg_begin();
      E = F->arg_end();
      for (unsigned I = 0; A != E; ++I, ++A) {
        if (A->getType()->isPointerTy()) {
          if (A->getType()->isFloatingPointTy())
            kernel_arg_type_Vec.push_back(MDString::get(*Ctx, "float*"));
          else
            kernel_arg_type_Vec.push_back(MDString::get(*Ctx, "int*"));
        } else {
          if (A->getType()->isFloatTy())
            kernel_arg_type_Vec.push_back(MDString::get(*Ctx, "float"));
          if (A->getType()->isIntegerTy())
            kernel_arg_type_Vec.push_back(MDString::get(*Ctx, "int"));
        }
      }
      F->setMetadata("kernel_arg_type", MDNode::get(*Ctx, kernel_arg_type_Vec));
      kernel_arg_type_Vec.clear();

      // construct kernel_arg_addr_space
      std::vector<Metadata *> kernel_arg_addr_space_Vec;
      A = F->arg_begin();
      E = F->arg_end();
      for (unsigned I = 0; A != E; ++I, ++A) {
        if (A->getType()->isPointerTy())
          kernel_arg_addr_space_Vec.push_back(ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt32Ty(*Ctx), 1)));
        else
          kernel_arg_addr_space_Vec.push_back(ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt32Ty(*Ctx), 0)));
      }
      F->setMetadata("kernel_arg_addr_space",
                     MDNode::get(*Ctx, kernel_arg_addr_space_Vec));
      kernel_arg_addr_space_Vec.clear();

      // makr this Function as KERNEL
      F->setCallingConv(CallingConv::SPIR_KERNEL);
    }
  }
}

void PreprocessMetadata::preprocessVectorComputeMetadata(Module *M,
                                                         SPIRVMDBuilder *B,
                                                         SPIRVMDWalker *W) {
  using namespace VectorComputeUtil;

  auto EM = B->addNamedMD(kSPIRVMD::ExecutionMode);

  for (auto &F : *M) {
    if (F.getCallingConv() != CallingConv::SPIR_KERNEL)
      continue;

    // Add VC float control execution modes
    // RoundMode and FloatMode are always same for all types in VC
    // While Denorm could be different for double, float and half
    auto Attrs = F.getAttributes();
    if (Attrs.hasFnAttribute(kVCMetadata::VCFloatControl)) {
      SPIRVWord Mode = 0;
      Attrs
          .getAttribute(AttributeList::FunctionIndex,
                        kVCMetadata::VCFloatControl)
          .getValueAsString()
          .getAsInteger(0, Mode);
      spv::ExecutionMode ExecRoundMode =
          FPRoundingModeExecModeMap::map(getFPRoundingMode(Mode));
      spv::ExecutionMode ExecFloatMode =
          FPOperationModeExecModeMap::map(getFPOperationMode(Mode));
      VCFloatTypeSizeMap::foreach (
          [&](VCFloatType FloatType, unsigned TargetWidth) {
            EM.addOp().add(&F).add(ExecRoundMode).add(TargetWidth).done();
            EM.addOp().add(&F).add(ExecFloatMode).add(TargetWidth).done();
            EM.addOp()
                .add(&F)
                .add(FPDenormModeExecModeMap::map(
                    getFPDenormMode(Mode, FloatType)))
                .add(TargetWidth)
                .done();
          });
    }
    if (Attrs.hasFnAttribute(kVCMetadata::VCSLMSize)) {
      SPIRVWord SLMSize = 0;
      Attrs.getAttribute(AttributeList::FunctionIndex, kVCMetadata::VCSLMSize)
          .getValueAsString()
          .getAsInteger(0, SLMSize);
      EM.addOp()
          .add(&F)
          .add(spv::ExecutionModeSharedLocalMemorySizeINTEL)
          .add(SLMSize)
          .done();
    }
  }
}

} // namespace SPIRV

INITIALIZE_PASS(PreprocessMetadata, "preprocess-metadata",
                "Transform LLVM IR metadata to SPIR-V metadata format", false,
                false)

ModulePass *llvm::createPreprocessMetadata() {
  return new PreprocessMetadata();
}
