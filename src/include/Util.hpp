#ifndef UNIAS_UTIL_H
#define UNIAS_UTIL_H

// #define DEBUG_PRINT_CHAIN
// #define DEBUG_HIGH_FREQ
// #define ALLYES

#ifdef ALLYES
#define BASE_NUM 50
#define O_BASE 50
#define SC_THRESHOLD 2400
#else
#define BASE_NUM 20
#define O_BASE 20
#define SC_THRESHOLD 300 // 这是为特定structType的特定offset成员创建shortcuts的阈值。超过了就不创建了。
#endif

#define STAT_THRESHOLD 500000


#include <string>
#include <fstream>
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMUtil.h"

using namespace SVF;
using namespace llvm;
using namespace std;

extern llvm::cl::opt<std::string> SpecifyInput;

extern unordered_set<string> NewInitFuncstr;

extern unordered_set<string> blackCalls;
extern unordered_set<string> blackRets;

extern unordered_set<NodeID> blackNodes;

extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiIn;
extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiOut;

extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectIn;
extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectOut;

extern unordered_set<NodeID> traceNodes;
extern unordered_set<const CallInst*> traceiCalls;

extern unordered_set<const Function*> SELinuxfuncs;
extern unordered_set<NodeID> SELinuxNodes;
extern unordered_set<const CallInst*> SELinuxicalls;

extern unordered_map<const Type*, unordered_set<Function*>> type2funcs;

extern unordered_map<string, unordered_map<u32_t, unordered_set<PAGEdge*>>> typebasedShortcuts;
extern unordered_map<string, unordered_map<u32_t, unordered_set<unordered_set<PAGEdge*>*>>> additionalShortcuts;
extern unordered_map<string, unordered_set<PAGEdge*>> castSites;
extern unordered_map<PAGEdge*, unordered_map<u32_t, unordered_set<string>>> reverseShortcuts;
extern unordered_map<PAGNode*, PAGEdge*> gepIn;

extern unordered_map<const PAGEdge*, long> gep2byteoffset;
extern unordered_set<const PAGEdge*> variantGep;

extern unordered_map<Value*, Module*> value2Module;

extern unordered_map<StructType*, string> deAnonymousStructs;

extern unordered_map<const CallInst*, unordered_set<Function*>> callgraph;
extern unordered_map<NodeID, unordered_set<NodeID>> Real2Formal;
extern unordered_map<NodeID, unordered_set<NodeID>> Formal2Real;
extern unordered_map<NodeID, unordered_set<NodeID>> Ret2Call;
extern unordered_map<NodeID, unordered_set<NodeID>> Call2Ret;
extern bool ifCallGraphSet;

void sortMap(std::vector<pair<PAGNode*, u64_t>> &sorted, unordered_map<PAGNode*, u64_t> &before, int k);

void getNewInitFuncs();

void getBlackNodes(SVFIR* pag);

void setupPhiEdges(SVFIR* pag);

void setupSelectEdges(SVFIR* pag);

string printVal(const Value* val);

string printType(const Type* val);

string getStructName(StructType* sttype);

bool checkIfAddrTaken(SVFIR* pag, PAGNode* node);

void addSVFAddrFuncs(SVFModule* svfModule, SVFIR* pag);

StructType* ifPointToStruct(const Type* tp);

void handleAnonymousStruct(SVFModule* svfModule, SVFIR* pag);

long varStructVisit(GEPOperator* gepop, DataLayout* DL);

long regularStructVisit(StructType* sttype, s32_t idx, PAGEdge* gep, DataLayout* DL);

void getSrcNodes(PAGNode* node, unordered_set<PAGNode*> &visitedNodes);

void setupStores(SVFIR* pag);

StructType* gotStructSrc(PAGNode* node, unordered_set<PAGNode*> &visitedNodes);

void collectByteoffset(SVFIR* pag);

void processCastSites(SVFIR* pag);

void readCallGraph(string filename, SVFModule* mod, SVFIR* pag);


void setupCallGraph(SVFIR* _pag);


bool checkTwoTypes(Type* src, Type* dst, unordered_map<const Type*, unordered_set<const Type*>> &castmap);


void processCastMap(SVFIR* pag);
    

bool checkIfMatch(const CallInst* callinst, const Function* callee);

void processArguments(int argc, char **argv, int &arg_num, char **arg_value,
                                std::vector<std::string> &moduleNameVec);

unsigned getStructOriginalElemNum(const StructType *stType);

int64_t getGepIndexValue(const GetElementPtrInst* gepInst, unsigned i);

Module* getModuleFromValue(Value *val);

u64_t getTypeSize(DataLayout* DL, const Type* type);
u64_t getTypeSize(DataLayout* DL, const StructType *sty, u32_t field_idx);
u64_t getFieldOffset(DataLayout* DL, const StructType *sty, u32_t field_idx);

string printTypeWithSize(Type* type, const DataLayout& DL);

void printGVType(SVFIR* pag, GlobalVariable* gv);

bool checkIfProtectable(PAGNode* pagnode);

bool pairCompare(const std::pair<s64_t, std::string>& a, const std::pair<s64_t, std::string>& b);

#endif