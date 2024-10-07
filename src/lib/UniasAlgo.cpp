#include "../include/UniasAlgo.hpp"

bool UniasAlgo::ifValidForTypebasedShortcut(PAGEdge* edge, u32_t threshold){
    if(edge->getSrcNode()->getType()){
        if(auto sttype = ifPointToStruct(edge->getSrcNode()->getType())){
            auto offset = gep2byteoffset[edge];
            // 核心在于结构体类型和offset要能在typebasedShortcuts中匹配到。
            if(typebasedShortcuts.find(getStructName(sttype)) != typebasedShortcuts.end()
                && typebasedShortcuts[getStructName(sttype)].find(offset) != typebasedShortcuts[getStructName(sttype)].end()
                && typebasedShortcuts[getStructName(sttype)][offset].size() < threshold
            ){
                return true;
            }
        }
    }
    return false;
}

bool UniasAlgo::ifValidForCastSiteShortcut(PAGEdge* edge, u32_t threshold){
    if(edge->getSrcNode()->getType()){
        if(auto sttype = ifPointToStruct(edge->getSrcNode()->getType())){
            // 核心在于结构体类型要能在castSites中匹配到。
            if(castSites[getStructName(sttype)].size() < threshold){
                return true;
            }
        }
    }
    return false;
}

// Prop函数：本质上是对ComputeAlias的套壳，可管理工作队列、防止重复分析。
// 功能上，是对PAG中的下一个节点进行Alias query的操作。
// eg的一边是上一层ComputeAlias的cur节点，另一边是待分析的nxt节点。其用来防止重复计算（只有过程间分析时调用Prop的eg==nullptr）。
void UniasAlgo::Prop(PAGNode* nxt, PAGEdge* eg, bool state, PAGNode* icall){
    if(blackNodes.find(nxt->getId()) != blackNodes.end()){
        return;
    }
    if(visitedEdges.size() > 25){
        return;
    }
    if(eg && !visitedEdges.insert(eg).second){
        return;
    }
    if(icall && !visitedicalls.insert(icall).second){
        return;
    }
    ComputeAlias(nxt, state);
    if(icall){
        visitedicalls.erase(icall);
    }
    if(eg){
        visitedEdges.erase(eg);
    }
}

