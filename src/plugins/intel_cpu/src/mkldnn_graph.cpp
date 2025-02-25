// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <algorithm>
#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <unordered_set>
#include <limits>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <utility>

#include "mkldnn_graph.h"
#include "mkldnn_graph_dumper.h"
#include "mkldnn_graph_optimizer.h"
#include "mkldnn_extension_utils.h"
#include "mkldnn_extension_mngr.h"
#include "memory_solver.hpp"
#include "mkldnn_itt.h"
#include "mkldnn_infer_request.h"
#include <nodes/mkldnn_input_node.h>
#include <nodes/mkldnn_reorder_node.h>
#include <nodes/mkldnn_convert_node.h>

#include <ie_algorithm.hpp>
#include <blob_factory.hpp>
#include "nodes/common/cpu_memcpy.h"
#include "nodes/common/cpu_convert.h"

#include "precision_utils.h"
#include <ie_plugin_config.hpp>

#include "utils/general_utils.h"
#include "utils/debug_capabilities.h"
#include "utils/node_dumper.h"
#include "utils/ngraph_utils.hpp"
#include "utils/cpu_utils.hpp"
#include "utils/verbose.h"
#include "memory_desc/cpu_memory_desc_utils.h"

#include <ngraph/node.hpp>
#include <ngraph/function.hpp>
#include <ngraph/variant.hpp>
#include <ngraph/ops.hpp>
#include <transformations/utils/utils.hpp>
#include <low_precision/low_precision.hpp>
#include "memory_desc/dnnl_blocked_memory_desc.h"

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace InferenceEngine::details;

typedef std::unordered_set<MKLDNNEdgePtr> edge_cluster_t;
typedef std::vector<edge_cluster_t> edge_clusters_t;

mkldnn::engine MKLDNNGraph::eng(mkldnn::engine::kind::cpu, 0);

template<typename NET>
void MKLDNNGraph::CreateGraph(NET &net, const MKLDNNExtensionManager::Ptr& extMgr,
        MKLDNNWeightsSharing::Ptr &w_cache) {
    OV_ITT_SCOPE(FIRST_INFERENCE, MKLDNNPlugin::itt::domains::MKLDNN_LT, "CreateGraph");

    if (IsReady())
        ForgetGraphData();
    // disable caching if graph was created only once
    weightsCache = config.streamExecutorConfig._streams != 1 ? w_cache : nullptr;

    Replicate(net, extMgr);
    InitGraph();

    status = Ready;

    CPU_DEBUG_CAP_ENABLE(serialize(*this));
}

template void MKLDNNGraph::CreateGraph(const std::shared_ptr<const ngraph::Function>&,
        const MKLDNNExtensionManager::Ptr&, MKLDNNWeightsSharing::Ptr&);
template void MKLDNNGraph::CreateGraph(const CNNNetwork&,
        const MKLDNNExtensionManager::Ptr&, MKLDNNWeightsSharing::Ptr&);

void MKLDNNGraph::Replicate(const std::shared_ptr<const ngraph::Function> &subgraph, const MKLDNNExtensionManager::Ptr& extMgr) {
    this->_name = "subgraph";
    this->reuse_io_tensors = false;

    isQuantizedFlag = (config.lpTransformsMode == Config::On) &&
                      ngraph::pass::low_precision::LowPrecision::isFunctionQuantized(subgraph);

    // Map data object onto producer node
    std::map<std::shared_ptr<ngraph::Node>, std::pair<MKLDNNNodePtr, int>> op2node;

    // nodes which has no consumers (output or just unused). But doesn't marked as graph output.
    // Will be stored as fake output separately.
    std::deque<ngraph::Output<ngraph::Node>> unusedOutputs;

    auto getParentOutputPort = [](const std::shared_ptr<ngraph::Node> childOp, const std::shared_ptr<ngraph::Node> parentOp,
                                  const size_t childInputPort) -> int {
        for (size_t parentPort = 0; parentPort < parentOp->get_output_size(); parentPort++) {
            if (childOp->input(childInputPort).get_tensor_ptr() == parentOp->output(parentPort).get_tensor_ptr()) {
                return static_cast<int>(parentPort);
            }
        }

        return -1;
    };

    for (const auto op : subgraph->get_ordered_ops()) {
        const MKLDNNNodePtr node {MKLDNNNode::factory().create(op, getEngine(), extMgr, weightsCache)};
        if (isQuantized()) {
            node->setQuantizedGraphFlag(true);
        }

        graphNodes.push_back(node);

        if (op->get_type_info() == ngraph::op::v0::Parameter::get_type_info_static()) {
            inputNodesMap[node->getName()] = node;
        }

        if (op->get_type_info() == ngraph::op::v0::Result::get_type_info_static()) {
            const auto prev = op->input_value(0);
            const std::string inputID = ngraph::op::util::get_ie_output_name(prev);

            outputNodesMap[inputID] = node;
        }

        for (size_t port = 0; port < op->get_input_size(); port++) {
            auto parentOp = op->get_input_node_shared_ptr(port);

            auto portInfo = op2node[parentOp];
            auto parentNode = portInfo.first;

            MKLDNNEdgePtr edge(new MKLDNNEdge(parentNode, node, getParentOutputPort(op, parentOp, port), port));
            node->addEdge(edge);
            graphEdges.push_back(edge);
        }

        if (!MKLDNNPlugin::one_of(op->get_type_info(),
                ngraph::op::v0::Result::get_type_info_static(),
                ngraph::op::v3::Assign::get_type_info_static(),
                ngraph::op::v6::Assign::get_type_info_static())) {
            int outPortIdx = 0;
            for (int oi = 0; oi < op->get_output_size(); oi++) {
                op2node[op->output(oi).get_node_shared_ptr()] = {node, outPortIdx++};
                if (op->get_output_target_inputs(oi).empty()) {
                    unusedOutputs.push_back(op->output(oi));
                }
            }
        }
    }

    // Add stub output node for unused data
    for (auto unusedOutput : unusedOutputs) {
        auto portInfo = op2node[unusedOutput.get_node_shared_ptr()];
        auto parentNode = portInfo.first;
        auto port = portInfo.second;
        const auto nodeName = std::string("stub_") + std::to_string(unusedOutput.get_index()) + "_" + parentNode->getName();
        const MKLDNNNodePtr outNode = std::make_shared<MKLDNNInputNode>(parentNode->outputShapes[port],
                                                                        parentNode->getOriginalOutputPrecisionAtPort(port),
                                                                        nodeName, "Result", getEngine(), weightsCache);
        MKLDNNEdgePtr edge(new MKLDNNEdge(parentNode, outNode, port, 0));
        outNode->addEdge(edge);
        graphEdges.push_back(edge);
        graphNodes.push_back(outNode);
    }
}

