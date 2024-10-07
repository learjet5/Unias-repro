#include "SVF-LLVM//SVFIRBuilder.h"
#include "SVF-LLVM/BasicTypes.h"
#include "WPA/Andersen.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/resource.h>
#include <thread>

#include "include/UniasAlgo.hpp"
#include "include/Util.hpp"

using namespace llvm;
using namespace SVF;

// Command line parameters.
cl::list<std::string> InputFilenames(
    cl::Positional, cl::OneOrMore, cl::desc("<input bitcode files>"));

cl::opt<unsigned> VerboseLevel(
    "verbose-level", cl::desc("Print information at which verbose level"),
    cl::init(0));

static llvm::cl::opt<std::string> OutputDir("OutputDir",
    llvm::cl::desc("OutputDir"), llvm::cl::init(""));

static llvm::cl::opt<std::string> CallGraphPath("CallGraphPath",
    llvm::cl::desc("CallGraphPath"), llvm::cl::init(""));

static llvm::cl::opt<size_t> ThreadNum("ThreadNum",
    llvm::cl::desc("ThreadNum"), llvm::cl::init(1));

static llvm::cl::opt<std::string> SpecificGV("SpecificGV",
    llvm::cl::desc("Specify the gv"), llvm::cl::init(""));

unordered_set<GlobalVariable*> analysiscope;

UniasAlgo* performAnalysis(Value* gv, SVFIR* pag){
    // 每分析一个GV，就构建一个UniasAlgo实例。
    auto* unias = new UniasAlgo();
    unias->pag = pag;
    auto curLayout = dyn_cast<GlobalVariable>(gv)->getParent()->getDataLayout();
    unias->DL = &curLayout;
    for(auto node : blackNodes){
        unias->blackNodes.insert(node);
    }
    PNwithOffset firstLayer(0 ,false);
    unias->AnalysisStack.push(firstLayer); // 分析栈中初始节点(os=0, cf=false)。
    // find the variable you want to query on the graph
    auto pgnode = pag->getGNode(pag->getValueNode(gv));
    unias->taskNode = pgnode;
    unias->ComputeAlias(pgnode, false);
    // Production `flows-to` is false, `I-Alias` is true
    // For global variables, we use `flows-to`
    // Aliases will be unias.Aliases, it's a field sensitive map
    // where Aliases[0] shows the aliases of field indice 0
    return unias;
}


unordered_map<const Value*, CallInst*> calledmap;


