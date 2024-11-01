#include "../include/Util.hpp"
#include "../include/UtilLLVM.hpp"
#include "llvm/Support/raw_ostream.h"

// llvm::cl::opt<std::string> SpecifyInput("SpecifyInput",
//     llvm::cl::desc("specify input such as indirect calls or global variables"), llvm::cl::init(""));

unordered_set<string> blackCalls;
unordered_set<string> blackRets;

unordered_set<NodeID> blackNodes;

unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiIn;
unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiOut;

unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectIn;
unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectOut;

// unordered_map<const SVFType*, unordered_set<const SVFFunction*>> type2funcs;

// Shortcuts相关。
unordered_map<string, unordered_map<u32_t, unordered_set<PAGEdge*>>> typebasedShortcuts; // {structName -> {offset -> PAGEdgeSet}}
unordered_map<string, unordered_map<u32_t, unordered_set<unordered_set<PAGEdge*>*>>> additionalShortcuts; // {structName -> {offset -> ...}}
unordered_map<string, unordered_set<PAGEdge*>> castSites; // {structName -> PAGEdgeSet}
unordered_map<PAGEdge*, unordered_map<u32_t, unordered_set<string>>> reverseShortcuts;
unordered_map<PAGNode*, PAGEdge*> gepIn; // 把GEP边的DestNode映射到GEP边。

// Field-sensitivity相关。
unordered_map<const PAGEdge*, long> gep2byteoffset; // {FieldEdge -> byteOffset} // 记录Field边对应的byteOffset值。
unordered_set<const PAGEdge*> variantGep; // [FieldEdges] // 记录所有offset为non-constant的Field边。

// 记录Value与其对应的Module，便于我们恢复Datalayout信息。(Added by LHY)
unordered_map<const Value*, const Module*> value2Module; // Don't refactor this to SVFValue. 

unordered_map<StructType*, string> deAnonymousStructs;   // Refactor this to SVFStructType???

// CallGraph相关。
unordered_map<const CallInst*, unordered_set<const Function*>> callgraph; // Refactor this to SVFCallInst and SVFFunction???
unordered_map<NodeID, unordered_set<NodeID>> Real2Formal;
unordered_map<NodeID, unordered_set<NodeID>> Formal2Real;
unordered_map<NodeID, unordered_set<NodeID>> Ret2Call;
unordered_map<NodeID, unordered_set<NodeID>> Call2Ret;
bool ifCallGraphSet = false;

void sortMap(std::vector<pair<PAGNode*, u64_t>> &sorted, unordered_map<PAGNode*, u64_t> &before, int k){
    sorted.reserve(before.size());
    for (const auto& kv : before) {
        sorted.emplace_back(kv.first, kv.second);
    }
    if(sorted.size() <= k){
        return;
    }
    std::stable_sort(std::begin(sorted), std::end(sorted),
                    [](const pair<PAGNode*, u64_t>& a, const pair<PAGNode*, u64_t>& b) { return a.second > b.second; });
    // std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end(), [](const pair<PAGNode*, u64_t>& a, const pair<PAGNode*, u64_t>& b) { return a.second > b.second; });
}

uint64_t getSizeInBytes(Type *type) {
    if (type->isIntegerTy()) {
        // Integer types (i8, i16, i32, i64, etc.)
        return type->getPrimitiveSizeInBits() / 8;
    } else if (type->isFloatingPointTy()) {
        // Floating-point types (float, double, etc.)
        return type->getPrimitiveSizeInBits() / 8;
    } else if (type->isPointerTy()) {
        // Pointer types (assuming 64-bit pointers)
        return 8;
    } else if (StructType *sttype = dyn_cast<StructType>(type)) {
        // Struct types
        uint64_t size = 0;
        for (Type *elementType : sttype->elements()) {
            // Add the size of each element (assuming no padding for simplicity)
            size += getSizeInBytes(elementType);
        }
        return size;
    } else if (ArrayType *arrayType = dyn_cast<ArrayType>(type)) {
        // Array types
        uint64_t elementSize = getSizeInBytes(arrayType->getElementType());
        return elementSize * arrayType->getNumElements();
    } else {
        // Other types (not handled in this example)
        return 0;
    }
}