void MKLDNNGraph::Replicate(const CNNNetwork &network, const MKLDNNExtensionManager::Ptr& extMgr) {
    OV_ITT_SCOPE_CHAIN(FIRST_INFERENCE, taskChain, itt::domains::MKLDNN_LT, "MKLDNNGraph::Replicate", "CNNNetwork");

    InputsDataMap inputsInfo = network.getInputsInfo();
    OutputsDataMap outputsInfo = network.getOutputsInfo();

    this->_name = network.getName();

    std::shared_ptr<const ngraph::Function> func = network.getFunction();
    if (!func) {
        IE_THROW() << "Function pointer inside CNNNetwork is nullptr";
    }

    isQuantizedFlag = (config.lpTransformsMode == Config::On) &&
                      ngraph::pass::low_precision::LowPrecision::isFunctionQuantized(func);

    auto orderedOps = func->get_ordered_ops();

    // TODO [NM]: unordered_map is preferred from performance perspective. Needs hash for ngraph::Node
    std::map<std::shared_ptr<ngraph::Node>, MKLDNNNodePtr> op2node;
    std::deque<ngraph::Output<ngraph::Node>> unusedOutputs;  // nodes which has no consumers (output or just unused)

    auto getParentOutputPort = [](const std::shared_ptr<ngraph::Node> childOp, const std::shared_ptr<ngraph::Node> parentOp,
                                  const size_t childInputPort) -> int {
        for (size_t parentPort = 0; parentPort < parentOp->get_output_size(); parentPort++) {
            if (childOp->input(childInputPort).get_tensor_ptr() == parentOp->output(parentPort).get_tensor_ptr()) {
                return static_cast<int>(parentPort);
            }
        }

        return -1;
    };

    OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, "AllNodes");

    // Replicate All Nodes in topological order
    for (const auto& op : orderedOps) {
        const MKLDNNNodePtr node(MKLDNNNode::factory().create(op, getEngine(), extMgr, weightsCache));
        if (isQuantized()) {
            node->setQuantizedGraphFlag(true);
        }
        graphNodes.push_back(node);

        if (op->get_type_info() == ngraph::op::v0::Parameter::get_type_info_static()) {
            const auto inInfo = inputsInfo.find(node->getName());
            if (inInfo != inputsInfo.end()) {
                inputNodesMap[node->getName()] = node;
                if (node->isDynamicNode()) {
                    graphHasDynamicInput = true;
                }
            }
        }

        if (op->get_type_info() == ngraph::op::v0::Result::get_type_info_static()) {
            const auto &input = op->input_value(0);
            const auto name = ngraph::op::util::get_ie_output_name(input);

            if (outputsInfo.count(name) != 0) {
                outputNodesMap[name] = node;
            }
        }

        op2node[op] = node;

        for (size_t port = 0; port < op->get_input_size(); port++) {
            auto parentOp = op->get_input_node_shared_ptr(port);
            auto parentNode = op2node[parentOp];

            MKLDNNEdgePtr edge(new MKLDNNEdge(parentNode, node, getParentOutputPort(op, parentOp, port), static_cast<int>(port)));
            node->addEdge(edge);
            graphEdges.push_back(edge);
        }

        if (!MKLDNNPlugin::one_of(op->get_type_info(),
                ngraph::op::v0::Result::get_type_info_static(),
                ngraph::op::v3::Assign::get_type_info_static(),
                ngraph::op::v6::Assign::get_type_info_static())) {
            for (int oi = 0; oi < op->get_output_size(); oi++) {
                if (op->get_output_target_inputs(oi).empty()) {
                    unusedOutputs.push_back(op->output(oi));
                }
            }
        }
    }

    // Add stub output node for unused outputs
    for (auto unusedOutput : unusedOutputs) {
        auto parentNode = op2node[unusedOutput.get_node_shared_ptr()];
        const auto port = unusedOutput.get_index();
        const auto nodeName = std::string("stub_") + std::to_string(unusedOutput.get_index()) + "_" + parentNode->getName();
        const MKLDNNNodePtr outNode = std::make_shared<MKLDNNInputNode>(parentNode->outputShapes[port],
                                                                        parentNode->getOriginalOutputPrecisionAtPort(port),
                                                                        nodeName, "Result", getEngine(), weightsCache);
        MKLDNNEdgePtr edge(new MKLDNNEdge(parentNode, outNode, port, 0));
        outNode->addEdge(edge);
        graphEdges.push_back(edge);
        graphNodes.push_back(outNode);
    }

    if (config.enforceBF16)
        EnforceBF16();

    // change precision for input/output nodes to avoid extra data conversion when set input/output blobs
    // also we need to change input/output precisions for consumers/producers to avoid inserting reorder
    for (auto &input : inputNodesMap) {
        const auto precToSet = normalizeToSupportedPrecision(inputsInfo.at(input.first)->getPrecision());
        input.second->setOriginalOutputPrecisionAtPort(0, precToSet);
        const auto childEdges = input.second->getChildEdgesAtPort(0);
        for (size_t i = 0; i < childEdges.size(); i++) {
            const auto child = childEdges[i]->getChild();
            if (child->getOriginalInputPrecisionAtPort(childEdges[i]->getOutputNum()) != Precision::BF16)
                child->setOriginalInputPrecisionAtPort(childEdges[i]->getOutputNum(), precToSet);
        }
    }

    for (auto &output : outputNodesMap) {
        const auto precToSet = normalizeToSupportedPrecision(outputsInfo.at(output.first)->getPrecision());
        output.second->setOriginalInputPrecisionAtPort(0, precToSet);
        const auto parentEdges = output.second->getParentEdgesAtPort(0);
        for (size_t i = 0; i < parentEdges.size(); i++) {
            const auto parent = parentEdges[i]->getParent();
            parent->setOriginalOutputPrecisionAtPort(parentEdges[i]->getInputNum(), precToSet);
        }
    }

    // Loading mean images
    for (const auto& input : inputsInfo) {
        Shape outShape;
        if (!inputNodesMap[input.first]->outputShapes.front().getRank()) {
            outShape =  Shape(SizeVector({1, 1}));
        } else {
            outShape = inputNodesMap[input.first]->outputShapes.front();
        }
        InputInfo::Ptr ii = inputsInfo[input.first];
        if (ii && ii->getPreProcess().getNumberOfChannels()) {
            _normalizePreprocMap[input.first].Load(outShape, ii);
        }
    }
}

