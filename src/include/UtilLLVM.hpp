#ifndef UNIAS_UTILLLVM_H
#define UNIAS_UTILLLVM_H
#include "SVF-LLVM/BasicTypes.h"
#include "SVFIR/SVFType.h"
#include "SVFIR/SVFValue.h"
#include <llvm/IR/Instruction.h>

#include <string>
#include <fstream>
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMUtil.h"

using namespace SVF;
using namespace llvm;
using namespace std;

// 
// LLVM & SVF class conversion.
// 
const Type* getLLVMType(const SVFType* tp);

const Value* getLLVMValue(const SVFValue* val);

const SVFType* getSVFType(const Type* tp);

const SVFValue* getSVFValue(const Value* val);

const llvm::GlobalVariable* getLLVMGlobalVariable(const SVFGlobalValue* gv);
const llvm::GlobalVariable* getLLVMGlobalVariableRep(const SVFGlobalValue* gv);
const SVFGlobalValue* getSVFGlobalValueRep(const SVFGlobalValue* gv);

const llvm::Function* getLLVMFunction(const SVFFunction* func);

const llvm::Instruction* getLLVMInstruction(const SVFInstruction* inst);

const llvm::CallInst* getLLVMCallInst(const SVFCallInst *inst);

// 
// Raw LLVM IR analysis.
// 
unsigned getStructOriginalElemNum(const StructType *stType);

int64_t getGepIndexValue(const GetElementPtrInst* gepInst, unsigned i);

const Module* getModuleFromValue(const SVFValue *val);
const Module* getModuleFromValue(const Value *val);

u64_t getTypeSize(const DataLayout* DL, const Type* type);
u64_t getTypeSize(const DataLayout* DL, const StructType *sty, u32_t field_idx);
u64_t getFieldOffset(const DataLayout* DL, const StructType *sty, u32_t field_idx);

StructType* ifPointToStruct(const SVFType* tp);
StructType* ifPointToStruct(const Type* tp);

// 
// Information printing.
// 
string printTypeWithSize(Type* type, const DataLayout& DL);

string printVal(const SVFValue* val);
string printVal(const Value* val);

string printType(const SVFType* val);
string printType(const Type* val);

void printGVType(SVFIR* pag, const SVFGlobalValue* gv);
void printGVType(SVFIR* pag, const GlobalVariable* gv);

#endif