// [initialize]
void getBlackNodes(SVFIR* pag){
    errs() << "[initialize] Exec getBlackNodes...\n";
    // dataflow cannot through constants
    for(auto edge : pag->getGNode(pag->getConstantNode())->getOutgoingEdges(PAGEdge::Addr)){
        blackNodes.insert(edge->getDstID());
    }
    errs() << "blackConsts: " << blackNodes.size() << "\n";

    // dummy nodes in pag
    for(u32_t i = 0; i < 4; i++){
        blackNodes.insert(i);
    }

    // llvm.compiler.used
    const auto llvm_compiler_used = pag->getGNode(4);
    if(llvm_compiler_used->hasValue() && llvm_compiler_used->getValueName() == "llvm.compiler.used"){
        blackNodes.insert(4);
    }

    // Func-calls
    unordered_map<const Function*, unsigned int> callees;
    unordered_map<const Function*, unordered_set<NodeID>> calleeNodes;
    unordered_set<NodeID> blackCalls;
    for(auto calledge : pag->getSVFStmtSet(PAGEdge::Call)){
        // TODO: No elegant here...
        auto callPE = dyn_cast<CallPE>(calledge);
        auto callInst = dyn_cast<SVFCallInst>(callPE->getCallInst()->getCallSite());
        auto llvmCallInst = getLLVMCallInst(callInst);
        auto callee = dyn_cast<Function>(llvmCallInst->getCalledOperand()->stripPointerCasts());
        callees[callee]++;
        calleeNodes[callee].insert(calledge->getDstID());
    }
    for(auto callee : callees){
        if(callee.second / (callee.first->arg_size() + 1) > BASE_NUM * 5){
            if(callee.first->getName().find('.') == string::npos){
                for(auto node : calleeNodes[callee.first]){
                    blackNodes.insert(node);
                    blackCalls.insert(node);
                }
            }else if(callee.second / (callee.first->arg_size() + 1) > BASE_NUM * 10){
                for(auto node : calleeNodes[callee.first]){
                    blackNodes.insert(node);
                    blackCalls.insert(node);
                }
            }
        }
    }
    errs() << "blackCalls: " << blackCalls.size() << "\n";

    unordered_map<string, unsigned int> rets;
    unordered_map<string, unordered_set<NodeID>> retNodes;
    unordered_set<NodeID> blackRets;
    for(auto retEdge : pag->getSVFStmtSet(PAGEdge::Ret)){
        auto retinst = dyn_cast<RetPE>(retEdge);
        auto func = SVFUtil::getCallee(retinst->getCallInst()->getCallSite())->getName();
        rets[func]++;
        retNodes[func].insert(retEdge->getSrcID());
    }
    for(auto ret : rets){
        if(ret.second > BASE_NUM * 5){
            if(ret.first.find('.') == string::npos){
                for(auto node : retNodes[ret.first]){
                    blackNodes.insert(node);
                    blackRets.insert(node);
                }
            }else if(ret.second > BASE_NUM * 10){
                for(auto node : retNodes[ret.first]){
                    blackNodes.insert(node);
                    blackRets.insert(node);
                }
            }
        }
    }
    errs() << "blackRets: " << blackRets.size() << "\n";
    
    for(auto i = 0; i < pag->getNodeNumAfterPAGBuild(); i++){
        auto node = pag->getGNode(i);
        if(node->getIncomingEdges(PAGEdge::Store).size() > O_BASE * 50){
            blackNodes.insert(i);
        }
        if(node->getOutgoingEdges(PAGEdge::Store).size() > O_BASE * 10){
            blackNodes.insert(i);
        }
        if(node->getOutgoingEdges(PAGEdge::Copy).size() > O_BASE * 15){
            blackNodes.insert(i);
        }
        if(node->getOutgoingEdges(PAGEdge::Load).size() > O_BASE * 5){
            blackNodes.insert(i);
        }
    }

    for(auto node : Call2Ret){
        if(node.second.size() > BASE_NUM){
            blackNodes.insert(node.first);
        }
    }
    for(auto node : Ret2Call){
        if(node.second.size() > BASE_NUM){
            blackNodes.insert(node.first);
        }
    }
    for(auto node : Formal2Real){
        if(node.second.size() > BASE_NUM){
            blackNodes.insert(node.first);
        }
    }
    for(auto node : Real2Formal){
        if(node.second.size() > BASE_NUM * 2.5){
            blackNodes.insert(node.first);
        }
    }

    errs() << "blackNodes: " << blackNodes.size() << "\n";
}

// [initialize]
void setupPhiEdges(SVFIR* pag){
    for(auto edge : pag->getPTASVFStmtSet(PAGEdge::Phi)){
        const auto phi = dyn_cast<PhiStmt>(edge);
        const auto dst = edge->getDstNode();
        for(auto var : phi->getOpndVars()){
            if(blackNodes.find(var->getId()) == blackNodes.end()){
                phiIn[dst->getId()][edge].insert(var->getId());
                phiOut[var->getId()][edge].insert(dst->getId());
            }
        }
    }
    errs() << "[initialize] Finish setupPhiEdges!\n";
}

// [initialize]
void setupSelectEdges(SVFIR* pag){
    for(auto edge : pag->getPTASVFStmtSet(PAGEdge::Select)){
        const auto select = dyn_cast<SelectStmt>(edge);
        const auto dst = edge->getDstNode();
        selectIn[dst->getId()][edge].insert(select->getTrueValue()->getId());
        selectIn[dst->getId()][edge].insert(select->getFalseValue()->getId());
        selectOut[select->getTrueValue()->getId()][edge].insert(dst->getId());
        selectOut[select->getFalseValue()->getId()][edge].insert(dst->getId());
    }
    errs() << "[initialize] Finish setupSelectEdges!\n";
}

// [tool] 以一种硬编码的方式计算Type的字节数大小，供很多其他工具函数调用。
// 虽然这样做没考虑alignment，但在DataLayout不可用的情况下也可回退到此处。
uint64_t getManualTypeSize(Type *Ty) {
    if (Ty->isIntegerTy()) {
        // Return the size of integer types in bytes
        return Ty->getIntegerBitWidth() / 8;
    } else if (Ty->isFloatTy()) {
        return 4; // Size of float
    } else if (Ty->isDoubleTy()) {
        return 8; // Size of double
    } else if (Ty->isArrayTy()) { // 处理数组时，确实是“元素个数*元素大小”。
        Type *ElementType = Ty->getArrayElementType();
        uint64_t ElementSize = getManualTypeSize(ElementType);
        return ElementSize * Ty->getArrayNumElements();
    } else if (Ty->isStructTy()) { // 处理结构体时，遍历所有成员element并递归计算size。
        uint64_t StructSize = 0;
        for (Type *Element : dyn_cast<StructType>(Ty)->elements()) {
            StructSize += getManualTypeSize(Element);
        }
        return StructSize;
    } else if (Ty->isPointerTy()) {
        return 8; // Assuming 64-bit pointers
    } else{
        // errs() << printType(Ty) << "\n";
    }
    // Add more type cases as needed
    return 0;
}