void MKLDNNGraph::InitGraph() {
    MKLDNNGraphOptimizer optimizer;
    CPU_DEBUG_CAP_ENABLE(initNodeDumper(config.debugCaps));

    SortTopologically();
    InitNodes();

    optimizer.ApplyCommonGraphOptimizations(*this);
    SortTopologically();

    InitDescriptors();

    InitOptimalPrimitiveDescriptors();

    InitEdges();

    optimizer.ApplyImplSpecificGraphOptimizations(*this);
    SortTopologically();

    Allocate();

    CreatePrimitives();

#ifndef CPU_DEBUG_CAPS
    for (auto &graphNode : graphNodes) {
        graphNode->cleanup();
    }
#endif
    ExtractConstantAndExecutableNodes();

    ExecuteConstantNodesOnly();
}

void MKLDNNGraph::InitNodes() {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::InitNodes");
    for (auto &node : graphNodes) {
        node->init();
    }
}

void MKLDNNGraph::InitDescriptors() {
    OV_ITT_SCOPE_CHAIN(FIRST_INFERENCE, taskChain, MKLDNNPlugin::itt::domains::MKLDNN_LT, "InitDescriptors", "Prepare");

    for (auto &node : graphNodes) {
        if (node->getType() == Input && _normalizePreprocMap.find(node->getName()) != _normalizePreprocMap.end()) {
            auto *inputNode = dynamic_cast<MKLDNNInputNode *>(node.get());
            if (inputNode)
                inputNode->withMeanImage();
        }
        OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, node->profiling.getSupportedDescriptors);
        node->getSupportedDescriptors();

        OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, node->profiling.initSupportedPrimitiveDescriptors);
        node->initSupportedPrimitiveDescriptors();

        OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, node->profiling.filterSupportedPrimitiveDescriptors);
        node->filterSupportedPrimitiveDescriptors();
    }

    for (auto &node : graphNodes) {
        OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, node->profiling.selectOptimalPrimitiveDescriptor);
        node->selectOptimalPrimitiveDescriptor();
    }
}

void MKLDNNGraph::InitOptimalPrimitiveDescriptors() {
    OV_ITT_SCOPED_TASK(itt::domains::MKLDNNPlugin, "MKLDNNGraph::InitOptimalPrimitiveDescriptors");
    for (auto &node : graphNodes) {
        OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, node->profiling.initOptimalPrimitiveDescriptor);
        node->initOptimalPrimitiveDescriptor();
    }
}

void MKLDNNGraph::ExtractConstantAndExecutableNodes() {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::ExtractConstantAndExecutableNodes");
    for (const auto& graphNode : graphNodes) {
        if (graphNode->isConstant()) {
            constantGraphNodes.emplace_back(graphNode);
        } else if (CPU_DEBUG_CAPS_ALWAYS_TRUE(graphNode->isExecutable())) {
            /* @todo
             * Revise implementation.
             * With current way it is possible that with debug_caps enabled
             * we execute a node, which is not ready to be executed
             */
            executableGraphNodes.emplace_back(graphNode);
        }
    }
}

void MKLDNNGraph::ExecuteConstantNodesOnly() const {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::ExecuteConstantNodesOnly");
    mkldnn::stream stream(eng);

    using shared_memory_ptr = MKLDNNWeightsSharing::MKLDNNSharedMemory::Ptr;

    auto acquireSharedOutputs = [this](const MKLDNNNodePtr & node) {
        std::vector<shared_memory_ptr> outputs;
        bool hasLocalAllocatedEdges = false;
        bool hasExternalInvalidEdges = false;

        for (size_t i = 0; i < node->getChildEdges().size(); ++i) {
            auto edgePtr = node->getChildEdgeAt(i);
            if (edgePtr) {
                if (edgePtr->isUseExternalMemory()) {
                    auto ptr = weightsCache->get(edgePtr->name());
                    outputs.emplace_back(ptr);
                    if (!ptr->isValid())
                        hasExternalInvalidEdges = true;
                } else {
                    hasLocalAllocatedEdges = true;
                }
            }
        }

        return std::make_tuple(hasExternalInvalidEdges, hasLocalAllocatedEdges, outputs);
    };

    for (const auto &node : constantGraphNodes) {
        if (weightsCache) {
            auto sharedOutputs = acquireSharedOutputs(node);

            if (std::get<0>(sharedOutputs) || std::get<1>(sharedOutputs)) {
                ExecuteNode(node, stream);

                for (auto & output : std::get<2>(sharedOutputs))
                    output->valid(true);
            }
        } else {
            ExecuteNode(node, stream);
        }
    }
}

static bool isReorderAvailable(const MemoryDesc& parentDesc, const MemoryDesc& childDesc, const mkldnn::engine& eng) {
    memory::desc dstMemDesc = MemoryDescUtils::convertToDnnlMemoryDesc(childDesc.clone())->getDnnlDesc();
    memory::desc srcMemDesc = MemoryDescUtils::convertToDnnlMemoryDesc(parentDesc.clone())->getDnnlDesc();
    mkldnn::primitive_attr attr;

    dnnl_primitive_desc_t result = nullptr;
    auto status = dnnl_reorder_primitive_desc_create(&result, &srcMemDesc.data, eng.get(), &dstMemDesc.data, eng.get(),
                                                     attr.get());
    if (result) {
        mkldnn_primitive_desc_destroy(result);
    }

    return mkldnn_success == status;
}

