/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include "ngraph_utils.h"
#include "tf_graphcycles.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/attr_value_util.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/platform/default/logging.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/util/device_name_utils.h"

using namespace std;
namespace ngraph_bridge {

class NGraphClusterPass : public tensorflow::GraphOptimizationPass {
public:
  tf::Status Run(const tf::GraphOptimizationPassOptions &options) {
    // TODO(amprocte): Remove this when we have proper support for graphs with
    // cycles.
    if (std::getenv("NGRAPH_TF_SKIP_CLUSTERING") != nullptr) {
      VLOG(0) << "NGRAPH_TF_SKIP_CLUSTERING is set. Skipping clustering step.";
      return tf::Status::OK();
    }

    tf::Graph *graph = options.graph->get();

    TF_RETURN_IF_ERROR(IdentifyClusters(graph));

    return tf::Status::OK();
  }

private:
  // TODO(amprocte): do we need to look at job name, replica, task?
  bool IsNGraphNode(const tf::Node *node) {
    tf::DeviceNameUtils::ParsedName parsed;

    if (!tf::DeviceNameUtils::ParseFullName(node->def().device(), &parsed)) {
      return false;
    }

    // TODO(amprocte): change to DEVICE_NGRAPH constant
    return (parsed.has_type && parsed.type == "NGRAPH_CPU");
  }

  struct Cluster {
    int index;
    std::set<tf::Node *> nodes;
  };

  tf::Status IdentifyClusters(tf::Graph *graph) {
    std::map<tf::Node *, std::shared_ptr<Cluster>> cluster_map;

    tf::GraphCycles gc;

    for (auto node : graph->op_nodes()) {
      int new_index = gc.NewNode();
      cluster_map[node] = std::make_shared<Cluster>();
      cluster_map[node]->index = new_index;
      cluster_map[node]->nodes.insert(node);
    }

    for (auto edge : graph->edges()) {
      tf::Node *src = edge->src();
      tf::Node *dst = edge->dst();

      // Skip source/sink
      if (!src->IsOp() || !dst->IsOp()) {
        continue;
      }

      if (!gc.InsertEdge(cluster_map[src]->index, cluster_map[dst]->index)) {
        return tf::errors::InvalidArgument(
            "Input graph has a cycle (inserting an edge from ",
            src->DebugString(), " to ", dst->DebugString(),
            " would create a cycle)");
      }
    }

    bool changed;

    do {
      changed = false;

      for (auto edge : graph->edges()) {
        tf::Node *src = edge->src();
        tf::Node *dst = edge->dst();

        if (!IsNGraphNode(src) || !IsNGraphNode(dst)) {
          continue;
        }

        int src_index = cluster_map[src]->index;
        int dst_index = cluster_map[dst]->index;

        if (gc.HasEdge(src_index, dst_index) &&
            gc.ContractEdge(src_index, dst_index)) {
          for (auto node : cluster_map[dst]->nodes) {
            cluster_map[src]->nodes.insert(node);
            cluster_map[node] = cluster_map[src];
          }
          changed = true;
        }
      }
    } while (changed);

    std::set<Cluster *> seen;
    int cluster_count = 0;

    for (auto kv : cluster_map) {
      auto cluster = kv.second.get();
      bool has_ngraph_ops = false;

      for (auto node : cluster->nodes) {
        if (IsNGraphNode(node)) {
          has_ngraph_ops = true;
          break;
        }
      }

      if (!has_ngraph_ops) {
        continue;
      }

      if (seen.count(cluster) == 0) {
        seen.insert(cluster);
        VLOG(0) << "cluster " << cluster_count << ": " << cluster->nodes.size()
                << " nodes";

        for (auto node : cluster->nodes) {
          if (!IsNGraphNode(node)) {
            return tf::errors::InvalidArgument(
                "Node ", node->DebugString(),
                " is not an nGraph node but was placed in an nGraph cluster.");
          }

          VLOG(0) << ">> cluster " << cluster_count << ": " << node
                  << " :: " << node->name() << " [" << node->type_string()
                  << "]";

          node->AddAttr("_ngraph_cluster", cluster_count);
        }
        cluster_count++;
      }
    }

    VLOG(0) << cluster_count << " total clusters";

    return tf::Status::OK();
  }
};
} // namespace ngraph_bridge

namespace tensorflow {
REGISTER_OPTIMIZATION(OptimizationPassRegistry::POST_REWRITE_FOR_EXEC, 105,
                      ngraph_bridge::NGraphClusterPass);
} // namespace tensorflow