// [tool] getStructName函数也是个重要的工具函数，在Util和UniasAlgo里都有使用。
// TODO: 把里面getManualTypeSize的计算方法替换掉。
bool deAnonymous = false;
string getStructName(StructType* sttype){
    auto origin_name = sttype->getStructName().str();
    if(origin_name.find(".anon.") != string::npos){
        const auto fieldNum = SymbolTableInfo::SymbolInfo()->getNumOfFlattenElements(LLVMModuleSet::getLLVMModuleSet()->getSVFType(sttype));;
        
        // const auto stsize = DL->getTypeStoreSize(sttype);
        auto stsize = getManualTypeSize(sttype);
        return to_string(fieldNum) + "," + to_string(stsize);
    }
    if(origin_name.find("union.") != string::npos){
        if(origin_name.rfind('.') == 5){
            return origin_name;
        }else{
            return origin_name.substr(0, origin_name.rfind('.'));
        }
    }
    if(origin_name.find("struct." )!= string::npos){
        if(origin_name.rfind('.') == 6){
            return origin_name;
        }else{
            return origin_name.substr(0, origin_name.rfind('.'));
        }
    }
    if(deAnonymous){
        if(deAnonymousStructs.find(sttype) != deAnonymousStructs.end()){
            return deAnonymousStructs[sttype];
        }else{
            const auto fieldNum = SymbolTableInfo::SymbolInfo()->getNumOfFlattenElements(LLVMModuleSet::getLLVMModuleSet()->getSVFType(sttype));
            // const auto stsize = DL->getTypeStoreSize(sttype);
            auto stsize = getManualTypeSize(sttype);
            return to_string(fieldNum) + "," + to_string(stsize);
        }
    }
    return "";
}

unordered_set<CallInst*>* getSpecificGV(SVFModule* svfmod);

unordered_set<PAGNode*> addrvisited;

bool checkIfAddrTaken(SVFIR* pag, PAGNode* node){
    if(!addrvisited.insert(node).second){
        return false;
    }
    if(node->hasOutgoingEdges(PAGEdge::Store)){
        return true;
    }
    if(phiOut.find(node->getId()) != phiOut.end()){
        for(auto edge : phiOut[node->getId()]){
            for(auto nxt : edge.second){
                if(checkIfAddrTaken(pag, pag->getGNode(nxt))){
                    return true;
                }
            }
        }
    }
    for(auto edge : node->getOutEdges()){
        if(checkIfAddrTaken(pag, edge->getDstNode())){
            return true;
        }
    }
    addrvisited.erase(node);
    return false;
}

// [initialize] 用于KallGraph。
// void addSVFAddrFuncs(SVFModule* svfModule, SVFIR* pag){
// 	for(auto F : *svfModule){
// 		auto funcnode = pag->getGNode(pag->getValueNode(F));
//         addrvisited.clear();
// 		if(checkIfAddrTaken(pag, funcnode)){
//             type2funcs[F->getType()].emplace(F);
// 		}
// 	}
// }

