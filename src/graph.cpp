#include "graph.h"

#include <algorithm>
#include <map>

#pragma ide diagnostic ignored "openmp-use-default-none"

Graph::Graph(uint64_t *edges, EdgeT edgesNum)
    : coreClock_("kCore"),
      preprocessClock_("Preprocess"),
      triCountClock_("TriCount"),
      trussClock_("Truss"),
      rawEdges_(edges),
      rawEdgesNum_(edgesNum) {
  rawNodesNum_ = FIRST(rawEdges_[rawEdgesNum_ - 1]) + 1;
}

void Graph::FreeRawGraph() {
#ifndef CUDA
  if (rawEdges_) {
    MyFree((void *&)rawEdges_, rawEdgesNum_ * sizeof(uint64_t));
  }
  if (rawCore_) {
    MyFree((void *&)rawCore_, rawNodesNum_ * sizeof(NodeT));
  }
  if (rawNodeIndex_) {
    MyFree((void *&)rawNodeIndex_, (rawNodesNum_ + 1) * sizeof(EdgeT));
  }
  if (rawAdj_) {
    MyFree((void *&)rawAdj_, rawEdgesNum_ * sizeof(NodeT));
  }
#endif
}

void Graph::FreeGraph() {
#ifndef CUDA
  if (edges_) {
    MyFree((void *&)edges_, edgesNum_ * sizeof(uint64_t));
  }
  if (nodeIndex_) {
    MyFree((void *&)nodeIndex_, (nodesNum_ + 1) * sizeof(EdgeT));
  }
  if (adj_) {
    MyFree((void *&)adj_, edgesNum_ * sizeof(NodeT));
  }
#endif
}

void Graph::FreeHalfGraph() {
#ifndef CUDA
  if (halfEdges_) {
    MyFree((void *&)halfEdges_, halfEdgesNum_ * sizeof(uint64_t));
  }
  if (halfNodeIndex_) {
    MyFree((void *&)halfNodeIndex_, (halfNodesNum_ + 1) * sizeof(EdgeT));
  }
  if (halfAdj_) {
    MyFree((void *&)halfAdj_, halfEdgesNum_ * sizeof(NodeT));
  }
#endif
}

Graph::~Graph() {
  FreeRawGraph();
  FreeGraph();
  FreeHalfGraph();
}

NodeT Graph::GetMaxCore() {
  log_info(coreClock_.Start());

  ::ConstructCSRGraph(rawEdges_, rawEdgesNum_, rawNodeIndex_, rawAdj_);
  log_info(coreClock_.Count("Construct Raw CSR Graph"));

  ::KCore(rawNodeIndex_, rawAdj_, rawNodesNum_, rawCore_);
  log_info(coreClock_.Count("KCore"));

  NodeT maxCoreNum = 0;
#pragma omp parallel for reduction(max : maxCoreNum)
  for (NodeT i = 0; i < rawNodesNum_; i++) {
    maxCoreNum = std::max(maxCoreNum, rawCore_[i]);
  }

#ifdef CUDA
  // rawCore_
  //  CUDA_TRY(cudaMalloc((void **)&cudaRawCore_, rawNodesNum_ * sizeof(NodeT)));
  CUDA_TRY(cudaMallocManaged((void **)&cudaRawCore_, rawNodesNum_ * sizeof(NodeT)));
  CUDA_TRY(cudaMemcpy(cudaRawCore_, rawCore_, rawNodesNum_ * sizeof(NodeT), cudaMemcpyHostToDevice));
#endif

  log_info(coreClock_.Count("maxK: %u", maxCoreNum));
  return maxCoreNum;
}