void MKLDNNGraph::InitEdges() {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::InitEdges");

    size_t numberOfEdges = graphEdges.size();

    std::unordered_set<std::string> uniqueLayerNames;
    for (auto node : graphNodes) {
        uniqueLayerNames.insert(node->getName());
    }

    auto insertReorder = [&](MKLDNNEdgePtr& edge, bool isOptimized) {
        std::string basicLayerName = edge->getParent()->getName() + "_" +
                                     MKLDNNReorderNode::getReorderArgs(edge->getInputDesc(), edge->getOutputDesc()) + "_" +
                                     edge->getChild()->getName();
        std::string layerName = basicLayerName;
        int idx = 0;
        while (uniqueLayerNames.find(layerName) != uniqueLayerNames.end()) {
            idx++;
            layerName = basicLayerName + "_" + std::to_string(idx);
        }
        uniqueLayerNames.insert(layerName);

        // optimized flag indicate that just desc update w/o actual physical memory movement.
        InsertReorder(edge, layerName, edge->getInputDesc(), edge->getOutputDesc(), isOptimized);
    };

    auto updateEdge = [&](int& i) {
        graphEdges.erase(graphEdges.begin() + i);
        i--;
        numberOfEdges--;
    };

    for (auto i = 0; i < numberOfEdges; i++) {
        auto edge = graphEdges[i];
        auto reorderStatus = graphEdges[i]->needReorder();
        if (reorderStatus == MKLDNNEdge::ReorderStatus::Regular) {
            MKLDNNEdge::ReorderStatus reorderStatusInternal = MKLDNNEdge::ReorderStatus::Regular;
            // Check if there is a reorder that needs the precision conversion
            if (edge->getInputDesc().getPrecision() != edge->getOutputDesc().getPrecision() &&
                    !isReorderAvailable(edge->getInputDesc(), edge->getOutputDesc(), this->getEngine())) {
                // If we are here, then we need to insert Convert, because there are no reorders that support such type conversion
                const auto& inDesc = edge->getInputDesc();
                const auto& outDesc = edge->getOutputDesc();

                std::string convertName = edge->getParent()->getName() + "_" +
                                          inDesc.getPrecision().name() + "_" + outDesc.getPrecision().name();

                auto convertNode = std::make_shared<MKLDNNConvertNode>(inDesc.getShape(), inDesc.getPrecision(), outDesc.getPrecision(),
                                                                       convertName, this->getEngine(), this->weightsCache);
                convertNode->setDescs(inDesc, outDesc);
                InsertNode(edge, convertNode, true);

                //Check if reorder is still needed
                reorderStatusInternal = convertNode->getChildEdgeAt(0)->needReorder();
                if (reorderStatusInternal != MKLDNNEdge::ReorderStatus::No)
                    edge = convertNode->getChildEdgeAt(0);
            }
            if (reorderStatusInternal != MKLDNNEdge::ReorderStatus::No) {
                insertReorder(edge, reorderStatusInternal == MKLDNNEdge::ReorderStatus::Optimized);
            }
            updateEdge(i);
        } else if (reorderStatus == MKLDNNEdge::ReorderStatus::Optimized) {
            insertReorder(edge, true);
            updateEdge(i);
        }
    }
}

static inline bool isConstOutput(MKLDNNEdgePtr edge) {
    return edge->getParent()->isConstant() && !edge->getChild()->isConstant();
}

static edge_clusters_t findEdgeClusters(const std::vector<MKLDNNEdgePtr> & graphEdges) {
    typedef std::unordered_map<MKLDNNEdgePtr, size_t> edge_cluster_idx_map_t;

    edge_clusters_t edge_clusters;
    edge_cluster_idx_map_t edge_cluster_indices;

    for (auto &edge : graphEdges) {
        if (!edge->hasDefinedMaxSize())
            continue;

        auto edge_it = edge_cluster_indices.find(edge);

        if (edge_it != edge_cluster_indices.end())
            continue;   // edge is visited

        size_t cluster_idx = edge_clusters.size();
        MKLDNNEdgePtr last_shared_edge = nullptr;

        // find cluster index
        for (auto shared_edge = edge->getSharedEdge(std::nothrow);
            shared_edge;
            shared_edge = shared_edge->getSharedEdge(std::nothrow)) {
            auto shared_edge_it = edge_cluster_indices.find(shared_edge);
            if (shared_edge_it != edge_cluster_indices.end()) {
                cluster_idx = shared_edge_it->second;
                last_shared_edge = shared_edge;
                break;
            }
        }

        // add shared edges to cluster
        edge_cluster_indices.emplace(edge, cluster_idx);

        if (cluster_idx == edge_clusters.size())
            edge_clusters.emplace_back(edge_cluster_t { edge });
        else
            edge_clusters[cluster_idx].emplace(edge);

        for (auto shared_edge = edge->getSharedEdge(std::nothrow);
            shared_edge != last_shared_edge;
            shared_edge = shared_edge->getSharedEdge(std::nothrow)) {
            edge_cluster_indices.emplace(shared_edge, cluster_idx);
            edge_clusters[cluster_idx].emplace(shared_edge);
        }
    }

    return edge_clusters;
}