// ComputeAlias函数：Unias的核心算法，递归的写法相当于是深度优先遍历。
// state: true表示计算I-Alias关系和一些反向边，false表示计算flows-to关系的正向边。
void UniasAlgo::ComputeAlias(PAGNode* cur, bool state){
    // ComputeAlias调用次数统计与限制。
    nodeFreq[cur]++;
    counter++;
    if(counter > STAT_THRESHOLD){
        vector<pair<PAGNode*, u64_t>> nodeFreqSorted;
        sortMap(nodeFreqSorted, nodeFreq, 50);
        for(auto i = 0; i < 50 && i < nodeFreqSorted.size(); i++){
            blackNodes.insert(nodeFreqSorted[i].first->getId());
        }

        nodeFreq.clear();
        counter = 0;
    }
    
    // 1. 处理初始栈节点。
    // 2. 在分析过程中，分析栈里规约到只剩一个节点时，也需要记录一下Alias结果。
    // （此时没有offset要求，也即各个field都可能被记录aliases）
    if(AnalysisStack.size() == 1){
        Aliases[AnalysisStack.top().offset].insert(cur); // 只有这里会设置Aliases的结果。
    }
    // 处理正向Load边（规则1、4，后者边）。
    if(cur->hasOutgoingEdges(PAGEdge::Load)){
        for(auto edge : cur->getOutgoingEdges(PAGEdge::Load)){
            if(AnalysisStack.size() > 1){
                auto &topItem = AnalysisStack.top();
                // if(topItem.curFlow == false && (topItem.isVariant || topItem.offset == 0)){
                if(topItem.offset == 0){
                    AnalysisStack.pop(); // 这里pop()应该是指正向的store边和load边完成了规则1的配对。继续Prop分析下一个节点。
                    Prop(edge->getDstNode(), edge, false, nullptr);
                    AnalysisStack.push(topItem);
                }
            }
        }
    }
    
    // 处理反向Store边（规则2， 后者边）。
    if(AnalysisStack.size() > 1){
        auto &topItem = AnalysisStack.top();
        if(topItem.curFlow && topItem.offset == 0 && cur->hasIncomingEdges(PAGEdge::Store)){
            for(auto edge : cur->getIncomingEdges(PAGEdge::Store)){
                AnalysisStack.pop(); // 这里pop()应该是指反向的load边和store边完成了规则2的配对。继续Prop分析下一个节点。
                Prop(edge->getSrcNode(), edge, true, nullptr);
                AnalysisStack.push(topItem);
            }
        }
    }

    // 处理正向Assign边（各种Assign边的子类）。
    // 遇到这类边都是直接Prop（state均为false），不使用AnalysisStack。
    // if(AnalysisStack.size() > 1){
        if(cur->hasOutgoingEdges(PAGEdge::Copy)){
            for(auto edge : cur->getOutgoingEdges(PAGEdge::Copy)){
                Prop(edge->getDstNode(), edge, false, nullptr);
            }
        }
        
        if(selectOut.find(cur->getId()) != selectOut.end()){
            for(auto edge : selectOut[cur->getId()]){
                for(auto dst : edge.second){
                    Prop(pag->getGNode(dst), edge.first, false, nullptr);
                }
            }
        }
        if(phiOut.find(cur->getId()) != phiOut.end()){
            for(auto edge : phiOut[cur->getId()]){
                for(auto dst : edge.second){
                    Prop(pag->getGNode(dst), edge.first, false, nullptr);
                }
            }
        }
        if(Real2Formal.find(cur->getId()) != Real2Formal.end()){
            for(auto formal : Real2Formal[cur->getId()]){
                Prop(pag->getGNode(formal), nullptr, false, cur);
            }
        }
        if(cur->hasOutgoingEdges(PAGEdge::Call)){
            for(auto edge : cur->getOutgoingEdges(PAGEdge::Call)){
                const auto callee = SVFUtil::getCallee(dyn_cast<CallPE>(edge)->getCallInst()->getCallSite())->getName();
                if(blackCalls.find(callee) == blackCalls.end()
                    && callee.find("kmalloc") == string::npos
                    && callee.find("kzalloc") == string::npos
                    && callee.find("kcalloc") == string::npos
                ){
                    // 要求callee不在黑名单中，且不是kmalloc、kzalloc、kcalloc，才能执行Prop操作。
                    Prop(edge->getDstNode(), edge, false, nullptr);
                }
            }
        }   
        
        if(Ret2Call.find(cur->getId()) != Ret2Call.end()){
            for(auto callsite : Ret2Call[cur->getId()]){
                Prop(pag->getGNode(callsite), nullptr, false, pag->getGNode(callsite));
            }
        }
        if(cur->hasOutgoingEdges(PAGEdge::Ret)){
            for(auto edge : cur->getOutgoingEdges(PAGEdge::Ret)){
                const auto callee = SVFUtil::getCallee(dyn_cast<RetPE>(edge)->getCallInst()->getCallSite())->getName();
                if(blackRets.find(callee) == blackRets.end()
                    && callee.find("kmalloc") == string::npos
                    && callee.find("kzalloc") == string::npos
                    && callee.find("kcalloc") == string::npos
                ){
                    Prop(edge->getDstNode(), edge, false, nullptr);
                }
            }
        }
    // }
    
    // 处理反向Assign边。
    // 遇到这类边都是直接Prop（state均为true），不使用AnalysisStack。
    if(state){
        if(cur->hasIncomingEdges(PAGEdge::Copy)){
            for(auto edge : cur->getIncomingEdges(PAGEdge::Copy)){
                Prop(edge->getSrcNode(), edge, true, nullptr);
            }
        }
        if(selectIn.find(cur->getId()) != selectIn.end()){
            for(auto edge : selectIn[cur->getId()]){
                for(auto src : edge.second){
                    Prop(pag->getGNode(src), edge.first, true, nullptr);
                }
            }
        }
        if(phiIn.find(cur->getId()) != phiIn.end()){
            for(auto edge : phiIn[cur->getId()]){
                for(auto src : edge.second){
                    Prop(pag->getGNode(src), edge.first, true, nullptr);
                }
            }
        }
        if(Formal2Real.find(cur->getId()) != Formal2Real.end()){
            for(auto real : Formal2Real[cur->getId()]){
                Prop(pag->getGNode(real), nullptr, true, pag->getGNode(real));
            }
        }
        if(cur->hasIncomingEdges(PAGEdge::Call)){
            for(auto edge : cur->getIncomingEdges(PAGEdge::Call)){
                const auto callee = SVFUtil::getCallee(dyn_cast<CallPE>(edge)->getCallInst()->getCallSite())->getName();
                if(blackCalls.find(callee) == blackCalls.end()
                    && callee.find("kmalloc") == string::npos
                    && callee.find("kzalloc") == string::npos
                    && callee.find("kcalloc") == string::npos
                ){
                    Prop(edge->getSrcNode(), edge, true, nullptr);
                }
            }
        }
        if(Call2Ret.find(cur->getId()) != Call2Ret.end()){
            for(auto ret : Call2Ret[cur->getId()]){
                Prop(pag->getGNode(ret), nullptr, true, cur);
            }
        }
        if(cur->hasIncomingEdges(PAGEdge::Ret)){
            for(auto edge : cur->getIncomingEdges(PAGEdge::Ret)){
                const auto callee = SVFUtil::getCallee(dyn_cast<RetPE>(edge)->getCallInst()->getCallSite())->getName();
                if(blackRets.find(callee) == blackRets.end()
                    && callee.find("kmalloc") == string::npos
                    && callee.find("kzalloc") == string::npos
                    && callee.find("kcalloc") == string::npos
                ){
                    Prop(edge->getSrcNode(), edge, true, nullptr);
                }
            }
        }
    }

    // 处理正向Store边（规则1，前者边）。
    // if(AnalysisStack.size() > 1 && cur->hasOutgoingEdges(PAGEdge::Store)){
    if(cur->hasOutgoingEdges(PAGEdge::Store)){
        for(auto edge : cur->getOutgoingEdges(PAGEdge::Store)){
            auto dstNode = edge->getDstNode();
            PNwithOffset newTypeInfo(0, false);
            AnalysisStack.push(newTypeInfo);
            Prop(dstNode, edge, true, nullptr); // 将state设置为true，用来匹配反向边。
            AnalysisStack.pop(); // Prop之后都是要将栈复原的，相当于目前这条edge的分析已经结束了。
        }
    }
    

    // 处理反向Load边（规则2、4，前者边）。
    if(state && cur->hasIncomingEdges(PAGEdge::Load)){
        for(auto edge : cur->getIncomingEdges(PAGEdge::Load)){
            auto srcNode = edge->getSrcNode();
            PNwithOffset newTypeInfo(0, true); // 这里将curFlow设为true
            AnalysisStack.push(newTypeInfo);
            Prop(srcNode, edge, true, nullptr);
            AnalysisStack.pop();
        }
    }

    // void collectByteoffset(SVFIR* pag)：负责variantGep、gep2byteoffset的初始化。

    // 处理反向Gep边及Shortcuts。
    if(state && cur->hasIncomingEdges(PAGEdge::Gep)){
        for(auto edge : cur->getIncomingEdges(PAGEdge::Gep)){
            assert(!AnalysisStack.empty());
            // if(!AnalysisStack.empty()){
            auto &topItem = AnalysisStack.top();
            if(variantGep.find(edge) != variantGep.end()){ // 如果是variantGep，回退到field不敏感的分析。
                Prop(edge->getSrcNode(), edge, true, nullptr);
            }else if(gep2byteoffset.find(edge) != gep2byteoffset.end()){ // constantGEP且能根据GEP边获取字节数偏移。
                // Consider taking shortcut?
                bool castShortcutTaken = false;
                const auto offset = gep2byteoffset[edge]; // 获取当前GEP边的offset字节数（这是初始化时计算的）。
                if(!taken && ifValidForTypebasedShortcut(edge, SC_THRESHOLD * 5)){ // 如果判断为可以做shortcuts，进入if body。
                    taken = true;
                    unordered_set<PAGNode*> visitedShortcuts;
                    auto sttype = ifPointToStruct(edge->getSrcNode()->getType()); // TODO: 理论上应该检查下nullptr。
                    const auto stname = getStructName(sttype);
                    // 处理Field-to-Field Shortcuts，并进行Prop。
                    if(typebasedShortcuts.find(stname) != typebasedShortcuts.end()
                        && typebasedShortcuts[stname].find(offset) != typebasedShortcuts[stname].end()
                    ){
                        for(auto dstShort : typebasedShortcuts[stname][offset]){
                            Prop(dstShort->getDstNode(), dstShort, false, nullptr);
                            visitedShortcuts.insert(dstShort->getDstNode());
                        }
                    }
                    // 处理Additional Shortcuts，并进行Prop。（Unias论文里似乎没提到这个）
                    if(additionalShortcuts.find(stname) != additionalShortcuts.end()
                        && additionalShortcuts[stname].find(offset) != additionalShortcuts[stname].end()
                    ){
                        for(auto dstSet : additionalShortcuts[stname][offset]){
                            for(auto dstShort : *dstSet){
                                if(visitedShortcuts.insert(dstShort->getDstNode()).second){
                                    Prop(dstShort->getDstNode(), dstShort, false, nullptr);
                                }
                            }
                        }
                    }
                    // 处理Field-to-CastSite Shortcuts。
                    if(ifValidForCastSiteShortcut(edge, SC_THRESHOLD)){
                        if(castSites.find(stname) != castSites.end()){
                            // 遍历所有符合类型的CastSites。每个dstCast都是一个Cast类型的PAGEdge*。
                            for(auto dstCast : castSites[stname]){
                                bool needVisitDst = false;
                                bool needVisitSrc = false;
                                // 检查Cast边的Src节点的类型信息。
                                if(auto castSrcTy = dstCast->getSrcNode()->getType()){
                                    if(auto castSrcSt = ifPointToStruct(castSrcTy)){
                                        if(getStructName(castSrcSt) == stname){
                                            needVisitDst = true;
                                        }else{
                                            needVisitSrc = true;
                                        }
                                    }else{
                                        needVisitSrc = true;
                                    }
                                }else{
                                    needVisitSrc = true;
                                }
                                // 检查Cast边的Dst节点的类型信息。
                                if(auto castDstTy  = dstCast->getDstNode()->getType()){
                                    if(auto castDstSt = ifPointToStruct(castDstTy)){
                                        if(getStructName(castDstSt) == getStructName(sttype)){
                                            needVisitSrc = true;
                                        }else{
                                            needVisitDst = true;
                                        }
                                    }else{
                                        needVisitDst = true;
                                    }
                                }else{
                                    needVisitDst = true;
                                }
                                // 判断应该在Cast边的Src端还是Dest端进行Prop。
                                // 注意：前面两处走shortcut时都没有对topItem.offset进行修改，这里略有不同。
                                // 解释：需要先减去offset，是因为走Cast的shortcut过去后还要再匹配一条正向GEP边。
                                if(needVisitSrc){
                                    topItem.offset -= offset;
                                    Prop(dstCast->getSrcNode(), dstCast, true, nullptr);
                                    topItem.offset += offset;
                                }
                                if(needVisitDst){
                                    topItem.offset -= offset;
                                    Prop(dstCast->getDstNode(), dstCast, false, nullptr);
                                    topItem.offset += offset;
                                }
                            }
                        }
                        castShortcutTaken = true; // 表示CastSite类型的shortcut是可以处理的。
                    }
                    taken = false; // shortcuts后面的节点递归分析结束，恢复该变量状态。
                }
                // 这里相当于是不走shortcut，进行基础数据流分析，直接在当前GEP反向边的Src节点进行Prop。
                // TODO：这样设计其实是不sound的，会忽略PAG本地附近区域的alias及读写情况。
                if(!castShortcutTaken){ // 如果在处理当前GEP反向边时，没有做CastSite类型的shortcut，才能进入if。（按论文mutually exclusive的设计）
                    topItem.offset -= offset;
                    Prop(edge->getSrcNode(), edge, true, nullptr);
                    topItem.offset += offset;
                }
            }
                
            // }
        }
    }

    // 处理正向Gep边。
    if(cur->hasOutgoingEdges(PAGEdge::Gep)){
        for(auto edge : cur->getOutgoingEdges(PAGEdge::Gep)){
            auto &topItem = AnalysisStack.top();
            if(variantGep.find(edge) != variantGep.end()){ // 如果是variantGep，回退到field不敏感的分析。
                // topItem.isVariant = true;
                Prop(edge->getDstNode(), edge, true, nullptr);
                // topItem.isVariant = false;
            }else if(gep2byteoffset.find(edge) != gep2byteoffset.end()){ // 根据GEP边获取字节数offset（而不是index偏移）。
                topItem.offset += gep2byteoffset[edge];
                // topItem.geptimes++;
                // if(topItem.geptimes < 4)
                    Prop(edge->getDstNode(), edge, true, nullptr);
                // topItem.geptimes--;
                topItem.offset -= gep2byteoffset[edge]; // 栈顶元素offset恢复原状。
            }
        }
    }
    
}

// [Added by LHY]
// 该函数用于对Aliases结果进行处理，把byteOffset转化回结构体的OriginalElem的index，使输出结果更可读。
void UniasAlgo::postProcessGV() {

}