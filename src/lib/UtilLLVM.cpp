#include "../include/Util.hpp"
#include "llvm/Support/raw_ostream.h"
#include "../include/UtilLLVM.hpp"

// 
// LLVM & SVF class conversion.
// 
const Type* getLLVMType(const SVFType* tp) {
    auto llvmType = LLVMModuleSet::getLLVMModuleSet()->getLLVMType(tp);
    return llvmType;
}

const Value* getLLVMValue(const SVFValue* val) {
    auto llvmValue = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(val);
    return llvmValue;
}

const SVFType* getSVFType(const Type* tp) {
    auto svfType = LLVMModuleSet::getLLVMModuleSet()->getSVFType(tp);
    return svfType;
}

const SVFValue* getSVFValue(const Value* val) {
    // It's like a dispatcher.
    auto svfValue = LLVMModuleSet::getLLVMModuleSet()->getSVFValue(val);
    return svfValue;
}

// Note: 这里把SVFGlobalValue转化成GlobalVariable有两点要注意。
// 1. 传入的gv参数需要来源于SVFModule::GlobalSet，否则dyn_cast这步会返回Null。
// 2. 由于该函数使用了getGlobalRep，可能多个不同SVFGlobalValue映射到一个GlobalVariable。（暂时弃用，后续再引入）
const GlobalVariable* getLLVMGlobalVariable(const SVFGlobalValue* gv) {
    auto llvmValue = getLLVMValue(gv);
    auto llvmGv = dyn_cast<GlobalVariable>(llvmValue); // llvmValue->stripPointerCasts()
    return llvmGv;
}
const GlobalVariable* getLLVMGlobalVariableRep(const SVFGlobalValue* gv) {
    auto llvmValue = getLLVMValue(gv);
    auto gvRepValue = LLVMUtil::getGlobalRep(llvmValue); // 考虑改下SVF代码，省去返回后dyn_cast的操作。
    auto llvmGv = dyn_cast<GlobalVariable>(gvRepValue); 
    return llvmGv;
}
const SVFGlobalValue* getSVFGlobalValueRep(const SVFGlobalValue* gv) {
    auto llvmGvRep = getLLVMGlobalVariableRep(gv);
    auto svfGv = LLVMModuleSet::getLLVMModuleSet()->getSVFGlobalValue(SVFUtil::cast<GlobalValue>(llvmGvRep));
    return svfGv;
}

const Function* getLLVMFunction(const SVFFunction* func) {
    auto llvmFunc = LLVMModuleSet::getLLVMModuleSet()->getLLVMFunction(func); // Use self-defined SVF function.
    // auto llvmValue = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(func);
    // auto llvmFunc = LLVMUtil::getLLVMFunction(llvmValue);
    return llvmFunc;
}

const llvm::Instruction* getLLVMInstruction(const SVFInstruction* inst) {
    auto llvmInst = LLVMModuleSet::getLLVMModuleSet()->getLLVMInstruction(inst); // Use self-defined SVF function.
    // auto llvmValue = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(inst);
    // auto llvmInst = dyn_cast<Instruction>(llvmValue); // llvmValue->stripPointerCasts()
    return llvmInst;
}

const llvm::CallInst *getLLVMCallInst(const SVFCallInst *inst) {
    auto llvmValue = getLLVMValue(inst);
    auto llvmInst = dyn_cast<CallInst>(llvmValue); // llvmValue->stripPointerCasts()
    return llvmInst;
}


// 
// Raw LLVM IR analysis.
// 
unsigned getStructOriginalElemNum(const StructType *stType) {
    unsigned idx = 0;
    const auto stInfo = SymbolTableInfo::SymbolInfo()->getTypeInfo(LLVMModuleSet::getLLVMModuleSet()->getSVFType(stType));
    while(auto type = stInfo->getOriginalElemType(idx)) {
        idx += 1;
    }
    return idx;
}

// [tool] 获取gepInst的第i个index的值（第一个index记作i=0）。
int64_t getGepIndexValue(const GetElementPtrInst* gepInst, unsigned i) {
    // assert(i < gepInst->getNumIndices() && "i >= gepInst->getNumIndices()");
    if(i >= gepInst->getNumIndices()) return 0;
    auto offsetVal = gepInst->getOperand(i+1); // 第 0 个操作数是基址指针
    auto op = SVFUtil::dyn_cast<ConstantInt>(offsetVal);
    auto idxVal = op->getSExtValue();
    return idxVal;
}