void MKLDNNGraph::AllocateWithReuse() {
    edge_clusters_t edge_clusters = findEdgeClusters(graphEdges);

    size_t edge_clusters_count = edge_clusters.size();

    for (size_t i = 0; i < edge_clusters_count;) {
        auto &cluster = edge_clusters[i];
        bool erase = false;
        for (auto &edge : cluster) {
            if (edge->getStatus() == MKLDNNEdge::Status::NeedAllocation
                && edge->getParent()->isConstant()) {
                if (edge->getParent()->getType() == Input) {
                    auto constNode = std::static_pointer_cast<MKLDNNInputNode>(edge->getParent());
                    edge->reuse(std::const_pointer_cast<MKLDNNMemory>(constNode->getMemoryPtr()));
                } else {
                    edge->externalAllocate(weightsCache);
                }
                erase = true;
            }
        }

        if (erase) {
            std::swap(edge_clusters[i], edge_clusters[edge_clusters_count - 1]);
            --edge_clusters_count;
        } else {
            ++i;
        }
    }

    edge_clusters.resize(edge_clusters_count);

    const int64_t alignment = 32;  // 32 bytes

    std::vector<MemorySolver::Box> boxes(edge_clusters.size());
    for (int i = 0; i < edge_clusters.size(); i++) {
        MemorySolver::Box &box = boxes[i];
        box = { std::numeric_limits<int>::max(), 0, 0, i };
        for (auto &edge : edge_clusters[i]) {
            int e_start = edge->getParent()->execIndex;
            int e_finish = edge->getChild()->execIndex;

            if (!edge->hasDefinedMaxSize()) {
                IE_THROW() << "Can not allocate memory since the size is undefined.";
            }

            int64_t e_size = edge->getDesc().getMaxMemSize();  // size in bytes (from the beginning of data to the last element)
            box.start = std::min(e_start, box.start);
            box.finish = std::max(e_finish, box.finish);
            box.size =  std::max(e_size, box.size);
        }

        // Constant data are filled once on load.
        // So we need it untouchable during all execution time
        // -1 is a place holder for a max timestamp.
        bool isConst = false, isOutput = false, isInput = false;
        for (auto &edge : edge_clusters[i]) {
            isConst  |= isConstOutput(edge);
            isOutput |= edge->getChild()->getType() == Output;
            isInput  |= edge->getParent()->getType() == Input;
        }

        if (reuse_io_tensors) {
            if (isInput | isConst) box.start = 0;
            if (isOutput | isConst) box.finish = -1;
        } else {
            if (isInput  | isOutput | isConst) {
                box.start = 0;
                box.finish = -1;
            }
        }

        box.size = div_up(box.size, alignment);
    }

    MemorySolver memSolver(boxes);
    size_t total_size = static_cast<size_t>(memSolver.solve()) * alignment;

    memWorkspace = std::make_shared<MKLDNNMemory>(eng);
    memWorkspace->Create(DnnlBlockedMemoryDesc(InferenceEngine::Precision::I8, Shape(InferenceEngine::SizeVector{total_size})));

    if (edge_clusters.empty())
        return;

    auto* workspace_ptr = static_cast<int8_t*>(memWorkspace->GetData());

    for (int i = 0; i < edge_clusters.size(); i++) {
        int count = 0;
        for (auto &edge : edge_clusters[i]) {
            if (edge->getStatus() == MKLDNNEdge::Status::NeedAllocation) {
                int64_t offset = memSolver.getOffset(i);
                // !! Fallback to individual memory allocation !!
                // if you like to check infer without reuse just call this function without arguments.
                edge->allocate(workspace_ptr + offset * alignment);  // alignment in byte

                // TODO: WA for some test (like strided_slice_test) which use tensors with
                //       shapes {0}. And it is implisitly converted into {1} tensor.
                //       Zeroing of input data allow pass tests.
                if (edge->getParent()->type == Input && edge->hasDefinedMaxSize())
                    edge->getMemoryPtr()->FillZero();

                count++;
            }
        }
        IE_ASSERT(count == 1);
    }
}

void MKLDNNGraph::Allocate() {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::Allocate");

    // resolve edges. Define which will be a view on others
    //   NeedAllocation - real blob
    //   NotAllocated - view on other blob, peer or in-place
    for (auto& edge : graphEdges) edge->init();

    // Allocate memory space for all edges marked with NeedAllocation
    AllocateWithReuse();

    // Resolve all other edges with status NotAllocated and in-place
    for (auto& node : graphNodes) node->resolveInPlaceEdges();

    // Create dummy memory with undefined desc for edges that are not allocated on the previous stages (memory solver and inPlace resolving)
    for (auto& edge : graphEdges) edge->allocate();

    // Check all getters. Should work.
    for (auto& edge : graphEdges) edge->validate();
}

void MKLDNNGraph::CreatePrimitives() {
    OV_ITT_SCOPED_TASK(itt::domains::MKLDNNPlugin, "MKLDNNGraph::CreatePrimitives");
    for (auto& node : graphNodes) {
        OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, node->profiling.createPrimitive);
        node->createPrimitive();
    }
}

void MKLDNNGraph::PushInputData(const std::string& name, const InferenceEngine::Blob::Ptr &in) {
    if (!IsReady()) IE_THROW()<< "Wrong state. Topology not ready.";

    auto input = inputNodesMap.find(name);
    if (input != inputNodesMap.end()) {
        auto& inTensorDesc = in->getTensorDesc();
        auto node = input->second;
        auto childEdge = node->getChildEdgeAt(0);
        const auto& outDims = node->getOutputShapeAtPort(0);

        const void *ext_data_ptr = in->cbuffer();
        void *inter_data_ptr = childEdge->getMemory().GetData();

        if (ext_data_ptr != inter_data_ptr) {
            auto ext_tdesc = MemoryDescUtils::convertToDnnlBlockedMemoryDesc(in->getTensorDesc());

            auto ext_mem = MKLDNNMemory(eng);
            ext_mem.Create(ext_tdesc, ext_data_ptr, false);

            childEdge->getMemory().SetData(ext_mem, 0, false);
        }

        // todo: make sure 'name' exists in this map...
        if (_normalizePreprocMap.find(name) != _normalizePreprocMap.end()) {
            if (inTensorDesc.getPrecision() == InferenceEngine::Precision::FP32) {
                _normalizePreprocMap[name].NormalizeImage(outDims, reinterpret_cast<float *>(inter_data_ptr),
                                                          inTensorDesc.getLayout());
            } else {
                IE_THROW() << "Mean image of type " << inTensorDesc.getPrecision().name() << " is unsupported";
            }
        }
    } else {
        IE_THROW() << "Input blob for infer '" << name << "' doesn't correspond to input in network";
    }
}

