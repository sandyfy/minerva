#include <glog/logging.h>
#include <gflags/gflags.h>
#include <cstdlib>
//#include <fstream>

#include "minerva_system.h"
#include "op/impl/basic.h"
#include "procedures/impl_decider.h"

using namespace std;

/////////////////////// flag definitions //////////////////////
static bool IsValidImplType(const char* flag, const std::string& value) {
  return strcmp(flag, "basic") || strcmp(flag, "mkl") || strcmp(flag, "cuda");
}
DEFINE_string(impl, "basic", "use basic|mkl|cuda kernels");
static const bool impl_valid = gflags::RegisterFlagValidator(&FLAGS_impl, &IsValidImplType);

/////////////////////// member function definitions //////////////////////
namespace minerva {

void MinervaSystem::Initialize(int* argc, char*** argv) {
  google::InitGoogleLogging((*argv)[0]);
  gflags::ParseCommandLineFlags(argc, argv, true);
  LoadBuiltinDagMonitors();
  static SimpleImplDecider all_basic_impl(ImplType::kBasic);
  static SimpleImplDecider all_mkl_impl(ImplType::kMkl);
  static SimpleImplDecider all_cuda_impl(ImplType::kCuda);
  if (FLAGS_impl == "mkl") {
    impl_decider_ = &all_mkl_impl;
  } else if (FLAGS_impl == "cuda") {
    impl_decider_ = &all_cuda_impl;
  } else {
    impl_decider_ = &all_basic_impl;
  }
}
void MinervaSystem::Finalize() {
}

MinervaSystem::MinervaSystem(): impl_decider_(NULL) {
}

void MinervaSystem::LoadBuiltinDagMonitors() {
  logical_dag_.RegisterMonitor(&lnode_states_);
  logical_dag_.RegisterMonitor(&expand_engine_);
  physical_dag_.RegisterMonitor(&pnode_states_);
  physical_dag_.RegisterMonitor(&physical_engine_);
}
  
void MinervaSystem::SetImplDecider(PhysicalDagProcedure* decider) {
  impl_decider_ = decider;
}

void MinervaSystem::Eval(NArray& narr) {
  LOG(INFO) << "Evaluation start...";
  // logical dag
  expand_engine_.GCNodes(logical_dag_, lnode_states_);// GC useless logical nodes
  std::vector<uint64_t> id_to_eval = {narr.data_node_->node_id()};
  expand_engine_.Process(logical_dag_, lnode_states_, id_to_eval);

  // physical dag
  auto physical_nodes = expand_engine_.GetPhysicalNodes(narr.data_node_->node_id());
  // 1. decide impl type
  impl_decider_->Process(physical_dag_, pnode_states_, physical_nodes.ToVector());
  // 2. gc useless physical nodes
  physical_engine_.GCNodes(physical_dag_, pnode_states_);// GC useless physical nodes
  // 3. do computation
  physical_engine_.Process(physical_dag_, pnode_states_, physical_nodes.ToVector());

  LOG(INFO) << "Evaluation completed!";
}

float* MinervaSystem::GetValue(NArray& narr) {
  NVector<uint64_t> phy_nid = expand_engine_.GetPhysicalNodes(narr.data_node_->node_id());
  float* rstptr = new float[narr.Size().Prod()];
  Scale srcstart = Scale::Origin(narr.Size().NumDims());
  for(uint64_t nid : phy_nid) {
    PhysicalData& pdata = physical_dag_.GetDataNode(nid)->data_;
    float* srcptr = data_store_.GetData(pdata.data_id, DataStore::CPU);
    basic::NCopy(srcptr, pdata.size, srcstart,
        rstptr, narr.Size(), pdata.offset, pdata.size);
  }
  return rstptr;
}

void MinervaSystem::IncrExternRC(LogicalDag::DNode* dnode, int amount) {
  dnode->data_.extern_rc += amount;
  uint64_t ldnode_id = dnode->node_id();
  if(expand_engine_.IsExpanded(ldnode_id)) {
    // this means the node's physical data has been created
    // incr the extern_rc of its physical_node, and change the rc in data_store
    for(uint64_t pnode_id : expand_engine_.GetPhysicalNodes(ldnode_id)) {
      PhysicalDataNode* pnode = physical_dag_.GetDataNode(pnode_id);
      pnode->data_.extern_rc += amount;
    }
  }
}
  
} // end of namespace minerva