// Alias分析结果后处理。（Added by LHY）
// 遍历Unias别名分析结果，记录Protect/Written属性。
map<s64_t, string> getGvWrittenInfo(UniasAlgo* unias) {
    map<s64_t, string> result;
    for (const auto& pair : unias->Aliases) {// 遍历每个field。
        s64_t byteOffset = pair.first;
        const std::unordered_set<PAGNode*>& nodeSet = pair.second;
        // 依次检验当前field的所有alias节点是否可保护。
        bool ifProtectable = true;
        for (const auto& node : nodeSet) {
            if(!checkIfProtectable(node)) {
                ifProtectable = false;
                break;
            }
        }
        if (ifProtectable) {
            result.emplace(byteOffset, "Protect");
        } else {
            result.emplace(byteOffset, "Written");
        }
        // fout << "Offset: " << byteOffset << (ifProtectable ? " [Protect] ":" [Written] ") << ";\tAliasNum: " << nodeSet.size() << endl;
    }
    return result;
} 
void postProcessResults(UniasAlgo* unias, GlobalVariable* gv, ofstream &fout) {
    string output = "";
    // 对于标量GV，直接输出Aliases结果。
    // 对于标量数组GV，直接输出Aliases结果。
    // 对于结构体GV，按index输出各个field的可保护情况。（只考虑Original Fields）
    // 对于结构体数组GV，先无视数组元素索引，按普通结构体处理。
    map<s64_t, string> uniasRes = getGvWrittenInfo(unias); // byteOffset -> Protect/Written
    set<s64_t> protectableOffsets;
    for (const auto& pair : uniasRes) {
        if(pair.second == "Protect") protectableOffsets.insert(pair.first);
    }
    // 整理GV的整体类型信息。
    DataLayout curLayout = gv->getParent()->getDataLayout();
    auto gvType = gv->getType();
    output += ("GV Name: " + gv->getName().str() + "\n");
    auto elemType = gvType->getPointerElementType(); // 先去除LLVM IR给GV加的那层“指针”类型。
    output += ("GV Type: " + printType(elemType) + " (Stripped outer layer)\n");
    bool isGvArray = false;
    while(elemType && elemType->isArrayTy()){ // 去除“数组”类型的包裹。
        elemType = elemType->getArrayElementType();
        isGvArray = true;
    }
    // 整理GV的基础类型信息。
    s64_t gvAllSize = getTypeSize(&curLayout, gvType->getPointerElementType());
    if(elemType->isStructTy()) {
        StructType* stType = dyn_cast<StructType>(elemType);
        auto stLayout = curLayout.getStructLayout(stType);
        auto stOffsets = stLayout->getMemberOffsets();
        output += ("Elem Struct Fields Num: " + std::to_string(stOffsets.size()) + "\n");
        // 当GV涉及结构体时，根据stOffsets和gvAllSize对Aliases结果进行划分，便于阅读。
        vector<s64_t> splitters;
        for(auto i : stOffsets) {
            splitters.push_back(i);
        }
        splitters.push_back(gvAllSize); 
        vector<s64_t> AliasesKeys;
        for (const auto& pair : unias->Aliases) { AliasesKeys.push_back(pair.first); }
        // 把splitters和AliasesKeys都在一个vector里排列并记录来源。
        std::vector<std::pair<s64_t, std::string>> combinedSeq;
        for (s64_t a : splitters) {
            combinedSeq.push_back({a, "A"});
        }
        for (s64_t b : AliasesKeys) {
            combinedSeq.push_back({b, "B"});
        }
        std::sort(combinedSeq.begin(), combinedSeq.end(), pairCompare);
        for (auto i = 0; i < combinedSeq.size(); i++) {
            auto offset = combinedSeq[i].first;
            auto tag = combinedSeq[i].second;
            if (tag=="A") {
                // 获取当前offset对应的结构体field index。
                auto it = std::find(splitters.begin(), splitters.end(), offset);
                int idx = 0;
                if (it != splitters.end()) {
                    idx = std::distance(splitters.begin(), it);
                } else {
                    errs() << "This should never be reached!\n";
                }
                // 
                if(idx == splitters.size()-1) {
                    output += ("Struct Boundary Size: " + to_string(gvAllSize) + "\n");
                } else {
                    // output += ("Idx "+ to_string(idx) + ":\n"); //shorter
                    output += ("Idx: "+ to_string(idx) + "; " + "Offset: " + to_string(offset) + "\n");
                } 
            } else if (tag=="B") {
                output += ("\tbyteOffset: " + std::to_string(offset) + " ["+uniasRes[offset]+"]");
                output += (";\tAliasNum: " + std::to_string(unias->Aliases[offset].size()) + "\n");
            }
        }
        // TODO: 按结构体的original index列可保护比例。
        output += "Protectable Ratio: " + std::to_string(protectableOffsets.size()) + "/" + std::to_string(unias->Aliases.size()) + " (TBD)\n";
    } 
    else if(elemType->isSingleValueType()) {
        for (const auto& pair : uniasRes) {
            s64_t byteOffset = pair.first;
            auto  tag = pair.second;
            output += ("\tbyteOffset: " + std::to_string(byteOffset) + " ["+tag+"]");
            output += (";\tAliasNum: " + std::to_string(unias->Aliases[byteOffset].size()) + "\n");
        }
        output += "Protectable Ratio: " + std::to_string(protectableOffsets.size()) + "/" + std::to_string(unias->Aliases.size()) + "\n";
    } 
    else {
        // errs() << "Special GV element type: " << printType(gvType) << "\n";
        output += ("Special GV element type: " + printType(gvType) + "\n");
    }    
    fout << output << endl;
    fout << endl << flush;
    fout.flush();
}
void postProcessResults_old(UniasAlgo* unias, GlobalVariable* gv, ofstream &fout) { // 老版输出
    // (old)输出格式为：全局变量名 + 
    // 可保护的filed占所有Aliases里fileds的比例 +
    // 各个可保护的field的byteOffset值。
    unsigned allFieldNum = unias->Aliases.size();
    set<s64_t> protectableOffsets;
    for (const auto& pair : unias->Aliases) {
        s64_t offset = pair.first;
        const std::unordered_set<PAGNode*>& nodeSet = pair.second;
        // 依次检验当前field的所有alias节点是否可保护。
        bool ifProtectable = true;
        for (const auto& node : nodeSet) {
            if(!checkIfProtectable(node)) {
                ifProtectable = false;
                break;
            }
        }
        if (ifProtectable) {
            protectableOffsets.insert(offset);
        }
    }
    string output = gv->getName().str() + "\n";
    output += std::to_string(protectableOffsets.size()) + "/" + std::to_string(allFieldNum) + "\n";
    for(auto s : protectableOffsets) {
        output += std::to_string(s) + "\n";
    }
    fout << output << endl;
    fout << endl << flush;
    fout.flush();
}

