# 大规模图数据中kmax-truss问题的求解和算法优化

> [比赛网址](https://www.datafountain.cn/competitions/473/datasets)

## 代码编译说明

* 方法一：在代码目录下运行`make`命令，当前目录得到`kmax_truss`可执行程序。

* 方法二：

```shell script
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

在项目根目录下的bin目录内得到`kmax_truss`可执行程序

## 运行使用说明

运行命令：

```shell script
./kmax_truss -f ***.tsv(图数据文件路径)
```

标准输出为：

```text
kmax = x, Edges in kmax-truss = y.
```

其中x、y为整数，x代表kmax-truss值，y代表kmax-truss中的边数量。

## 基本算法介绍

算法流程如下：

* 读取文件
* 图Core分解获取kmax的上界
* 根据上界获取子图，数据预处理
* 三角形记数获取支持边信息
* Truss分解获取kmax的下界
* 根据下界获取子图，数据预处理
* 三角形记数获取支持边信息
* Truss分解获取kmax的真实值

## 并行化设计思路和方法

### 读取文件

采用多线程分块读取文件，自动识别块首和块尾。

### 数据预处理

边的解压，CSR建图，获取边的序号等操作均采取并行化设计。

### 三角形记数

采用并行边迭代的方式，获取每条边的三角形数量。

### Core分解

Core分解也采用并行计算。

### Truss分解

支持边数量刷新，边的筛选均采用并行计算。

## 算法优化

所有部分都是并行计算的。

## 详细算法设计与实现

### 读取文件

核心代码如下所示：

```cpp
  std::vector<std::thread> threads(FILE_SPLIT_NUM);
  for (uint64_t i = 0; i < FILE_SPLIT_NUM; i++) {
    uint64_t start = len_ * i / FILE_SPLIT_NUM;
    uint64_t end = len_ * (i + 1) / FILE_SPLIT_NUM;
    if (i != 0) {
      while (*(byte_ + start) != '\n') {
        ++start;
      }
      ++start;
    }
    if (i + 1 != FILE_SPLIT_NUM) {
      while (*(byte_ + end) != '\n') {
        ++end;
      }
      ++end;
    }
    threads[i] = std::thread(
        [=]() { ::GetEdges(edges + edgesNum, byte_ + start, end - start); });
    edgesNum += ::GetLineNum(byte_ + start, end - start);
  }
  for (auto &thread : threads) {
    thread.join();
  }
```

### 数据预处理

```c++
// 计算节点的度
void CalDeg(const uint64_t *edges, EdgeT edgesNum, NodeT nodesNum,
            NodeT *&deg) {
  deg = (NodeT *)calloc(nodesNum, sizeof(NodeT));

#pragma omp parallel for
  for (EdgeT i = 0; i < edgesNum; i++) {
    __sync_fetch_and_add(&deg[FIRST(edges[i])], 1);
  }
}
```

```c++
// 边的解压缩
void Unzip(const uint64_t *edges, EdgeT edgesNum, NodeT *&edgesFirst,
           NodeT *&edgesSecond) {
  edgesFirst = (NodeT *)malloc(edgesNum * sizeof(NodeT));
  edgesSecond = (NodeT *)malloc(edgesNum * sizeof(NodeT));
#pragma omp parallel for
  for (EdgeT i = 0; i < edgesNum; i++) {
    edgesFirst[i] = FIRST(edges[i]);
    edgesSecond[i] = SECOND(edges[i]);
  }
}
```

```c++
// 边编号
void GetEdgesId(const uint64_t *edges, EdgeT edgesNum, EdgeT *&edgesId,
                const EdgeT *halfNodeIndex, const NodeT *halfEdgesSecond) {
  edgesId = (EdgeT *)malloc(edgesNum * sizeof(EdgeT));

#pragma omp parallel for schedule(dynamic, 1024)
  for (EdgeT i = 0u; i < edgesNum; i++) {
    NodeT u = std::min(FIRST(edges[i]), SECOND(edges[i]));
    NodeT v = std::max(FIRST(edges[i]), SECOND(edges[i]));
    edgesId[i] = std::lower_bound(halfEdgesSecond + halfNodeIndex[u],
                                  halfEdgesSecond + halfNodeIndex[u + 1], v) -
                 halfEdgesSecond;
  }
}
```

### 三角形记数

采用边迭代并行计算的方式，将无向图转化成有向图，省去一半的计算。

```c++
// 三角形计数获取支持边数量
void GetEdgeSup(EdgeT halfEdgesNum, NodeT *&halfEdgesFirst,
                NodeT *&halfEdgesSecond, NodeT *&halfDeg, EdgeT *&halfNodeIndex,
                NodeT *&edgesSup) {
  edgesSup = (NodeT *)calloc(halfEdgesNum, sizeof(NodeT));
#pragma omp parallel for schedule(dynamic, 1024)
  for (EdgeT i = 0; i < halfEdgesNum; i++) {
    NodeT u = halfEdgesFirst[i];
    NodeT v = halfEdgesSecond[i];
    EdgeT uStart = halfNodeIndex[u];
    EdgeT uEnd = halfNodeIndex[u + 1];
    EdgeT vStart = halfNodeIndex[v];
    EdgeT vEnd = halfNodeIndex[v + 1];
    while (uStart < uEnd && vStart < vEnd) {
      if (halfEdgesSecond[uStart] < halfEdgesSecond[vStart]) {
        ++uStart;
      } else if (halfEdgesSecond[uStart] > halfEdgesSecond[vStart]) {
        ++vStart;
      } else {
        __sync_fetch_and_add(&edgesSup[i], 1);
        __sync_fetch_and_add(&edgesSup[uStart], 1);
        __sync_fetch_and_add(&edgesSup[vStart], 1);
        ++uStart;
        ++vStart;
      }
    }
  }
}
```

### Core分解

主要有以下几个函数：

* 并行扫描度取值

```c++
void Scan(NodeT n, const NodeT *deg, NodeT level, NodeT *curr,
          NodeT &currTail);
```

* 子任务循环迭代分解

```c++
void SubLevel(const EdgeT *nodeIndex, const NodeT *edgesSecond,
              const NodeT *curr, NodeT currTail, NodeT *deg, NodeT level,
              NodeT *next, NodeT &nextTail);
```

* 求解k-core的主流程

```c++
void KCore(const EdgeT *nodeIndex, const NodeT *edgesSecond, NodeT nodesNum,
           NodeT *deg);
```

### Truss分解

主要有以下几个函数：

* 并行扫描支持边是否与truss层次相同

```c++
void Scan(EdgeT numEdges, const NodeT *edgesSup, NodeT level, EdgeT *curr,
          EdgeT &currTail, bool *inCurr);
```

* 并行扫描支持边层次小于指定层次

```c++
void ScanLessThanLevel(EdgeT numEdges, const NodeT *edgesSup, NodeT level,
                       EdgeT *curr, EdgeT &currTail, bool *inCurr);
```

* 更新支持边的数值

```c++
void UpdateSup(EdgeT e, NodeT *edgesSup, NodeT level, NodeT *buff, EdgeT &index,
               EdgeT *next, bool *inNext, EdgeT &nextTail);
```

* 子任务循环迭代消减truss

```c++
void SubLevel(const EdgeT *nodeIndex, const NodeT *edgesSecond,
              const EdgeT *curr, bool *inCurr, EdgeT currTail, NodeT *edgesSup,
              NodeT level, EdgeT *next, bool *inNext, EdgeT &nextTail,
              bool *processed, const EdgeT *edgesId,
              const uint64_t *halfEdges);
```

* 求解k-truss的主流程

```c++
void KTruss(const EdgeT *nodeIndex, const NodeT *edgesSecond,
            const EdgeT *edgesId, const uint64_t *halfEdges, EdgeT halfEdgesNum,
            NodeT *edgesSup, NodeT startLevel);
```

## 实验结果与分析

### 图数据集

> [下载地址](http://datafountain.int-yt.com/Files/BDCI2020/473HuaKeDaKtruss/ktruss-data.zip)

| **数据集**               | **说明**                          | **kmax** | **Edges in kmax-truss** |
|:---------------------:|:-----------------------------------:|:--------:|:-----------------------:|
| s18.e16.rmat.edgelist | 顶点数：0.2 million、边数：7 million  | 164      | 225,529                 |
| s19.e16.rmat.edgelist | 顶点数：0.5 million、边数：15 million | 223      | 334,934                 |
| cit-Patents           | 顶点数：3.7 million、边数：33 million | 36       | 2,625                   |
| soc-LiveJournal       | 顶点数：4.8 million、边数：85 million | 362      | 72,913                  |

以上文件中，图用顺序存放的边表表示。每条边包含两个顶点以及权重（其中源顶点和目标顶点各占4个字节），边的存储长度为8个字节，文件的存储格式如下：  
目标顶点（4字节，无符号整型） 源顶点（4字节，无符号整型） 权重  
目标顶点（4字节，无符号整型） 源顶点（4字节，无符号整型） 权重  
……[EOF]  

### 运行环境

4核8线程 2.6GHz

```shell script
# cat /proc/cpuinfo | grep name | cut -f2 -d: | uniq -c
      8  Intel(R) Xeon(R) Gold 6278C CPU @ 2.60GHz
# cat /proc/cpuinfo| grep "physical id"| sort| uniq| wc -l
1
# cat /proc/cpuinfo| grep "cpu cores"| uniq
cpu cores       : 4
# cat /proc/cpuinfo| grep "processor"| wc -l
8
```

### 实验结果

| **数据集**            | **单线程(ms)** | **多线程(ms)** |
|:---------------------:|:-----------:|:-----------:|
| s18.e16.rmat.edgelist | 13384       | 1983        |
| s19.e16.rmat.edgelist | 28888       | 4022        |
| cit-Patents           | 2500        | 602         |
| soc-LiveJournal       | 6582        | 1482        |

## 程序代码模块说明

| ****文件****                           | ****功能****           |
|----------------------------------------|-----------------------|
| [src/main.cpp](src/main.cpp)           | 代码主流程             |
| [src/graph.cpp](src/graph.cpp)         | 图的处理流程           |
| [src/kcore.cpp](src/kcore.cpp)         | 图的Core分解           |
| [src/ktruss.cpp](src/ktruss.cpp)       | 图的Truss分解          |
| [src/kron_gen.cpp](src/kron_gen.cpp)   | 自定义生成kron图测试使用|
| [src/log.cpp](src/log.cpp)             | 日志打印               |
| [src/preprocess.cpp](src/preprocess.cpp)| 图的预处理            |
| [src/read_file.cpp](src/read_file.cpp) | 文件读取               |
| [src/tricount.cpp](src/tricount.cpp)   | 三角形计算支持边        |