// [tool] 用于根据Value获取其所在的Module。
const llvm::Module* getModuleFromValue(const SVFValue *val) {
    auto llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(val);
    return getModuleFromValue(llvmVal);
}
const llvm::Module* getModuleFromValue(const Value *val) {
    if (!val) {
        return nullptr;
    }
    // 如果全局变量里有val的记录，则可直接返回结果。
    if(value2Module.find(val) != value2Module.end()){
        return value2Module[val];
    } else {
        // errs() << "not hit\n";
        const llvm::Module* result = nullptr;
        // TODO: LLVM IR中，全局变量声明里的getelementptr指令对应的Value没办法被下面的分支匹配。（不过这影响不大）
        // 如果是 GlobalValue（包括 GlobalVariable, Function 等）
        if (auto *global = dyn_cast<llvm::GlobalValue>(val)) {
            // errs() << "hit gv\n";
            result = global->getParent();
        } 
        // 如果是指令（Instruction）
        else if (auto *inst = dyn_cast<llvm::Instruction>(val)) {
            // errs() << "hit inst\n";
            if (auto *F = inst->getFunction()) {
                result = F->getParent();
            }
        }
        // 如果是参数（Argument）
        else if (auto *arg = dyn_cast<llvm::Argument>(val)) {
            // errs() << "hit arg\n";
            if (auto *F = arg->getParent()) {
                result = F->getParent();
            }
        }
        // 如果是基本块（BasicBlock）
        else if (auto *bb = dyn_cast<llvm::BasicBlock>(val)) {
            // errs() << "hit bb\n";
            if (auto *F = bb->getParent()) {
                result = F->getParent();
            }
        }
        // 如果是全局变量的初始化表达式（ConstantExpr）
        else if (auto *CE = dyn_cast<llvm::ConstantExpr>(val)) {
            // errs() << "hit const expr\n";
            if (GlobalVariable *GV = dyn_cast<GlobalVariable>(CE->getOperand(0))) {
                result = GV->getParent();
            }
        }
        if(result) value2Module.emplace(val, result); // 更新全局记录。
        return result;
    }
}

// [tool] 用于计算带padding的size。（Added by LHY）
// 参数DL应为当前GV所在模块的DataLayout。
u64_t getTypeSize(const DataLayout* DL, const Type* type) {
    if(!DL) {
        // errs() << "getTypeSize1: No DL!";
        return 0;
    }
    // if the type has size then simply return it, otherwise just return 0
    if(type->isSized()) {
        // 取带padding的size有以下两个选择。个人感觉后者比较靠谱。
        // return DL->getTypeStoreSize(const_cast<Type*>(type));
        return DL->getTypeAllocSize(const_cast<Type*>(type));
        // 为什么我这里返回的TypeSize没有使用getFixedSize()，也能转换成整型？
    }
    else {
        // errs() << "getTypeSize1: Type is not sized!";
        return 0;
    }
}
u64_t getTypeSize(const DataLayout* DL, const StructType *sty, u32_t field_idx) {
    if(!DL) {
        // errs() << "getTypeSize2: No DL!";
        return 0;
    }
    auto stAllSize = getTypeSize(DL, sty);
    const StructLayout *stTySL = DL->getStructLayout( const_cast<StructType *>(sty) );
    /// if this struct type does not have any element, i.e., opaque
    if(sty->isOpaque()){
        // errs() << "getTypeSize2: StructType is opaque!";
        return 0;
    }
    else {
        auto stOffsets = stTySL->getMemberOffsets();
        if(field_idx >= stOffsets.size()) return 0; // 检查field_idx是否越界。
        // return stTySL->getElementOffset(field_idx); // 按SVF的函数抄过来发现有问题，算size和offset是不同的。。。
        // 分类讨论，结尾field单独处理。
        if(field_idx!=stOffsets.size()-1){
            auto tmp = stTySL->getElementOffset(field_idx+1) - stTySL->getElementOffset(field_idx);
            return tmp;
        } else {
            // 处理结尾field。
            auto tmp = stAllSize - stTySL->getElementOffset(field_idx);
            return tmp;
        }
    }
}
u64_t getFieldOffset(const DataLayout* DL, const StructType *sty, u32_t field_idx) {
    if(!DL) {
        // errs() << "getTypeSize3: No DL!";
        return 0;
    }
    const StructLayout *stTySL = DL->getStructLayout( const_cast<StructType *>(sty) );
    if(sty->isOpaque()){
        // errs() << "getTypeSize3: StructType is opaque!";
        return 0;
    }
    else {
        auto offsets = stTySL->getMemberOffsets();
        if(field_idx >= offsets.size()) return 0;
        return stTySL->getElementOffset(field_idx);
    }
}

StructType* ifPointToStruct(const SVFType* tp) {
    auto llvmTp = LLVMModuleSet::getLLVMModuleSet()->getLLVMType(tp); // 这种map这样去查效率很低。
    return ifPointToStruct(llvmTp);
}
StructType* ifPointToStruct(const Type* tp){
    if(tp && tp->isPointerTy() && (tp->getNumContainedTypes() > 0)){
        if(auto ret = dyn_cast<StructType>(tp->getNonOpaquePointerElementType())){
            return ret;
        }else if(auto ret = dyn_cast<ArrayType>(tp->getNonOpaquePointerElementType())){
            while(isa<ArrayType>(ret->getArrayElementType())){
                ret = dyn_cast<ArrayType>(ret->getArrayElementType());
            }
            if(auto st = dyn_cast<StructType>(ret->getArrayElementType())){
                return st;
            }
        }
    }
    return nullptr;
}


