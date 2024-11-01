#ifndef UNIAS_UTIL_H
#define UNIAS_UTIL_H

// #define DEBUG_PRINT_CHAIN
// #define DEBUG_HIGH_FREQ
// #define ALLYES

#include "SVF-LLVM/BasicTypes.h"
#include "SVFIR/SVFType.h"
#include "SVFIR/SVFValue.h"
#include <llvm/IR/Instruction.h>
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

// extern llvm::cl::opt<std::string> SpecifyInput;

extern unordered_set<string> NewInitFuncstr;

extern unordered_set<string> blackCalls;
extern unordered_set<string> blackRets;

extern unordered_set<NodeID> blackNodes;

extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiIn;
extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> phiOut;

extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectIn;
extern unordered_map<NodeID, unordered_map<SVFStmt*, unordered_set<NodeID>>> selectOut;

// extern unordered_map<const SVFType*, unordered_set<const SVFFunction*>> type2funcs;

extern unordered_map<string, unordered_map<u32_t, unordered_set<PAGEdge*>>> typebasedShortcuts;
extern unordered_map<string, unordered_map<u32_t, unordered_set<unordered_set<PAGEdge*>*>>> additionalShortcuts;
extern unordered_map<string, unordered_set<PAGEdge*>> castSites;
extern unordered_map<PAGEdge*, unordered_map<u32_t, unordered_set<string>>> reverseShortcuts;
extern unordered_map<PAGNode*, PAGEdge*> gepIn;

extern unordered_map<const PAGEdge*, long> gep2byteoffset;
extern unordered_set<const PAGEdge*> variantGep;

extern unordered_map<const Value*, const Module*> value2Module;

extern unordered_map<StructType*, string> deAnonymousStructs;

extern unordered_map<const CallInst*, unordered_set<const Function*>> callgraph;
extern unordered_map<NodeID, unordered_set<NodeID>> Real2Formal;
extern unordered_map<NodeID, unordered_set<NodeID>> Formal2Real;
extern unordered_map<NodeID, unordered_set<NodeID>> Ret2Call;
extern unordered_map<NodeID, unordered_set<NodeID>> Call2Ret;
extern bool ifCallGraphSet;

// 
// Unias initialization.
// 
void readCallGraph(string filename, SVFModule* mod, SVFIR* pag);

void setupCallGraph(SVFIR* _pag);

void getBlackNodes(SVFIR* pag);

void setupPhiEdges(SVFIR* pag);

void setupSelectEdges(SVFIR* pag);

void handleAnonymousStruct(SVFModule* svfModule, SVFIR* pag);

void collectByteoffset(SVFIR* pag);

void setupStores(SVFIR* pag);

void processCastSites(SVFIR* pag);

void processCastMap(SVFIR* pag);

// 
// Unias specific.
// 
void processArguments(int argc, char **argv, int &arg_num, char **arg_value,
                                std::vector<std::string> &moduleNameVec);

bool checkIfProtectable(PAGNode* pagnode);

// KallGraph related.
bool checkIfAddrTaken(SVFIR* pag, PAGNode* node);
void addSVFAddrFuncs(SVFModule* svfModule, SVFIR* pag);
bool checkIfMatch(const CallInst* callinst, const Function* callee);

// 
// Util functions.
// 
void sortMap(std::vector<pair<PAGNode*, u64_t>> &sorted, unordered_map<PAGNode*, u64_t> &before, int k);

bool pairCompare(const std::pair<s64_t, std::string>& a, const std::pair<s64_t, std::string>& b);

bool checkTwoTypes(Type* src, Type* dst, unordered_map<const Type*, unordered_set<const Type*>> &castmap);

long varStructVisit(GEPOperator* gepop, const DataLayout* DL);

long regularStructVisit(StructType* sttype, s64_t idx, PAGEdge* gep, const DataLayout* DL);

void getSrcNodes(PAGNode* node, unordered_set<PAGNode*> &visitedNodes);

string getStructName(StructType* sttype);

StructType* gotStructSrc(PAGNode* node, unordered_set<PAGNode*> &visitedNodes);

#endif