#include "tree/hybrid.h"

#include "common/macro.h"
#include "common/logger.h"
#include "evaluator/recorder.h"
#include "sort/sorter.h"
#include "transformer/transformer.h"

#include <cassert>
#include <queue>
#include <thread>
#include <algorithm>

namespace ursus {
namespace tree {

Hybrid::Hybrid() {
  tree_type = TREE_TYPE_HYBRID;
}

/**
 * @brief build trees on the GPU
 * @param input_data_set 
 * @return true if success to build otherwise false
 */
bool Hybrid::Build(std::shared_ptr<io::DataSet> input_data_set){
  LOG_INFO("Build Hybrid Tree");
  bool ret = false;

  // Load an index from file it exists
  // otherwise, build an index and dump it to file
  auto index_name = GetIndexName(input_data_set);
  if(!DumpFromFile(index_name)) { 
    //===--------------------------------------------------------------------===//
    // Create branches
    //===--------------------------------------------------------------------===//
    std::vector<node::Branch> branches = CreateBranches(input_data_set);

    //===--------------------------------------------------------------------===//
    // Assign Hilbert Ids to branches
    //===--------------------------------------------------------------------===//
    ret = AssignHilbertIndexToBranches(branches);
    assert(ret);

    //===--------------------------------------------------------------------===//
    // Sort the branches either CPU or GPU depending on the size
    //===--------------------------------------------------------------------===//
    ret = sort::Sorter::Sort(branches);
    assert(ret);

    //===--------------------------------------------------------------------===//
    // Build the internal nodes in a top-down fashion on the GPU
    //===--------------------------------------------------------------------===//
    ret = Top_Down(branches); 
    assert(ret);

    //===--------------------------------------------------------------------===//
    // Transform nodes into SOA fashion 
    //===--------------------------------------------------------------------===//
    // transform only leaf nodes
    auto leaf_node_offset = total_node_count-leaf_node_count;
    node_soa_ptr = new node::Node_SOA[leaf_node_count];
    LOG_INFO("leaf node count %u", leaf_node_count);
    assert(node_soa_ptr);
    ret = CopyBranchToNodeSOA(branches, NODE_TYPE_LEAF, /*FIXME*/ -1, 0);

    // Dump internal and leaf nodes into a file
    DumpToFile(index_name);
  }

  //PrintTree(); 

 //===--------------------------------------------------------------------===//
 // Move Trees to the GPU
 //===--------------------------------------------------------------------===//
  // move only leaf nodes to the GPU
  // FIXME : use stream..
  ret = MoveTreeToGPU(0, leaf_node_count);
  assert(ret);

  return true;
}

bool Hybrid::DumpFromFile(std::string index_name) {

  FILE* index_file;
  index_file = fopen(index_name.c_str(),"rb");

  if(!index_file) {
    LOG_INFO("An index file(%s) doesn't exist", index_name.c_str());
    return false;
  }

  LOG_INFO("Load an index file (%s)", index_name.c_str());
  auto& recorder = evaluator::Recorder::GetInstance();
  recorder.TimeRecordStart();

  size_t height;

  // read tree height
  fread(&height, sizeof(size_t), 1, index_file);
  level_node_count.resize(height);
  for(ui range(level_itr, 0, height)) {
    // read node count for each tree level
    fread(&level_node_count[level_itr], sizeof(ui), 1, index_file);
  }
  // read total node count
  fread(&total_node_count, sizeof(ui), 1, index_file);

  // read leaf node count
  fread(&leaf_node_count, sizeof(ui), 1, index_file);

  // read the entire nodes
  node_ptr = new node::Node[total_node_count];
  fread(node_ptr, sizeof(node::Node), total_node_count, index_file);

  // read leaf nodes
  node_soa_ptr = new node::Node_SOA[leaf_node_count];
  fread(node_soa_ptr, sizeof(node::Node_SOA), leaf_node_count, index_file);

  fclose(index_file);

  auto elapsed_time = recorder.TimeRecordEnd();
  LOG_INFO("Done, time = %.6fs", elapsed_time/1000.0f);

  return true;
}

bool Hybrid::DumpToFile(std::string index_name) {
  auto& recorder = evaluator::Recorder::GetInstance();

  LOG_INFO("Dump an index into file (%s)...", index_name.c_str());

  recorder.TimeRecordStart();
  // NOTE :: Use fwrite since it is fast
  FILE* index_file;
  index_file = fopen(index_name.c_str(),"wb");

  size_t height = level_node_count.size();
  // write tree height
  fwrite(&height, sizeof(size_t), 1, index_file);
  for(ui range(level_itr, 0, height)) {
    // write each tree node count
    fwrite(&level_node_count[level_itr], sizeof(ui), 1, index_file);
  }

  // write total node count
  fwrite(&total_node_count, sizeof(ui), 1, index_file);

  // write leaf node count
  fwrite(&leaf_node_count, sizeof(ui), 1, index_file);

  // Unlike dump function in MPHR class, we use the queue structure to dump the
  // tree onto an index file since the nodes are allocated here and there in a
  // Top-Down fashion
  std::queue<node::Node*> bfs_queue;
  std::vector<ll> original_child_offset; // for backup

  // push the root node
  bfs_queue.emplace(node_ptr);

  // if the queue is not empty,
  while(!bfs_queue.empty()) {
    // pop the first element 
    node::Node* node = bfs_queue.front();
    bfs_queue.pop();

    // NOTE : Backup the child offsets in order to recover node's child offset later
    // I believe accessing memory is faster than accesing disk,
    // I don't use another fwrite for this job.
    if( node->GetNodeType() == NODE_TYPE_INTERNAL) {
      for(ui range(child_itr, 0, node->GetBranchCount())) {
        node::Node* child_node = node->GetBranchChildNode(child_itr);
        bfs_queue.emplace(child_node);

        // backup current child offset
        original_child_offset.emplace_back(node->GetBranchChildOffset(child_itr));

        // reassign child offset
        ll child_offset = (ll)bfs_queue.size()*(ll)sizeof(node::Node);
        node->SetBranchChildOffset(child_itr, child_offset);
      }
    }

    // write an internal node on disk
    fwrite(node, sizeof(node::Node), 1, index_file);

    // Recover child offset
    if( node->GetNodeType() == NODE_TYPE_INTERNAL) {
      for(ui range(child_itr, 0, node->GetBranchCount())) {
        // reassign child offset
        node->SetBranchChildOffset(child_itr, original_child_offset[child_itr]);
      }
    }
    original_child_offset.clear();
  }

  // write leaf nodes
  fwrite(node_soa_ptr, sizeof(node::Node_SOA), leaf_node_count, index_file);
  fclose(index_file);

  auto elapsed_time = recorder.TimeRecordEnd();
  LOG_INFO("Done, time = %.6fs", elapsed_time/1000.0f);
  return true;
}

int Hybrid::Search(std::shared_ptr<io::DataSet> query_data_set, 
                   ui number_of_search){
  auto& recorder = evaluator::Recorder::GetInstance();

  //===--------------------------------------------------------------------===//
  // Read Query 
  //===--------------------------------------------------------------------===//
  auto query = query_data_set->GetPoints();
  auto d_query = query_data_set->GetDeviceQuery(number_of_search);

  //===--------------------------------------------------------------------===//
  // Prepare Hit & Node Visit Variables for an evaluation
  //===--------------------------------------------------------------------===//
  ui h_hit[GetNumberOfBlocks()] = {0};
  ui h_node_visit_count[GetNumberOfBlocks()] = {0};

  ui total_hit = 0;
  ui total_node_visit_count_cpu = 0;
  ui total_node_visit_count_gpu = 0;

  ui* d_hit;
  cudaErrCheck(cudaMalloc((void**) &d_hit, sizeof(ui)*GetNumberOfBlocks()));
  ui* d_node_visit_count;
  cudaErrCheck(cudaMalloc((void**) &d_node_visit_count, sizeof(ui)*GetNumberOfBlocks()));

  // initialize hit and node visit variables to zero
  global_SetHitCount<<<1,GetNumberOfBlocks()>>>(0);

  //===--------------------------------------------------------------------===//
  // Execute Search Function
  //===--------------------------------------------------------------------===//
  recorder.TimeRecordStart();
  ui number_of_batch = GetNumberOfBlocks();

  float total_jump_count = 0;
  for(ui range(query_itr, 0, number_of_search)) {
    ll visited_leafIndex = 0;
    ui node_visit_count = 0;
    ui query_offset = query_itr*GetNumberOfDims()*2;
    ui jump_count = 0;

    while(1) {
      //===--------------------------------------------------------------------===//
      // Traversal Internal Nodes on CPU
      //===--------------------------------------------------------------------===//
      auto start_node_index = TraverseInternalNodes(node_ptr, &query[query_offset],
                                                    visited_leafIndex, &node_visit_count);
      total_node_visit_count_cpu += node_visit_count;

      // no more overlapping internal nodes, terminate current query
      if( start_node_index == 0) {
        break;
      }

      auto start_node_offset = (start_node_index-1)/GetNumberOfDegrees(); 

      // XXX to pretend MPES
      //chunk_size = leaf_node_count;

      // resize chunk_size if the sum of start node offset and chunk size is
      // larger than number of leaf nodes
      if(start_node_offset+chunk_size > leaf_node_count) {
        chunk_size = leaf_node_count - start_node_offset;
      }

      // XXX
      //start_node_offset = 0;

      //===--------------------------------------------------------------------===//
      // Parallel Scanning Leaf Nodes on the GPU 
      //===--------------------------------------------------------------------===//
      global_ParallelScanning_Leafnodes<<<number_of_batch,GetNumberOfThreads()>>>
                             (&d_query[query_offset], start_node_offset, chunk_size);
      //cudaDeviceSynchronize();

      visited_leafIndex = (start_node_offset+chunk_size)*GetNumberOfDegrees();
      jump_count++;
    }
    total_jump_count += jump_count;

    //BruteForceSearchOnCPU(&query[query_offset]);
  }
  LOG_INFO("Avg. Jump Count %f", total_jump_count/number_of_search);

  global_GetHitCount<<<1,GetNumberOfBlocks()>>>(d_hit, d_node_visit_count);
  cudaMemcpy(h_hit, d_hit, sizeof(ui)*GetNumberOfBlocks(), cudaMemcpyDeviceToHost);
  cudaMemcpy(h_node_visit_count, d_node_visit_count, sizeof(ui)*GetNumberOfBlocks(), 
                                                             cudaMemcpyDeviceToHost);

  for(ui range(i, 0, GetNumberOfBlocks())) {
    total_hit += h_hit[i];
    total_node_visit_count_gpu += h_node_visit_count[i];
  }

  auto elapsed_time = recorder.TimeRecordEnd();
  LOG_INFO("Search Time on the GPU = %.6fms", elapsed_time);

  //===--------------------------------------------------------------------===//
  // Show Results
  //===--------------------------------------------------------------------===//
  LOG_INFO("Hit : %u", total_hit);
  LOG_INFO("Node visit count on CPU : %u", total_node_visit_count_cpu);
  LOG_INFO("Node visit count on GPU : %u", total_node_visit_count_gpu);
}

void Hybrid::SetChunkSize(ui _chunk_size){
  chunk_size = _chunk_size;
}

ll Hybrid::TraverseInternalNodes(node::Node *node_ptr, Point* query, 
                                 ll visited_leafIndex, ui *node_visit_count) {

  ll start_node_index=0;
  (*node_visit_count)++;

  // internal nodes
  if(node_ptr->GetNodeType() == NODE_TYPE_INTERNAL ) {
    for(ui range(branch_itr, 0, node_ptr->GetBranchCount())) {
      if( node_ptr->GetBranchIndex(branch_itr) > visited_leafIndex && 
          node_ptr->IsOverlap(query, branch_itr)) {
        start_node_index=TraverseInternalNodes(node_ptr->GetBranchChildNode(branch_itr), 
                                            query, visited_leafIndex, node_visit_count);

        if(start_node_index > 0) break;
      }
    }
  } // leaf nodes
  else {
    for(ui range(branch_itr, 0, node_ptr->GetBranchCount())) {
      if( node_ptr->GetBranchIndex(branch_itr) > visited_leafIndex ) {
        start_node_index = node_ptr->GetBranchIndex(branch_itr);
        break;
      }
    }
  }

  return start_node_index;
}

bool Hybrid::BruteForceSearchOnCPU(Point* query) {

  auto& recorder = evaluator::Recorder::GetInstance();
  const size_t number_of_threads = std::thread::hardware_concurrency();

  std::vector<ll> start_node_offset;
  ui hit=0;

  // parallel for loop using c++ std 11 
  {
    std::vector<std::thread> threads;
    std::vector<ll> thread_start_node_offset[number_of_threads];
    ui thread_hit[number_of_threads];

    auto chunk_size = leaf_node_count/number_of_threads;
    auto start_offset = 0 ;
    auto end_offset = start_offset + chunk_size + leaf_node_count%number_of_threads;

    //Launch a group of threads
    for (ui range(thread_itr, 0, number_of_threads)) {
      threads.push_back(std::thread(&Hybrid::Thread_BruteForce, this, 
                        query, std::ref(thread_start_node_offset[thread_itr]), std::ref(thread_hit[thread_itr]),
                        start_offset, end_offset));

      start_offset = end_offset;
      end_offset += chunk_size;
    }

    //Join the threads with the main thread
    for(auto &thread : threads){
      thread.join();
    }

    for(ui range(thread_itr, 0, number_of_threads)) {
      start_node_offset.insert( start_node_offset.end(), 
                                thread_start_node_offset[thread_itr].begin(), 
                                thread_start_node_offset[thread_itr].end()); 
      hit += thread_hit[thread_itr];
    }
  }

  std::sort(start_node_offset.begin(), start_node_offset.end());

  for( auto offset : start_node_offset) {
    LOG_INFO("start node offset %lu", offset);
  }
  //LOG_INFO("Hit on CPU : %u", hit);

  auto elapsed_time = recorder.TimeRecordEnd();
  LOG_INFO("BruteForce Scanning on the CPU (%u threads) = %.6fs", number_of_threads, elapsed_time/1000.0f);

  return true;
}

void Hybrid::Thread_BruteForce(Point* query, std::vector<ll> &start_node_offset,
                               ui &hit, ui start_offset, ui end_offset) {
  for(ui range(node_itr, start_offset, end_offset)) {
    for(ui range(child_itr, 0, node_soa_ptr[node_itr].GetBranchCount())) {
      if( node_soa_ptr[node_itr].IsOverlap(query, child_itr) ) {
        start_node_offset.emplace_back(node_itr);
        hit++;
      }
    }
  }
}

//===--------------------------------------------------------------------===//
// Cuda Variable & Function 
//===--------------------------------------------------------------------===//

__device__ ui g_hit[GetNumberOfBlocks()]; 
__device__ ui g_node_visit_count[GetNumberOfBlocks()]; 

__global__
void global_SetHitCount(ui init_value) {
  int tid = threadIdx.x;

  g_hit[tid] = init_value;
  g_node_visit_count[tid] = init_value;
}

__global__
void global_GetHitCount(ui* hit, ui* node_visit_count) {
  int tid = threadIdx.x;

  hit[tid] = g_hit[tid];
  node_visit_count[tid] = g_node_visit_count[tid];
}

__global__ 
void global_ParallelScanning_Leafnodes(Point* _query, ll start_node_offset, 
                                       ui chunk_size) {
  int bid = blockIdx.x;
  int tid = threadIdx.x;

  __shared__ Point query[GetNumberOfDims()*2];
  __shared__ ui t_hit[GetNumberOfThreads()]; 

  if(tid < GetNumberOfDims()*2) {
    query[tid] = _query[tid];
  }

  t_hit[tid] = 0;

  node::Node_SOA* first_leaf_node = g_node_soa_ptr;
  node::Node_SOA* node_soa_ptr = first_leaf_node + start_node_offset + bid;

  __syncthreads();

  for(ui range(node_itr, bid, chunk_size, GetNumberOfBlocks())) {

    MasterThreadOnly {
      g_node_visit_count[bid]++;
    }

    if(tid < node_soa_ptr->GetBranchCount() &&
        node_soa_ptr->IsOverlap(query, tid)) {
      t_hit[tid]++;
    }
    __syncthreads();

    node_soa_ptr+=GetNumberOfBlocks();
  }
  __syncthreads();

  //===--------------------------------------------------------------------===//
  // Parallel Reduction 
  //===--------------------------------------------------------------------===//
  ParallelReduction(t_hit, GetNumberOfThreads());

  MasterThreadOnly {
    if(N==1) {
      g_hit[bid] += t_hit[0] + t_hit[1];
    } else {
      g_hit[bid] += t_hit[0];
    }
  }
}

} // End of tree namespace
} // End of ursus namespace

