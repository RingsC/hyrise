#include "join_graph_edge.hpp"

#include <memory>
#include <ostream>
#include <vector>

#include "expression/abstract_expression.hpp"

namespace hyrise {

JoinGraphEdge::JoinGraphEdge(const JoinGraphVertexSet& init_vertex_set,
                             const std::vector<std::shared_ptr<AbstractExpression>>& init_predicates)
    : vertex_set(init_vertex_set), predicates(init_predicates) {}

std::ostream& operator<<(std::ostream& stream, const JoinGraphEdge& join_graph_edge) {
  stream << "Vertices: " << join_graph_edge.vertex_set << "; " << join_graph_edge.predicates.size() << " predicates\n";
  for (const auto& predicate : join_graph_edge.predicates) {
    stream << predicate->as_column_name() << '\n';
  }
  return stream;
}

}  // namespace hyrise