// [initialize]
void handleAnonymousStruct(SVFModule* svfModule, SVFIR* pag){
    errs() << "[initialize] Exec handleAnonymousStruct...\n";
    unordered_map<StructType*, unordered_set<SVFGlobalValue*>> AnonymousTypeGVs;
    errs() << "SVFModule GV size: " << svfModule->getGlobalSet().size() << "\n";
    for(auto ii = svfModule->global_begin(), ie = svfModule->global_end(); ii != ie; ii++){
        auto gv = *ii;
        if(auto gvtype = ifPointToStruct(gv->getType())){
            // errs() << "  Cur PointToStruct GV: " << gv->getName() << "\n";
            if(getStructName(gvtype) == ""){
                AnonymousTypeGVs[gvtype].emplace(gv);
            }
        }
    }
    for(auto edge : pag->getSVFStmtSet(PAGEdge::Copy)){
        if(edge->getSrcNode()->getType() && edge->getDstNode()->getType()){
            auto srcType = ifPointToStruct(edge->getSrcNode()->getType());
            auto dstType = ifPointToStruct(edge->getDstNode()->getType());
            if(srcType && dstType && (srcType != dstType)){
                if(AnonymousTypeGVs.find(srcType) != AnonymousTypeGVs.end()){
                    if(getStructName(dstType) != ""){
                        deAnonymousStructs[srcType] = getStructName(dstType);
                        AnonymousTypeGVs.erase(srcType);
                    }
                }else if(AnonymousTypeGVs.find(dstType) != AnonymousTypeGVs.end()){
                    if(getStructName(srcType) != ""){
                        deAnonymousStructs[dstType] = getStructName(srcType);
                        AnonymousTypeGVs.erase(dstType);
                    }
                }
            }
        }
    }
    for(auto const edge : pag->getSVFStmtSet(PAGEdge::Gep)){
        const auto gepstmt = dyn_cast<GepStmt>(edge);
        if(auto callinst = dyn_cast<CallInst>(getLLVMValue(edge->getValue()))){
            // memset, memcpy, llvm.memmove.p0i8.p0i8.i64, llvm.memset.p0i8.i64, llvm.memcpy.p0i8.p0i8.i64
            if(callinst->getCalledFunction()->getName().find("memset") == string::npos){
                if(callinst->arg_size() >= 2){
                    if(auto st1 = ifPointToStruct(callinst->getArgOperand(0)->stripPointerCasts()->getType())){
                        if(auto st2 = ifPointToStruct(callinst->getArgOperand(1)->stripPointerCasts()->getType())){
                            auto st1name = getStructName(st1);
                            auto st2name = getStructName(st2);
                            if(st1name != "" && st2name == ""){
                                deAnonymousStructs[st2] = st1name;
                                if(AnonymousTypeGVs.find(st2) != AnonymousTypeGVs.end()){
                                    AnonymousTypeGVs.erase(st2);
                                }
                            }else if(st1name == "" && st2name != ""){
                                deAnonymousStructs[st1] = st2name;
                                if(AnonymousTypeGVs.find(st1) != AnonymousTypeGVs.end()){
                                    AnonymousTypeGVs.erase(st1);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    deAnonymous = true;
    errs() << "[initialize] Finish handleAnonymousStruct!\n";
}

// [tool] 用于collectByteoffset。
// 
long varStructVisit(GEPOperator* gepop, const DataLayout* DL){
    // errs() << "  [varStructVisit]\n";
    if(!DL) {
        errs() << "varStructVisit: DataLayout is not available!\n";
    }
    long ret = 0;
    bool first = true;
    // ToSee: 看不懂bridge_gep的用法。应该是参考SVF里的SVFIRBuilder::computeGepOffset(...)函数的写法。
    for (bridge_gep_iterator gi = bridge_gep_begin(*gepop), ge = bridge_gep_end(*gepop); gi != ge; ++gi)
    {
        Type* gepTy = *gi;
        Value* offsetVal = gi.getOperand();
        auto idx = dyn_cast<ConstantInt>(offsetVal);
        if(idx){
            if(first){// 跳过第一个idx，因为第一个idx通常是0。
                first = false;
                continue;
            }else{
                if(gepTy->isArrayTy()){
                    // 处理数组。不累加byteOffset，一律视作对数组开头的操作。（Unias设计为array-insensitive）
                    continue;
                }else if(auto sttype = dyn_cast<StructType>(gepTy)){
                    // 处理结构体。根据idx前面各成员的大小累加byteOffset。
                    for(int fd = 0; fd < idx->getSExtValue(); fd++){
                        ret += getTypeSize(DL, sttype, fd);
                        // auto rtype = sttype->getElementType(fd);
                        // // ret += DL->getTypeStoreSize(rtype);
                        // ret += getManualTypeSize(rtype);
                    }
                }else{
                    assert(sttype && "could only be struct");
                }
            }
        }
    }
    return ret;
}

// [tool] 用于collectByteoffset函数。
// 根据给定GEP边的结构体类型和成员index，计算其byteOffset并返回。
// 重点重构对象！
long regularStructVisit(StructType* sttype, s64_t idx, PAGEdge* gep, const DataLayout* DL){
    // errs() << "  [regularStructVisit]\n";
    if(!DL) {
        errs() << "regularStructVisit: DataLayout is not available!\n"; // Triggered.
    }
    // errs() << "regularStructVisit: " << printVal(gep->getValue()) << "\n"; // For Debug.
    // ret byteoffset
    long ret = 0;
    const auto stinfo = SymbolTableInfo::SymbolInfo()->getTypeInfo(LLVMModuleSet::getLLVMModuleSet()->getSVFType(sttype)); //OrderedMap<const Type *, StInfo *>
    // Refactor: 用unsigned getStructOriginalElemNum(const StructType *stType)。
    u32_t stOriginalElemNum = getStructOriginalElemNum(sttype);
    u32_t lastOriginalType = 0;
    for(auto i = 0; i <= idx; i++){
        if(stinfo->getOriginalElemType(i)){ // 疑问：这里的i没有加到idx参数的大小怎么办？答：在下面的if语句处理。
            lastOriginalType = i;
        }
    }
    // 遍历idx前面所有成员element，累加各成员对应类型的byte size。
    // Cumulate previous byteoffset
    for(auto i = 0; i < lastOriginalType; i++){
        ret += getTypeSize(DL, sttype, i);
        // if(stinfo->getOriginalElemType(i)){
        //     auto rtype = const_cast<Type*>(stinfo->getOriginalElemType(i));
        //     // ret += DL->getTypeStoreSize(rtype);
        //     ret += getManualTypeSize(rtype);
        // }
    }
    // Check if completed
    // 理解：
    if(idx - lastOriginalType >= 0){
        auto svfEmbType = stinfo->getOriginalElemType(lastOriginalType);
        if(svfEmbType) {
            auto embType = getLLVMType(svfEmbType);
            while(embType && embType->isArrayTy()){
                embType = embType->getArrayElementType();
            }
            if(embType && embType->isStructTy()){
                ret += regularStructVisit(const_cast<StructType*>(dyn_cast<StructType>(embType)), idx - lastOriginalType, gep, DL);
            }
        }
    }
    // 记录全局变量并返回byteOffset值。
    const auto stname = getStructName(sttype);
    if(stname != ""){
        typebasedShortcuts[stname][ret].insert(gep);
        reverseShortcuts[gep][ret].insert(stname);
        // For edge struct.A.B.C, only allow B.C edge, and A.B.C edge to here,
        // But for this edge, we don't know other B.C edges yet
    }
    return ret;
}

// [tool] 用于setupStores。
// 把node及其srcNodes递归地加入visitedNodes中。
void getSrcNodes(PAGNode* node, unordered_set<PAGNode*> &visitedNodes){
    if(visitedNodes.insert(node).second){ // 表示插入成功，即visitedNodes中本没有这个node。
        for(auto edge : node->getIncomingEdges(PAGEdge::Copy)){
            getSrcNodes(edge->getSrcNode(), visitedNodes);
        }
    }
}

// [initialize] 用于设置additionalShortcuts。
void setupStores(SVFIR* pag){
    for(auto edge : pag->getSVFStmtSet(PAGEdge::Store)){
        unordered_set<PAGNode*> srcNodes;
        unordered_set<PAGNode*> dstNodes;
        for(auto srcLoad : edge->getSrcNode()->getIncomingEdges(PAGEdge::Load)){
            getSrcNodes(srcLoad->getSrcNode(), srcNodes);
        }
        getSrcNodes(edge->getDstNode(), dstNodes);

        for(auto dstNode : dstNodes){
            if(gepIn.find(dstNode) != gepIn.end() && reverseShortcuts.find(gepIn[dstNode]) != reverseShortcuts.end()){
                for(auto srcNode : srcNodes){
                    if(gepIn.find(srcNode) != gepIn.end() && reverseShortcuts.find(gepIn[srcNode]) != reverseShortcuts.end()){
                        for(auto srcIdx : reverseShortcuts[gepIn[srcNode]]){
                            for(auto srcName : srcIdx.second){
                                for(auto dstIdx : reverseShortcuts[gepIn[dstNode]]){
                                    for(auto dstName : dstIdx.second){
                                        if(srcName != dstName){
                                            additionalShortcuts[dstName][dstIdx.first].insert(&typebasedShortcuts[srcName][srcIdx.first]);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    errs() << "additional shortcuts: " << additionalShortcuts.size() << "\n";
    reverseShortcuts.clear();
    gepIn.clear();
    errs() << "[initialize] Finish setupStores!\n";
}

// [tool] 用于collectByteoffset。
// 判断node是否之前处理过，同时尝试沿着Copy边确定node对应的结构体类型。
StructType* gotStructSrc(PAGNode* node, unordered_set<PAGNode*> &visitedNodes){
    if(!visitedNodes.insert(node).second){
        return nullptr;
    }
    for(auto nxt : node->getIncomingEdges(PAGEdge::Copy)){
        if(auto nxtType = nxt->getSrcNode()->getType()){
            auto llvmNxtTyoe = LLVMModuleSet::getLLVMModuleSet()->getLLVMType(nxtType);
            if(llvmNxtTyoe->isPointerTy() && llvmNxtTyoe->getNumContainedTypes() > 0){
                auto elemType = llvmNxtTyoe->getPointerElementType();
                while(elemType->isArrayTy()){
                    elemType = elemType->getArrayElementType();
                }
                if(elemType->isStructTy()){
                    return dyn_cast<StructType>(elemType);
                }
            }
            if(auto ret = gotStructSrc(nxt->getSrcNode(), visitedNodes)){
                return ret;
            }
        }
    }
    visitedNodes.erase(node);
    return nullptr;
}

void debugGEP(const PAGEdge* edge) {
    errs() << "GEP SrcNode: " << printVal(edge->getSrcNode()->getValue()) << "\n";
    errs() << "GEP DstNode: " << printVal(edge->getDstNode()->getValue()) << "\n";
    errs() << "    ";
    if(gep2byteoffset.find(edge) != gep2byteoffset.end()) {
        errs() << "[GEP, byteOffset: " << gep2byteoffset[edge] << "]";
    }
    if(variantGep.find(edge) != variantGep.end()) {
        errs() << "[Variant GEP]";
    }
    errs() << "\n";
}

// [initialize] 负责gep2byteoffset和variantGep的初始化。 
// 实现field-sensitive的非常重要的函数，里面还调用了一些复杂的工具函数。（非递归函数）
// 结构体嵌套以及多级index的gep指令是怎么处理的？一般就两个"type+idx"，然后多个gep指令来构成多级索引。
void collectByteoffset(SVFIR* pag){
    // 遍历PAG中的所有GEP边。将其转化为对应的LLVMInstruction并进行相关处理。
    // errs() << "[collectByteoffset] GEP Edges size: " << pag->getSVFStmtSet(PAGEdge::Gep).size() << "\n";
    for(auto const edge : pag->getSVFStmtSet(PAGEdge::Gep)){
        // errs() << "[collectByteoffset] Cur GEP: " << edge->getEdgeID() << "\n";
        // 此处PAGEdge edge其实都应该是GepStmt类型。
        const auto gepstmt = dyn_cast<GepStmt>(edge);
        if(!gepstmt){
            errs() << "[collectByteoffset] Can't get gepstmt!\n"; // not reached.
            continue;
        }
        // errs()<<"  gepstmt: "<<gepstmt->toString()<<"\n";
        // 理解：edge是SVFStatement，其配套了一个SVFValue。我们希望由此得到LLVM Instruction。
        auto llvmValue = getLLVMValue(edge->getValue());
        if(!llvmValue) {
            errs() << "[collectByteoffset] Can't get llvmValue for GepStmt!\n"; // not reached.
            continue;
        }
        // errs()<<"  llvmValue: "; llvmValue->print(errs()); errs()<<"\n";

        // 获取当前GEP边所在module的DataLayout。
        const Module* mod = getModuleFromValue(edge->getValue());
        const DataLayout* DL;
        if(mod) {
            DL = &mod->getDataLayout();
        } else {
            DL = nullptr;
            errs() << "[collectByteoffset] Fail to getModuleFromValue: "<< printVal(edge->getValue()) <<"\n"; // Triggered.
            continue;
            // 这主要会影响：regularStructVisit、varStructVisit、标量数组的处理。本质上还是影响了getTypeSize函数。
        }
        gepIn[edge->getDstNode()] = edge;
        
        // auto svfGepInst = edge->getInst();
        // if(!svfGepInst) {
        //     errs() << "[collectByteoffset] Can't get svfGepInst!\n"; // reachable. 说明GepStmt相应的SVFValue也许不是一个SVFInstruction。
        //     continue;
        // }
        // errs() << "  svfGepInst: " << svfGepInst->toString() << "\n";

        // const auto gepInst = dyn_cast<GetElementPtrInst>(llvmValue); // 这样获取gepInst极有可能得到nullptr。
        // errs() << "  gepInst: "; gepInst->print(errs()); errs()<<"\n"; // Problem: 空!

        // 处理getelementptr指令被嵌在call指令中的情况：需要借助gotStructSrc去推断base的类型。因为mem函数传参时常常会有类型转换。
        if(auto callInst = dyn_cast<CallInst>(llvmValue)){
            errs() << "  Enter branch (getelementptr in callInst).\n";
            // memset, memcpy, llvm.memmove.p0i8.p0i8.i64, llvm.memset.p0i8.i64, llvm.memcpy.p0i8.p0i8.i64
            // 函数名不包含memset才能进入if body。
            if(callInst->getCalledFunction()->getName().find("memset") == string::npos){
                unordered_set<PAGNode*> visitedNodes; // 仅用于getSrcNodes函数。
                if(auto sttype = gotStructSrc(edge->getSrcNode(), visitedNodes)){ // 通过Copy边去尝试推断结构体类型。
                    // 常规的结构体成员访问。
                    gep2byteoffset[edge] = regularStructVisit(sttype, gepstmt->getConstantStructFldIdx(), edge, DL);
                    // debugGEP(edge);
                }else{
                    if(!gepstmt->isVariantFieldGep() && gepstmt->isConstantOffset() && edge->getSrcNode()->getOutgoingEdges(PAGEdge::Gep).size() < 20){
                        // 非结构体的偏移访问（通常类型为i8，直接把index作为byteOffset）。
                        gep2byteoffset[edge] = gepstmt->getConstantStructFldIdx();
                        // debugGEP(edge);
                    }else{
                        // non-constant的成员访问。
                        variantGep.insert(edge);
                        // debugGEP(edge);
                    }
                }
            }
            continue;
        }

        // 处理正常的、单独的getelementptr操作。
        auto svfType = edge->getSrcNode()->getType();
        if(!svfType){
            errs() << "[collectByteoffset] Can't get svvfType for GepStmt SrcNode!\n"; // Triggered.
            continue;
        }
        auto llvmType = getLLVMType(svfType);
        if(!llvmType){
            errs() << "[collectByteoffset] Can't get llvmType for GepStmt SrcNode!\n"; // Triggered.
            continue;
        }
        // 要求Field边的Src节点是结构体指针类型。根据base的type和GEP的index计算byteOffset。
        if(llvmType->isPointerTy() && llvmType->getNumContainedTypes() > 0){
            auto elemType = llvmType->getPointerElementType();
            while(elemType && elemType->isArrayTy()){
                elemType = elemType->getArrayElementType();
            }
            // elemType是从GEP边SrcNode拿到的结构体/数组的基础类型，然后根据idx计算byteOffset。
            if(elemType){
                // 处理non-constant的成员访问。
                if(!gepstmt->isConstantOffset()){
                    // contains non-const index
                    if(elemType->isSingleValueType()){
                        // gep i8*, i64 0, %var
                        // 这种情况下，处理的是标量数组索引的Gep边。索引值为variant。
                        variantGep.insert(edge);
                        // debugGEP(edge);
                    }else if(elemType->isStructTy()){
                        if(gepstmt->isVariantFieldGep()){
                            // gep struct.A*, %var
                            // 这种情况下，处理的是结构体数组索引的Gep边。索引值为variant。
                            gep2byteoffset[edge] = 0;
                            // debugGEP(edge);
                        }else{
                            // getelementptr %struct.acpi_pnp_device_id_list, %struct.acpi_pnp_device_id_list* %8, i64 0, i32 2, i64 %indvars.iv, i32 1
                            // 这种情况下，处理的是结构体成员访问的Gep边。某个子索引值为variant。
                            gep2byteoffset[edge] = varStructVisit(const_cast<GEPOperator*>(dyn_cast<GEPOperator>(getLLVMValue(edge->getValue()))), DL);
                            // debugGEP(edge);
                        }
                    }else{
                        assert(false && "no ther case 1");
                    }
                }
                // 处理常量index的成员访问。
                else{
                    if(elemType->isSingleValueType()){
                        // 这种情况下，edge->getSrcNode()的类型是标量数组（可能是多维数组）。
                        // gep2byteoffset[edge] = DL->getTypeStoreSize(type->getPointerElementType()) * gepstmt->accumulateConstantOffset();
                        // 此处代码是错的：第一个乘数应该取elemType；第二个乘数试了几个case都是0。
                        // gep2byteoffset[edge] = getManualTypeSize(type->getPointerElementType()) * gepstmt->accumulateConstantOffset(); 
                        // 个人感觉这里也不能用SVF的GepStmt做分析，还是应该直接拿到GetElementPtrInst进行分析。
                        // gep2byteoffset[edge] = getTypeSize(DL, elemType) * gepstmt->getConstantFieldIdx();
                        // Anyways保守点就直接记录为0。相当于回退到array-insensitive，只访问数组第一个元素。
                        const auto gepInst = dyn_cast<GetElementPtrInst>(llvmValue);
                        if(!gepInst) {
                            errs() << "[collectByteoffset] Can't get gepInst from llvmValue!\n"; // Triggered many times.
                            continue;
                        }
                        if(gepInst) {
                            if(gepInst->getNumIndices()==1) { 
                                // 处理指针变量的加减法（可看作raw方式的数组索引）。
                                gep2byteoffset[edge] = getTypeSize(DL, elemType) * getGepIndexValue(gepInst, 0);
                            } else if(gepInst->getNumIndices()==2) {
                                // 处理普通的一维数组。
                                gep2byteoffset[edge] = getTypeSize(DL, elemType) * getGepIndexValue(gepInst, 1);
                            }
                            else if(gepInst->getNumIndices() > 2) {
                                // TODO: 处理多维数组。
                                gep2byteoffset[edge] = 0;
                            } else {
                                gep2byteoffset[edge] = 0;
                            }
                            // debugGEP(edge);
                        }
                        // errs() << "\nArray???: " << gep2byteoffset[edge] << "\t" << printVal(edge->getValue()) << "\n";
                        // gepstmt->getLocationSet().dump(); errs() << "; " << getTypeSize(DL, elemType) << "\t" << gepstmt->getConstantFieldIdx() << "\n";
                    }else if (elemType->isStructTy()){
                        // 这种情况下，edge->getSrcNode()的类型是结构体或结构体数组。
                        StructType* stType = dyn_cast<StructType>(elemType);
                        s64_t idx = gepstmt->getConstantStructFldIdx();
                        gep2byteoffset[edge] = regularStructVisit(stType, idx, edge, DL);
                        // debugGEP(edge);
                    }else{
                        assert(false && "no other case 2"); // Unias分析kernel不会走到这里。
                    }
                }
            }
        }else{
            // 跑实验证明其实不太会跑到这个分支。
            errs() << printVal(edge->getValue()) << "\n";
        }
    }
    errs() << "[initialize] Finish collectByteoffset!\n";
    errs() << "gep2byteoffset Num: " << gep2byteoffset.size() << "\n";  // 1037227
    errs() << "variantGep Num: " << variantGep.size() << "\n";  // 33988

    // For debug. 只要Field-sensitivity或offset出问题，就从这里调试。验证每条GEP边的byteOffset是否正确。
    // for (const auto &entry : gep2byteoffset) {
    //     const PAGEdge *edge = entry.first;
    //     debugGEP(edge);
    // }
    // for (const auto &edge : variantGep) {
    //     debugGEP(edge);
    // }
}

// [initialize]
void processCastSites(SVFIR* pag){
    for(auto edge : pag->getSVFStmtSet(SVFStmt::Copy)){
        if(edge->getSrcNode()->getType() != edge->getDstNode()->getType()){
            if(edge->getSrcNode()->getType()){
                if(auto sttype = ifPointToStruct(edge->getSrcNode()->getType())){
                    castSites[getStructName(sttype)].insert(edge);
                }
            }
            if(edge->getDstNode()->getType()){
                if(auto sttype = ifPointToStruct(edge->getDstNode()->getType())){
                    castSites[getStructName(sttype)].insert(edge);
                }
            }   
        }
    }
    errs() << "[initialize] Finish processCastSites!\n";
}

// [initialize]
void readCallGraph(string filename, SVFModule* mod, SVFIR* pag){
    unordered_map<string, const CallInst*> callinstsmap;
    unordered_map<string, const Function*> funcsmap;
    for(auto func : *mod){
        auto llvmFunc = getLLVMFunction(func);
        string funcname = llvmFunc->getName().str();
        funcsmap[funcname] = llvmFunc;
        for(auto &bb : *llvmFunc){
            for(auto &inst : bb){
                if(auto callinst = dyn_cast<CallInst>(&inst)){
                    if(callinst->isIndirectCall()){
                        auto svfCallInst = LLVMModuleSet::getLLVMModuleSet()->getSVFInstruction(&inst);
                        callinstsmap[to_string(pag->getValueNode(svfCallInst))] = callinst;
                    }
                }
            }
        }
    }

    ifstream fin(filename);
    string callsite, callee;
    u32_t calleenum;
    while(!fin.eof()){
        fin >> callsite;
        fin >> calleenum;
        for(long i = 0; i < calleenum; i++){
            fin >> callee;
            callgraph[callinstsmap[callsite]].insert(funcsmap[callee]);
        }
    }
    fin.close();
}

// [initialize]
void setupCallGraph(SVFIR* _pag){
    for(const auto callinst : callgraph){
        const auto argsize = callinst.first->arg_size();
        for(const auto callee : callinst.second){
            if(argsize == callee->arg_size()){
                for(unsigned int i = 0; i < argsize; i++){
                    auto llvmArgVal = callinst.first->getArgOperand(i)->stripPointerCasts();
                    auto argVal = LLVMModuleSet::getLLVMModuleSet()->getSVFValue(llvmArgVal);
                    if( _pag->hasValueNode(argVal)
                        && _pag->hasValueNode(getSVFValue(callee->getArg(i)))
                    ){
                        const auto real = _pag->getValueNode(argVal);
                        const auto formal = _pag->getValueNode(getSVFValue(callee->getArg(i)));
                        Real2Formal[real].insert(formal);
                        Formal2Real[formal].insert(real);
                        for(auto &bb : *callee){
                            for(auto &inst : bb){
                                if(auto retinst = dyn_cast<ReturnInst>(&inst)){
                                    if(retinst->getNumOperands() != 0 && callee->getReturnType()->isPointerTy()){
                                        const auto retval = _pag->getValueNode(getSVFValue(retinst->getReturnValue()));
                                        Ret2Call[retval].insert(_pag->getValueNode(getSVFValue(callinst.first)));
                                        Call2Ret[_pag->getValueNode(getSVFValue(callinst.first))].insert(retval);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    errs() << "Real2Formal " << Real2Formal.size() << "\n";
    errs() << "Formal2Real " << Formal2Real.size() << "\n";
    errs() << "Ret2Call " << Ret2Call.size() << "\n";
    errs() << "Call2Ret " << Call2Ret.size() << "\n";
    ifCallGraphSet = true;
}


bool checkTwoTypes(Type* src, Type* dst, unordered_map<const Type*, unordered_set<const Type*>> &castmap){
    if(src == dst){
        return true;
    }else if (src != nullptr && dst != nullptr
        && castmap[src].find(dst) != castmap[src].end()
    ){
        return true;
    }
    return false;
}

unordered_map<const Type*, unordered_set<const Type*>> castmap;

// [initialize]
void processCastMap(SVFIR* pag){
    for(auto edge : pag->getSVFStmtSet(PAGEdge::Copy)){
        if(auto srcType = edge->getSrcNode()->getType()){
            if(auto dstType = edge->getDstNode()->getType()){
                if(srcType && dstType && srcType != dstType){
                    castmap[getLLVMType(srcType)].emplace(getLLVMType(dstType));
                    castmap[getLLVMType(dstType)].emplace(getLLVMType(srcType));
                }
            }
        }
    }
    errs() << "[initialize] Finish processCastMap!\n";
}
    

bool checkIfMatch(const CallInst* callinst, const Function* callee){
    bool typeMatch = true;
    if(auto icallType = (callinst->getCalledOperand()->getType())){
        if(checkTwoTypes(callinst->getCalledOperand()->getType(), callee->getType(), castmap)){

        }else if(callinst->arg_size() != callee->arg_size()){
            // errs() << "here1" << "\n";
            typeMatch = false;
        }else if(checkTwoTypes(callinst->getType(), callee->getReturnType(), castmap)){
            for(auto i = 0; i < callinst->arg_size(); i++){
                auto icallargType = callinst->getArgOperand(i)->getType();
                auto calleeargType = callee->getArg(i)->getType();
                if(!checkTwoTypes(icallargType, calleeargType, castmap)){
                    if(icallargType->isPointerTy() && calleeargType->isPointerTy()){
                        auto icallpto = icallargType->getPointerElementType();
                        auto calleepto = calleeargType->getPointerElementType();
                        if((icallpto->isStructTy() && !calleepto->isStructTy())
                            || (!icallpto->isStructTy() && calleepto->isStructTy())
                        ){
                            typeMatch = false;
                            break;
                        }
                    }
                }
            }
        }else if(callinst->getType()->isPointerTy() && callee->getReturnType()->isPointerTy()){
            auto icallpto = callinst->getType()->getPointerElementType();
            auto calleepto = callee->getReturnType()->getPointerElementType();
            if((icallpto->isStructTy() && !calleepto->isStructTy())
                || (!icallpto->isStructTy() && calleepto->isStructTy())
            ){
                typeMatch = false;
            }
        }
    }
    return typeMatch;
}

void processArguments(int argc, char **argv, int &arg_num, char **arg_value,
                                std::vector<std::string> &moduleNameVec)
{
    for (int i = 0; i < argc; ++i)
    {
        string argument(argv[i]);
        if(argument.find("@") == 0){
            bool first_ir_file = true;
            ifstream fin(argument.substr(1));
            string tmp;
            fin >> tmp;
            while(!fin.eof()){
                if (LLVMUtil::isIRFile(tmp)){
                    if (find(moduleNameVec.begin(), moduleNameVec.end(), tmp) == moduleNameVec.end()){
                        moduleNameVec.push_back(tmp);
                    }
                    if(first_ir_file){
                        arg_value[arg_num] = argv[i];
                        arg_num++;
                        first_ir_file = false;
                    }
                }
                fin >> tmp;
            }
        }else
        {
            if (LLVMUtil::isIRFile(argument)){
                if (find(moduleNameVec.begin(), moduleNameVec.end(), argument) == moduleNameVec.end()){
                    moduleNameVec.push_back(argument);
                }
            }
            arg_value[arg_num] = argv[i];
            arg_num++;
        }
    }
}

// 判断一个变量节点是否可保护，即在init外有读写。
bool checkIfProtectable(PAGNode* pagnode){
    for(auto edge : pagnode->getIncomingEdges(PAGEdge::Store)){
        if(auto inst = dyn_cast<Instruction>(getLLVMValue(edge->getValue()))){
            if(auto func = inst->getFunction()){
                if(func->getSection().str() != ".init.text"
                 && func->getSection().str() != ".exit.text"
                 && NewInitFuncstr.find(func->getName().str()) == NewInitFuncstr.end()){
                    return false;
                }
            }
        }
    }
    return true;
}

bool pairCompare(const std::pair<s64_t, std::string>& a, const std::pair<s64_t, std::string>& b) {
    if (a.first == b.first) {
        return a.second < b.second;  // 如果值相同，"A" 小于 "B"
        // A是splitters，B是AliasesKeys。
    }
    return a.first < b.first;  // 否则按值从小到大排序
}