void MKLDNNGraph::PullOutputData(BlobMap &out) {
    if (!IsReady())
        IE_THROW() << "Wrong state. Topology not ready.";

    for (auto &outputMap : outputNodesMap) {
        auto name = outputMap.first;
        auto node = outputMap.second;
        auto parentEdge = node->getParentEdgeAt(0);
        const MKLDNNMemory& intr_blob = parentEdge->getMemory();

        const auto ext_blob_map = out.find(name);
        const auto ext_blob = ext_blob_map->second;
        if (ext_blob_map == out.end()) {
            IE_THROW(Unexpected) << "The network outputs do not contain mkldnn graph output node name: \"" << name << "\"";
        }

        const auto actualDesc = MemoryDescUtils::convertToTensorDesc(intr_blob.getDesc());
        auto &expectedDesc = ext_blob->getTensorDesc();

        // TODO [NM]: need to create universal reorder which will be detect cases when we really need to use it
        // WA: for cases when output shape after transformation will be 1x1x1x1 but model output is scalar
        bool isScalarOutput = false;
        if (actualDesc.getLayout() == SCALAR) {
            isScalarOutput = expectedDesc.getLayout() == SCALAR ||
                             (!expectedDesc.getDims().empty() &&
                             std::accumulate(expectedDesc.getDims().begin(), expectedDesc.getDims().end(), (size_t)1, std::multiplies<size_t>()) == 1);
        } else if (expectedDesc.getLayout() == SCALAR) {
            isScalarOutput = actualDesc.getLayout() == SCALAR ||
                             (!actualDesc.getDims().empty() &&
                             std::accumulate(actualDesc.getDims().begin(), actualDesc.getDims().end(), (size_t)1, std::multiplies<size_t>()) == 1);
        }

        const auto &outDims = intr_blob.getStaticDims();
        if (out[name]->getTensorDesc().getDims() != outDims && !isScalarOutput) {
            // WA: because input/output info initially contains non empty dims, order etc.
            // and setDims (called inside setShape) can't correct modify blocked desc for desc with blocked layout
            if (expectedDesc.getLayout() == Layout::BLOCKED) {
                expectedDesc = TensorDesc(expectedDesc.getPrecision(), expectedDesc.getLayout());
            }
            out[name]->setShape(outDims);
        }

        // check for empty output blob
        if (std::any_of(outDims.begin(), outDims.end(), [](const Dim dim) {return dim == 0;})) {
            continue;
        }

        auto srcPrec = actualDesc.getPrecision();
        auto dstPrec = expectedDesc.getPrecision();

        if (srcPrec == dstPrec && ext_blob->byteSize() != intr_blob.GetSize())
                IE_THROW() << "Output blob byte size is not equal network output byte size ("
                                   << ext_blob->byteSize() << "!=" << intr_blob.GetSize() << ").";

        void *ext_blob_ptr = ext_blob->buffer();
        void *intr_blob_ptr = intr_blob.GetData();

        // That is the same memory. No need to copy
        if (ext_blob_ptr == intr_blob_ptr) continue;

        if (actualDesc.getBlockingDesc() != expectedDesc.getBlockingDesc() && !isScalarOutput) {
            auto outBlobDesc = MemoryDescUtils::convertToDnnlBlockedMemoryDesc(expectedDesc);
            auto outBloMem = MKLDNNMemory(eng);
            outBloMem.Create(outBlobDesc, ext_blob_ptr, false);

            outBloMem.SetData(intr_blob, 0, false);
        } else {
            size_t size_to_copy = intr_blob.GetDescWithType<BlockedMemoryDesc>()->getPaddedElementsCount();
            // TODO: Should we support InferenceEngine::PluginConfigParams::KEY_DYN_BATCH_LIMIT???
            // TODO [DS]: phase 2: should we support this behaviour? Looks obsolete in the dynamic shapes paradigm
            if (config.batchLimit) {
                if (node->isDynamicNode()) {
                    IE_THROW(NotImplemented) << "[DS] not implemented dynamic batch for node with dynamic shape";
                }
                int MB_to_process = node->batchToProcess();
                size_to_copy = std::accumulate(outDims.begin() + 1, outDims.end(), (size_t)1, std::multiplies<size_t>()) * MB_to_process;
            }

            cpu_convert(intr_blob_ptr, ext_blob_ptr, srcPrec, dstPrec, size_to_copy);
        }
    }
}

inline void MKLDNNGraph::ExecuteNode(const MKLDNNNodePtr& node, const mkldnn::stream& stream) const {
    DUMP(node, infer_count);
    OV_ITT_SCOPED_TASK(itt::domains::MKLDNNPlugin, node->profiling.execute);

    if (node->isDynamicNode()) {
        node->executeDynamic(stream);
    } else {
        node->execute(stream);
    }
}

void MKLDNNGraph::Infer(MKLDNNInferRequest* request, int batch) {
    if (!IsReady()) {
        IE_THROW() << "Wrong state. Topology is not ready.";
    }

    mkldnn::stream stream(eng);

    for (const auto& node : executableGraphNodes) {
        VERBOSE(node, config.debugCaps.verbose);
        PERF(node, config.collectPerfCounters);

        if (request)
            request->ThrowIfCanceled();
        ExecuteNode(node, stream);
    }

    if (infer_count != -1) infer_count++;
}

void MKLDNNGraph::VisitNode(MKLDNNNodePtr node, std::vector<MKLDNNNodePtr>& sortedNodes) {
    if (node->temporary) {
        return;
    }

    if (node->permanent) {
        return;
    }

    node->temporary = true;

    for (size_t i = 0; i < node->getChildEdges().size(); i++) {
        VisitNode(node->getChildEdgeAt(i)->getChild(), sortedNodes);
    }

    node->permanent = true;
    node->temporary = false;

    sortedNodes.insert(sortedNodes.begin(), node);
}

void MKLDNNGraph::SortTopologically() {
    OV_ITT_SCOPE(FIRST_INFERENCE, itt::domains::MKLDNN_LT, "MKLDNNGraph::SortTopologically");

    std::vector<MKLDNNNodePtr> unsorted;
    std::vector<MKLDNNNodePtr> sorted;

    for (int i = 0; i < graphNodes.size(); i++) {
        MKLDNNNodePtr node = graphNodes[i];

        node->permanent = false;
        node->temporary = false;

        unsorted.push_back(node);
    }

    while (!unsorted.empty()) {
        MKLDNNNodePtr node = unsorted.at(0);
        unsorted.erase(unsorted.begin());

        VisitNode(node, sorted);
    }

    for (int i = 0; i < sorted.size(); i++) sorted[i]->execIndex = i;

    graphNodes.erase(graphNodes.begin(), graphNodes.end());
    graphNodes.assign(sorted.begin(), sorted.end());

    // TODO: Sort in/out edges by port index because of backward compatibility
    //       A lot of plugin logic are build on top of assumption that index in
    //       vector childEdges/parentEdges is port number. But that is not
    //       truth anymore. But to keep old logic correct need to simulate ordering.
    //
    // Make first N (N == port_num) edge indexes are matched with port index
    for (auto &node : graphNodes) {
        {
            int port_num = node->inputShapes.size();
            std::vector<MKLDNNEdgePtr> res(port_num);

            for (int i = 0; i < node->parentEdges.size(); i++) {
                auto edge = node->getParentEdgeAt(i);
                int port = edge->getOutputNum();
                if (port < port_num && !res[port])
                    res[port] = edge;
                else
                    res.push_back(edge);
            }
            node->parentEdges = {res.begin(), res.end()};
        }
        {
            int port_num = node->outputShapes.size();
            std::vector<MKLDNNEdgePtr> res(port_num);

            for (int i = 0; i < node->childEdges.size(); i++) {
                auto edge = node->getChildEdgeAt(i);
                int port = edge->getInputNum();
                if (port < port_num && !res[port])
                    res[port] = edge;
                else
                    res.push_back(edge);
            }
            node->childEdges = {res.begin(), res.end()};
        }
    }
}