// 获取max-k-truss主流程
NodeT Graph::KMaxTruss(NodeT startK, NodeT startLevel) {
  // 预处理
  Preprocess(startK);
  if (edgesNum_ == 0) {
    return 0;
  }

  // 三角形计数
  log_info(triCountClock_.Start());
#ifdef CUDA
  ::GetEdgeSup(halfEdgesNum_, halfEdges_, halfNodeIndex_, nodesNum_, edgesSup_);
#else
  ::GetEdgeSup(halfNodeIndex_, halfAdj_, halfNodesNum_, edgesSup_);
#endif
  log_info(triCountClock_.Count("Count"));

  // TODO can remove
  uint64_t count = 0;
  for (uint64_t i = 0; i < halfEdgesNum_; i++) {
    count += edgesSup_[i];
  }
  log_info(triCountClock_.Count("triangle count: %lu", count / 3));

  // 求解k-truss
  log_info(trussClock_.Start());
#ifdef CUDA
  CUDA_TRY(cudaMalloc((void **)&cudaNodeIndex_, (nodesNum_ + 1) * sizeof(EdgeT)));
  CUDA_TRY(cudaMemcpy(cudaNodeIndex_, nodeIndex_, (nodesNum_ + 1) * sizeof(EdgeT), cudaMemcpyHostToDevice));
  CUDA_TRY(cudaMalloc((void **)&cudaAdj_, edgesNum_ * sizeof(NodeT)));
  CUDA_TRY(cudaMemcpy(cudaAdj_, adj_, edgesNum_ * sizeof(NodeT), cudaMemcpyHostToDevice));
  CUDA_TRY(cudaMalloc((void **)&cudaEdgesId_, edgesNum_ * sizeof(EdgeT)));
  CUDA_TRY(cudaMemcpy(cudaEdgesId_, edgesId_, edgesNum_ * sizeof(EdgeT), cudaMemcpyHostToDevice));
  CUDA_TRY(cudaMalloc((void **)&cudaHalfEdges_, halfEdgesNum_ * sizeof(uint64_t)));
  CUDA_TRY(cudaMemcpy(cudaHalfEdges_, halfEdges_, halfEdgesNum_ * sizeof(uint64_t), cudaMemcpyHostToDevice));
  CUDA_TRY(cudaMalloc((void **)&cudaEdgesSup_, halfEdgesNum_ * sizeof(NodeT)));
  CUDA_TRY(cudaMemcpy(cudaEdgesSup_, edgesSup_, halfEdgesNum_ * sizeof(NodeT), cudaMemcpyHostToDevice));
  ::KTruss(cudaNodeIndex_, cudaAdj_, cudaEdgesId_, cudaHalfEdges_, halfEdgesNum_, cudaEdgesSup_, startLevel);
  CUDA_TRY(cudaDeviceSynchronize());
  CUDA_TRY(cudaMemcpy(edgesSup_, cudaEdgesSup_, halfEdgesNum_ * sizeof(NodeT), cudaMemcpyDeviceToHost));
  CUDA_TRY(cudaDeviceSynchronize());
#else
  ::KTruss(nodeIndex_, adj_, edgesId_, halfEdges_, halfEdgesNum_, edgesSup_, startLevel);
#endif
  log_info(trussClock_.Count("KTruss"));

  FreeHalfGraph();
  FreeGraph();

  // 打印信息
  NodeT possibleKMax = DisplayStats(edgesSup_, halfEdgesNum_, startK);

  return possibleKMax;
}

// 图的预处理
void Graph::Preprocess(NodeT startK) {
  log_info(preprocessClock_.Start());
  log_info(preprocessClock_.Count("startK: %u", startK));

  if (startK > 2u) {
#ifdef CUDA
    edgesNum_ = ::ConstructNewGraph(rawEdges_, edges_, cudaRawCore_, rawEdgesNum_, startK);
#else
    edgesNum_ = ::ConstructNewGraph(rawEdges_, edges_, rawCore_, rawEdgesNum_, startK);
#endif
    log_info(preprocessClock_.Count("ConstructNewGraph edgesNum_: %u", edgesNum_));
    if (edgesNum_ == 0) {
      return;
    }
  } else {
#ifdef CUDA
    // cuda memcpy
    edges_ = rawEdges_;
#else
    edges_ = rawEdges_;
#endif
    edgesNum_ = rawEdgesNum_;
    rawEdges_ = nullptr;
  }

  ::ConstructCSRGraph(edges_, edgesNum_, nodeIndex_, adj_);
  nodesNum_ = FIRST(edges_[edgesNum_ - 1]) + 1;
  log_info(preprocessClock_.Count("CSR nodesNum_: %u", nodesNum_));

  halfEdgesNum_ = edgesNum_ / 2;
  ::ConstructHalfEdges(edges_, halfEdges_, halfEdgesNum_);
  log_info(preprocessClock_.Count("halfEdgesNum_: %u", halfEdgesNum_));

  ::ConstructCSRGraph(halfEdges_, halfEdgesNum_, halfNodeIndex_, halfAdj_);
  halfNodesNum_ = FIRST(halfEdges_[halfEdgesNum_ - 1]) + 1;
  log_info(preprocessClock_.Count("Half CSR halfNodesNum_: %u", halfNodesNum_));

  ::GetEdgesId(edges_, edgesNum_, halfNodeIndex_, halfAdj_, edgesId_);
  log_info(preprocessClock_.Count("GetEdgesId"));

#ifdef CUDA
  // edges_ nodeIndex_ adj_ halfEdges_ halfNodeIndex_ halfAdj_ edgesId_
#endif
}