// 
// Information printing.
// 
string printTypeWithSize(Type* type, const DataLayout& DL){
    string res = "";
    res += printType(type);
    string tmp;
    raw_string_ostream rso(tmp);
    // DL.getTypeStoreSize(type).print(rso);
    DL.getTypeAllocSize(type).print(rso);
    res += (" (Size=" + tmp + ") ");
    return res;
}

string printVal(const SVFValue* val) {
    auto v = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(val);
    return printVal(v);
}
string printVal(const Value* val){
	string tmp;
    raw_string_ostream rso(tmp);
    if(isa<Function>(val)){
        return val->getName().str();
    }
    val->print(rso);
    // if(rso.str().length() > 500){
    //     return "too long";
    // }else{
        return rso.str();
    // }
}

string printType(const SVFType* val) {
    auto v = LLVMModuleSet::getLLVMModuleSet()->getLLVMType(val);
    return printType(v);
}
string printType(const Type* val){
	string tmp;
    raw_string_ostream rso(tmp);
    val->print(rso);
    if(rso.str().length() > 500){
        return "too long";
    }else{
        return rso.str();
    }
}

// [tool] 打印GV的全部类型信息，如果是结构体则打印其成员的类型、index及其offset。 
void printGVType(SVFIR* pag, const SVFGlobalValue* gv) {
    auto llvmGv = getLLVMGlobalVariable(gv);
    printGVType(pag, llvmGv);
}
void printGVType(SVFIR* pag, const GlobalVariable* gv) {
    auto curLayout = gv->getParent()->getDataLayout();
    errs() << "GV Name: " << gv->getName().str() << "\n";
    // errs() << "ValVar ID: " << pag->getValueNode(gv) << "\n";
    // PAGNode* gvNode = pag->getGNode(pag->getValueNode(gv));
    auto gvType = gv->getType();
    auto elemType = gvType->getPointerElementType(); // 先去除LLVM IR给GV加的那层“指针”类型。
    errs() << "Original Type: " << printTypeWithSize(elemType, curLayout) << "\n";
    while(elemType && elemType->isArrayTy()){ // 去除“数组”类型的包裹。
        elemType = elemType->getArrayElementType();
    }
    errs() << "Stripped Type: " << printTypeWithSize(elemType, curLayout) << "\n";
    if(elemType->isSingleValueType()){
        errs() << "[Single Value Type]" << "\n";
    }else if (elemType->isStructTy()){
        errs() << "[Struct Type]" << "\n";
        StructType* stType = dyn_cast<StructType>(elemType);
        unsigned long stSize = getTypeSize(&curLayout, stType);//curLayout.getTypeAllocSize(stType).getFixedSize();
        auto stLayout = curLayout.getStructLayout(stType);
        auto stOffsets = stLayout->getMemberOffsets();
        errs() << "StructLayout: " << stOffsets.size() << " (offsets below)" << "\n";
        for (auto i : stOffsets){
            errs() << "\t" << i << "\n";
        }
        const auto stInfo = SymbolTableInfo::SymbolInfo()->getTypeInfo(LLVMModuleSet::getLLVMModuleSet()->getSVFType(stType));;
        // stInfo包含OriginalElem、FlattenedElem、FlattenedField的信息，分别遍历输出。
        errs() << "OriginalElem: " << getStructOriginalElemNum(stType) << " (sizes below)" << "\n";
        unsigned long idx = 0;
        unsigned long sizeCnt = 0;
        while(auto i = stInfo->getOriginalElemType(idx)){
            auto tmp = getTypeSize(&curLayout, stType, idx);//curLayout.getTypeAllocSize(const_cast<Type*>(i)).getFixedSize();
            errs() << '\t' << tmp << " (" << printType(i) << ")\n";
            sizeCnt += tmp;
            idx++;
        }
        assert(sizeCnt == stSize && "OriginalElem Size Mismatch");
        // errs() << "FlattenedElem: " << stInfo->getNumOfFlattenElements() << "\n";
        // for (auto i : stInfo->getFlattenElementTypes()) {
        //     errs() << '\t' << printType(i) << "\n";
        // }
        // errs() << "FlattenedField: " << stInfo->getNumOfFlattenFields() << "\n";
        // for (auto i : stInfo->getFlattenFieldTypes()) {
        //     errs() << '\t' << printType(i) << "\n";
        // }
    }
    errs() << "\n";
}