void MKLDNNGraph::GetPerfData(std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> &perfMap) const {
    unsigned i = 0;
    std::function<void(std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> &, const MKLDNNNodePtr&)>
            getPerfMapFor = [&](std::map<std::string, InferenceEngine::InferenceEngineProfileInfo> &perfMap, const MKLDNNNodePtr& node) {
        InferenceEngine::InferenceEngineProfileInfo &pc = perfMap[node->getName()];
        pc.execution_index = i++;
        // TODO: Why time counter is signed?
        pc.cpu_uSec = pc.realTime_uSec = (long long) node->PerfCounter().avg();
        pc.status = pc.cpu_uSec > 0 ? InferenceEngine::InferenceEngineProfileInfo::EXECUTED
                                    : InferenceEngine::InferenceEngineProfileInfo::NOT_RUN;
        std::string pdType = node->getPrimitiveDescriptorType();
        size_t typeLen = sizeof(pc.exec_type) / sizeof(pc.exec_type[0]);
        pdType.copy(pc.exec_type, typeLen, 0);
        size_t layerTypeLen = sizeof(pc.layer_type) / sizeof(pc.layer_type[0]);
        node->typeStr.copy(pc.layer_type, layerTypeLen, 0);

        for (auto& fusedNode : node->fusedWith) {
            getPerfMapFor(perfMap, fusedNode);
        }

        for (auto& mergedWith : node->mergedWith) {
            getPerfMapFor(perfMap, mergedWith);
        }
    };

    for (int i = 0; i < graphNodes.size(); i++) {
        getPerfMapFor(perfMap, graphNodes[i]);
    }
}

void MKLDNNGraph::setConfig(const Config &cfg) {
    config = cfg;
}

const Config& MKLDNNGraph::getConfig() const {
    return config;
}

void MKLDNNGraph::setProperty(const std::map<std::string, std::string>& properties) {
    config.readProperties(properties);
}

Config MKLDNNGraph::getProperty() const {
    return config;
}

void MKLDNNGraph::RemoveEdge(MKLDNNEdgePtr& edge) {
    for (auto it = graphEdges.begin(); it != graphEdges.end(); it++) {
        if ((*it) == edge) {
            graphEdges.erase(it);
            return;
        }
    }
}

void MKLDNNGraph::DropNode(const MKLDNNNodePtr &node) {
    auto children = node->childEdges;
    auto parents = node->parentEdges;

    for (size_t i = 0; i < parents.size(); i++) {
        auto p_edge = parents[i].lock();
        if (!p_edge) continue;
        auto parent = p_edge->getParent();
        if (!parent) continue;

        for (size_t j = 0; j < children.size(); j++) {
            if (!children[j].lock())
                continue;
            auto child = children[j].lock()->getChild();
            if (!child)
                continue;

            MKLDNNEdgePtr &remEdge = p_edge;
            int inNum = 0;
            if (remEdge) {
                inNum = remEdge->getInputNum();
                remEdge->drop();
                RemoveEdge(remEdge);
            }
            remEdge = children[j].lock();
            int outNum = 0;
            if (remEdge) {
                outNum = remEdge->getOutputNum();
                remEdge->drop();
                RemoveEdge(remEdge);
            }
            MKLDNNEdgePtr newEdge(new MKLDNNEdge(parent, child, inNum, outNum));
            graphEdges.push_back(newEdge);
            parent->addEdge(newEdge);
        }
    }
}

void MKLDNNGraph::DropDWConvNode(const MKLDNNNodePtr &node) {
    auto children = node->childEdges;
    auto parents = node->parentEdges;

    auto parentConvEdge = parents[0].lock();
    if (!parentConvEdge) return;
    auto parentConv = parentConvEdge->getParent();
    if (!parentConv) return;

    parentConv->outputShapes[0] = node->outputShapes[0];

    for (size_t i = 0; i < 1; i++) {
        auto p_edge = parents[i].lock();
        if (!p_edge) continue;
        auto parent = p_edge->getParent();
        if (!parent) continue;

        for (size_t j = 0; j < children.size(); j++) {
            if (!children[j].lock())
                continue;
            auto child = children[j].lock()->getChild();
            if (!child)
                continue;

            MKLDNNEdgePtr &remEdge = p_edge;
            int inNum = 0;
            if (remEdge) {
                inNum = remEdge->getInputNum();
                remEdge->drop();
                RemoveEdge(remEdge);
            }
            remEdge = children[j].lock();
            int outNum = 0;
            if (remEdge) {
                outNum = remEdge->getOutputNum();
                remEdge->drop();
                RemoveEdge(remEdge);
            }
            MKLDNNEdgePtr newEdge(new MKLDNNEdge(parent, child, inNum, outNum));
            graphEdges.push_back(newEdge);
            parent->addEdge(newEdge);
        }
    }

    for (size_t i = 1; i < parents.size(); i++) {
        auto p_edge = parents[i].lock();
        if (!p_edge) continue;
        auto parent = p_edge->getParent();
        if (!parent) continue;

        MKLDNNEdgePtr &remEdge = p_edge;
        const auto portCandidate = remEdge->getOutputNum();
        int inNum = 0;
        if (remEdge) {
            inNum = remEdge->getInputNum();
            remEdge->drop();
            RemoveEdge(remEdge);
        }
        int outNum = parentConv->parentEdges.size();

        MKLDNNEdgePtr newEdge(new MKLDNNEdge(parent, parentConv, inNum, outNum));
        graphEdges.push_back(newEdge);
        parent->addEdge(newEdge);
        parentConv->inputShapes.push_back(node->getInputShapeAtPort(portCandidate));
    }
    parentConv->outputShapes[0] = node->getOutputShapeAtPort(0);
}

void MKLDNNGraph::RemoveDroppedNodes() {
    auto& nodes = this->GetNodes();

    auto it = nodes.begin();

    while (it != nodes.end()) {
        if ((*it)->isDropped()) {
            it = nodes.erase(it);
        } else {
            it++;
        }
    }
}

void MKLDNNGraph::RemoveDroppedEdges() {
    auto& edges = this->GetEdges();

    auto it = edges.begin();

    while (it != edges.end()) {
        if ((*it)->isDropped()) {
            it = edges.erase(it);
        } else {
            it++;
        }
    }
}