void eachThread(SVFIR* pag, GlobalVariable* gv, ofstream &fout){
    if (ThreadNum == 1) printGVType(pag, gv); // For debug. // 但多线程同时往errs()里写东西可能有问题。
    
    auto res = performAnalysis(gv, pag);
    // llvm::DataLayout curLayout(dyn_cast<GlobalVariable>(gv)->getParent());
    // res->DL = &curLayout; // 可能需要加，取决于DataLayout对象的生命周期。
    // postProcessResults(res, gv, fout);
    postProcessResults_old(res, gv, fout);
    delete res;

    // // Guoren的KallGraph相关代码。
    // set<string> targetcis;
    // // res->Aliases从field byteoffset映射到PAGNode*的set
    // for(auto alias : res->Aliases[0]){
    //     // 这里筛选的是指向函数的alias，也就是函数指针，但事实上我应该不需要这个筛选。
    //     if(alias->hasValue() && alias->getValue()->getType()->isPointerTy() && alias->getValue()->getType()->getPointerElementType()->isFunctionTy()){
    //         targetcis.insert(to_string(alias->getId()));// 把NodeID加入targetcis
    //     }
    // }
    // // 目前的输出格式为：gv名称 + 别名节点数量 + 函数指针alias的节点ID
    // string output = gv->getName().str() + "\n" + to_string(res->Aliases[0].size()) + "\n";
    // for(auto ci : targetcis){
    //     output += ci + "\n";
    // }
}

