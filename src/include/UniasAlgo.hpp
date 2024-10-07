#ifndef ALGO_H
#define ALGO_H
#include <unordered_map>
#include <unordered_set>
#include "Util.hpp"
#include "llvm/IR/DataLayout.h"

using namespace SVF;
using namespace std;
using namespace llvm;

class PNwithOffset{
public:
    // const PAGNode* pagnode;
    s64_t offset; // 用于实现field-sensitive，记录成员偏移。
    bool curFlow; // 目前代码看下来，curFlow的唯一作用就是匹配规则2中的反向store边。
    PNwithOffset(s64_t os, bool cf) : offset(os), curFlow(cf){//构造函数。

    }
};

using typeStack = stack<PNwithOffset>;

// 每分析一个GV，就构建一个UniasAlgo实例。
class UniasAlgo{
public:
    unordered_set<PAGEdge*> visitedEdges; // 用于在Prop函数中进行traversal控制。
    typeStack AnalysisStack;      // 元素为PNwithOffset的Stack，初始包含一个(os=0, cf=false)的元素。维护当前分析状态。
                                  // 只有规则1、2、4中Load/Store边的处理逻辑会对分析栈pop/push，GEP边的处理逻辑则会对栈顶元素offset操作。
                                  // 栈中只有一个元素说明Load/Store边全都规约掉了，栈顶元素的offset为0说明GEP边全部规约掉了。
    map<s64_t, unordered_set<PAGNode*>> Aliases; // 记录当前GV的各个fields的别名节点集合。
    SVFIR *pag;
    bool taken = false; // 记录当前ComputeAlias的分析是否采用了TypebasedShortcut。当前分析layer的递归调用层是不能再采用shortcuts的。（shortcutTaken）
    PAGNode* taskNode;  // 当前分析的起始GV节点，初始设定一个GV之后不再修改。
    DataLayout* DL;     // 当前GV所在bitcode文件的layout，可用于计算type的大小。（Added by LHY）
    unordered_set<NodeID> blackNodes;
    unordered_set<PAGNode*> visitedicalls;
    unordered_map<PAGNode*, u64_t> nodeFreq; // 记录每个PAGNode被ComputeAlias访问的次数。
    int counter = 0;    // 记录分析当前GV的过程中，ComputeAlias调用的总次数。
    int breakpoint = 3;
    
    bool ifValidForTypebasedShortcut(PAGEdge* edge, u32_t threshold);

    bool ifValidForCastSiteShortcut(PAGEdge* edge, u32_t threshold);

    void Prop(PAGNode* nxt, PAGEdge* eg, bool state, PAGNode* icall);

    void ComputeAlias(PAGNode* cur, bool state);

    void postProcessGV();
};

#endif