MKLDNNNodePtr MKLDNNGraph::InsertReorder(MKLDNNEdgePtr edge, std::string layerName, const MemoryDesc& inDesc, const MemoryDesc& outDesc,
                                         bool isOptimized) {
    MKLDNNNodePtr newReorder(new MKLDNNReorderNode(layerName, getEngine(), weightsCache));
    auto *reorderPtr = dynamic_cast<MKLDNNReorderNode *>(newReorder.get());
    if (reorderPtr == nullptr) {
        IE_THROW() << "MKLDNNGraph::InsertReorder: Cannot cast to MKLDNNReorderNode";
    }
    reorderPtr->setDescs(inDesc, outDesc);
    reorderPtr->setOptimized(isOptimized);

    InsertNode(edge, newReorder, true);

    // Using the method MKLDNNEdge::getDesc() we can check that input and output tensor descriptors are equal.
    // Due to the specificity of MKLDNNGraphOptimizer::MergeTransposeAndReorder() that isOptimized flag uses, we shouldn't do these checks.
    if (!isOptimized) {
        newReorder->getParentEdgeAt(0)->getDesc();
        newReorder->getChildEdgeAt(0)->getDesc();
    }

    return newReorder;
}

bool MKLDNNGraph::InsertNode(MKLDNNEdgePtr edge, MKLDNNNodePtr node, bool initNode) {
    auto oIndex = edge->getOutputNum();
    auto iIndex = edge->getInputNum();
    if (iIndex < 0 || oIndex < 0)
        IE_THROW() << "Cannot insert node '" << node->getName() << "' between nodes: "
                           << edge->getParent()->getName() << " and "
                           << edge->getChild()->getName() << ".";

    edge->drop();

    return InsertNode(edge->getParent(), edge->getChild(), node, iIndex, oIndex, initNode);
}

bool MKLDNNGraph::InsertNode(MKLDNNNodePtr parent, MKLDNNNodePtr child, MKLDNNNodePtr node, int parentPort, int childPort, bool initNode) {
    MKLDNNEdgePtr beforeNode(new MKLDNNEdge(parent, node, parentPort, 0));
    MKLDNNEdgePtr afterNode(new MKLDNNEdge(node, child, 0, childPort));

    // Add edge for beforeNode
    beforeNode->getChild()->parentEdges.push_back(beforeNode);
    parent->childEdges.push_back(beforeNode);

    // Add edge for afterNode
    afterNode->getParent()->childEdges.push_back(afterNode);
    child->parentEdges.push_back(afterNode);

    if (isQuantized()) {
        node->setQuantizedGraphFlag(true);
    }

    if (initNode) {
        node->getSupportedDescriptors();
        node->initSupportedPrimitiveDescriptors();
        node->filterSupportedPrimitiveDescriptors();
        node->selectOptimalPrimitiveDescriptor();
        node->initOptimalPrimitiveDescriptor();
    }

    graphEdges.push_back(beforeNode);
    graphEdges.push_back(afterNode);
    graphNodes.push_back(node);
    return true;
}

// Set all non const data paths precision to BF16
void MKLDNNGraph::EnforceBF16() {
    // Floating point parts of FP32 + INT8 or FP32 + BIN mixed precision models will be executed in BF16 precision
    // only if enforceBF16 flag was set manually because current performance is not good enough to enable it by default
    if (!implication(isQuantized(), config.manualEnforceBF16))
        return;
    /* list of node types that must be forced to be executed in BF16 precision
     * because of performance gains */
    static const std::unordered_set<Type, std::hash<int>> significantNodes { // std::hash<int> is necessary old compilers (defect in C++11 standart)
        Convolution,    // conv nets
        FullyConnected, // conv / bert nets
        RNNCell,        // recurent nets
        RNNSeq,         // recurent nets
        MatMul,         // bert nets
        ROIPooling,     // object detection nets
        Interpolate,    // super resolution nets
    };

    std::function<void(const MKLDNNNodePtr&, std::unordered_set<MKLDNNNodePtr>& skipNodes)> searchForNodesToSkip;
    searchForNodesToSkip = [&](const MKLDNNNodePtr& node, std::unordered_set<MKLDNNNodePtr>& skipNodes) -> void {
        for (size_t i = 0; i < node->getParentEdges().size(); i++) {
            const auto& parent = node->getParentEdgeAt(i)->getParent();
            if (significantNodes.count(parent->getType())) // stop at significant nodes
                continue;

            const auto res = skipNodes.insert(parent);
            if (res.second) // node not visited yet
                searchForNodesToSkip(parent, skipNodes);
        }
    };

    /* Skip BF16 enforcement for tail of the graph by forming set of nodes to skip.
     * Necessary to maintain accuracy.
     * Experiments show zero peformance impact on average */
    std::unordered_set<MKLDNNNodePtr> nodesToSkip;
    // starting from output nodes
    for (const auto& entry : outputNodesMap) {
        const auto& node = entry.second;
        searchForNodesToSkip(node, nodesToSkip);
    }

    for (const auto& node : graphNodes) {
        if (nodesToSkip.count(node) && !node->enforceBF16evenForGraphTail)
            continue;

        if (node->getType() != Input && node->getType() != Output) {
            for (size_t i = 0; i < node->getOriginalInputsNumber(); i++) {
                const auto &parent = node->getParentEdgesAtPort(i)[0]->getParent();
                /* Skip BF16 enforcement for nodes after Constant Inputs for maintaining precision for fusing.
                 * Precision conversion to BF16 does automatically, if convolution follows up after Constant Inputs
                 * and if activation is BF16 */
                if (!(parent->getType() == Input && parent->isConstant() &&
                      node->getType() != Concatenation) && // Concatenation node is exception because it doesn't change an accuracy for BF16 activation
                    !(parent->getType() == Input && node->getType() == Eltwise) && // exclude Eltwise after Input since it supports conversion to BF16
                    node->getOriginalInputPrecisionAtPort(i) == Precision::FP32)
                    node->setOriginalInputPrecisionAtPort(i, Precision::BF16);
            }

            for (size_t i = 0; i < node->getOriginalOutputsNumber(); i++) {
                if (node->getOriginalOutputPrecisionAtPort(i) == Precision::FP32)
                    node->setOriginalOutputPrecisionAtPort(i, Precision::BF16);
            }
        }
    }
}

std::shared_ptr<ngraph::Function> MKLDNNGraph::dump() const {
    return dump_graph_as_ie_ngraph_net(*this);
}