class ThreadPool {
public:
    ThreadPool(size_t threadCount, SVFIR* pag, std::queue<GlobalVariable*> &tasks) : stop(false), pag(pag), tasks(tasks) {
        for (size_t i = 0; i < threadCount; ++i) {
            workers.emplace_back([this] {
                static int t = 0;
                ofstream fout(OutputDir + "/" + to_string(t++));
                while (!stop) {
                    while(!queueMutex.try_lock()){
                        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 5));
                    }
                    
                    if(!this->tasks.empty()){
                        GlobalVariable* gv = this->tasks.front();
                        this->tasks.pop();
                        queueMutex.unlock();
                        eachThread(this->pag, gv, fout);
                    }else{
                        stop = true;
                        queueMutex.unlock();
                    }
                    
                }
                fout.close();
            });
        }
        
    }

    void WaitAll() {
        for(auto &worker : workers){
            worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<GlobalVariable*> &tasks;
    std::mutex queueMutex;
    bool stop;
    SVFIR* pag;
};

unordered_set<Function*>* getSpecifyInput(SVFModule* svfmod){
    if(SpecifyInput == ""){
        return nullptr;
    }
    unordered_set<string> gvs;
    string tmp;
    ifstream fin(SpecifyInput);
    while(!fin.eof()){
        fin >> tmp;
        gvs.insert(tmp);
    }
    auto ret = new unordered_set<Function*>();
    // for(auto ii = svfmod->global_begin(), ie = svfmod->global_end(); ii != ie; ii++){
    //     auto gv = *ii;
    //     if(gv->hasName() && gvs.find(gv->getName().str()) != gvs.end()){
    //         ret->insert(gv);
    //     }
    // }
    for(auto f : *svfmod){
        auto func = f->getLLVMFun();

        if(gvs.find(func->getName().str()) != gvs.end()){
            ret->insert(func);
        }
    }
    return ret;
}


void analysis_unias(SVFModule *M, SVFIR* pag, size_t threadcount){
    std::queue<GlobalVariable*> tasks;
    // if(auto specified = getSpecifyInput(M)){
    //     for(auto gv : *specified){
    //         tasks.push(gv);
    //     }
    // }
    for(auto gv : analysiscope){
        tasks.push(gv);
    }
    
    ThreadPool pool(threadcount, pag, tasks);
    pool.WaitAll();
}

// 这里面很多操作都值得分析。
void initialize(SVFIR* pag, SVFModule* svfModule){
    if(CallGraphPath != ""){
        readCallGraph(CallGraphPath, svfModule, pag);
        setupCallGraph(pag);
    }
    getBlackNodes(pag);
    setupPhiEdges(pag);
    setupSelectEdges(pag);
    handleAnonymousStruct(svfModule, pag);
    addSVFAddrFuncs(svfModule, pag);
    collectByteoffset(pag);
    setupStores(pag);
    processCastSites(pag);
    processCastMap(pag);
    errs() << "shortcuts setup in Unias! " << "\n\n";
}

unordered_map<u32_t, unordered_map<string, unordered_set<u32_t>>> sizeMaps;

void sortSizeMap(std::vector<pair<u32_t, unordered_map<string, unordered_set<u32_t>>>> &sorted, unordered_map<u32_t, unordered_map<string, unordered_set<u32_t>>> &before){
    sorted.reserve(before.size());
    for (const auto& kv : before) {
        sorted.emplace_back(kv.first, kv.second);
    }
    std::stable_sort(std::begin(sorted), std::end(sorted),
                    [](const pair<u32_t, unordered_map<string, unordered_set<u32_t>>>& a, const pair<u32_t, unordered_map<string, unordered_set<u32_t>>>& b) { return a.first > b.first; });
}


void setupICallValue(SVFModule* svfModule){
    for(auto func : *svfModule){
        for(auto &bb : *(func->getLLVMFun())){
            for(auto &inst : bb){
                if(auto ci = dyn_cast<CallInst>(&inst)){
                    if(ci->isIndirectCall()){
                        calledmap[ci->getCalledOperand()->stripPointerCasts()] = ci;
                    }
                }
            }
        }
    }
}

// 全量GV分析范围。
void getAnalysisScope(SVFModule* svfModule){
    // std::set<string> sectionNames;
    for(auto ii = svfModule->global_begin(), ie = svfModule->global_end(); ii != ie; ii++){
        auto gv = *ii;
        // sectionNames.insert(gv->getSection().str());
        if(!gv->isConstant() && !gv->hasSection()){
            // TODO: 还应筛选gv是writeable的。但guoren可能按System.map里的数据段也筛过了。
            analysiscope.insert(gv);
        }
    }
    errs() << "analysis scope size: " << analysiscope.size() << "\n";
}

// 直接从一个包含GV名称的文件中，获取分析范围。（Added by LHY）
void getExistingAnalysisScope(SVFModule* svfModule) {
    // For Linux-5.14, 12089 GVs should be analyzed.
    string InputScopename = "/mnt/sdc/lhy_tmp/spa/Uniasss/analyze/Linux-5.14-finalscope";
    ifstream fin(InputScopename);
    if (!fin) {
        errs() << "Fail to open " << InputScopename << "!\n";
        exit(0);
    }
    set<string> rawScope;
    string tmp;
    while(!fin.eof()){
        fin >> tmp;
        rawScope.insert(tmp);
    }
    errs() << "GV rawScope size: " << rawScope.size() << "\n";

    for(auto ii = svfModule->global_begin(), ie = svfModule->global_end(); ii != ie; ii++){
        auto gv = *ii;
        if(rawScope.find(gv->getName().str()) != rawScope.end()){
            analysiscope.insert(gv);
        }
    }
    errs() << "analysis scope size: " << analysiscope.size() << "\n";

    // 看下analysisScope里的GV有没有重名实体（实际上会有很多，比如key_type_asymmetric就有三个实体，不理解为什么）
    // TODO: 重名的gv，用gv->hasInitializer()筛选下。确保每个gvname只分析一个实体。
    set<string> recordedNames;
    for(auto gv : analysiscope){
        // gv->isDeclaration()
        // gv->isDefinitionExact()
        // gv->hasExactDefinition()
        string name = gv->getName().str();
        if(recordedNames.find(name) != recordedNames.end()) {
            errs() << "Find repeated object instance for GV name: " << name << "\n";
        } else {
            recordedNames.insert(name);
        }
    }
}

bool getSpecificGV(SVFModule* svfModule, string name) {
    for(auto ii = svfModule->global_begin(), ie = svfModule->global_end(); ii != ie; ii++){
        auto gv = *ii;
        if(!gv->isConstant() && !gv->hasSection()){
            if(gv->getName().str()==name) {
                analysiscope.insert(gv);
                return true;
            }
        }
    }
    return false;
}

int main(int argc, char **argv) {

	int arg_num = 0;
    char **arg_value = new char*[argc];
    std::vector<std::string> moduleNameVec;
    processArguments(argc, argv, arg_num, arg_value, moduleNameVec); // 自己实现的输入bc解析函数，初始化moduleNameVec。
    cl::ParseCommandLineOptions(arg_num, arg_value, "Whole Program Points-to Analysis\n");
    delete[] arg_value;
    errs() << "OutputDir: " << OutputDir <<"\n";
    errs() << "ThreadNum: " << ThreadNum <<"\n";
    errs() << "CallGraphPath: " << CallGraphPath <<"\n";

    auto inputModuleSet= LLVMModuleSet::getLLVMModuleSet();
    SVFModule* svfModule = inputModuleSet->buildSVFModule(moduleNameVec);
    
    svfModule->buildSymbolTableInfo();
    errs() << "SVF Module built!\n";

    /// Build Program Assignment Graph (SVFIR)
    SVFIRBuilder builder;
    SVFIR* pag = builder.build(svfModule);
    errs() << "pag built!\n";

    initialize(pag, svfModule);
    // return 0;

    // 获取分析范围
    // getAnalysisScope(svfModule);
    getExistingAnalysisScope(svfModule);
    getNewInitFuncs();
    
    // For single GV debug.
    // if(SpecificGV=="") {
    //     errs() << "No GV is specified!\n";
    //     return 1;
    // }
    // if(!getSpecificGV(svfModule, SpecificGV)){
    //     errs() << "No such GV is found!\n";
    //     return 1;
    // }

    analysis_unias(svfModule, pag, ThreadNum);
    errs() << "All Analysis finished!\n";

	return 0;
}