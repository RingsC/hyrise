#include "prepared_plan.hpp"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container_hash/hash.hpp>

#include "expression/expression_utils.hpp"
#include "expression/lqp_subquery_expression.hpp"
#include "expression/placeholder_expression.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"
#include "logical_query_plan/lqp_utils.hpp"
#include "types.hpp"
#include "utils/assert.hpp"

namespace {

using namespace hyrise;  // NOLINT

void lqp_bind_placeholders_impl(const std::shared_ptr<AbstractLQPNode>& lqp,
                                const std::unordered_map<ParameterID, std::shared_ptr<AbstractExpression>>& parameters,
                                std::unordered_set<std::shared_ptr<AbstractLQPNode>>& visited_nodes);

void expression_bind_placeholders_impl(
    std::shared_ptr<AbstractExpression>& expression,
    const std::unordered_map<ParameterID, std::shared_ptr<AbstractExpression>>& parameters,
    std::unordered_set<std::shared_ptr<AbstractLQPNode>>& visited_nodes) {
  visit_expression(expression, [&](auto& sub_expression) {
    if (const auto placeholder_expression = std::dynamic_pointer_cast<PlaceholderExpression>(sub_expression)) {
      const auto parameter_iter = parameters.find(placeholder_expression->parameter_id);
      Assert(parameter_iter != parameters.end(),
             "No expression specified for ValuePlaceholder. This should have been caught earlier");
      sub_expression = parameter_iter->second;

      return ExpressionVisitation::DoNotVisitArguments;
    }

    if (const auto subquery_expression = std::dynamic_pointer_cast<LQPSubqueryExpression>(sub_expression)) {
      lqp_bind_placeholders_impl(subquery_expression->lqp, parameters, visited_nodes);
    }

    return ExpressionVisitation::VisitArguments;
  });
}

void lqp_bind_placeholders_impl(const std::shared_ptr<AbstractLQPNode>& lqp,
                                const std::unordered_map<ParameterID, std::shared_ptr<AbstractExpression>>& parameters,
                                std::unordered_set<std::shared_ptr<AbstractLQPNode>>& visited_nodes) {
  visit_lqp(lqp, [&](const auto& node) {
    if (!visited_nodes.emplace(node).second) {
      return LQPVisitation::DoNotVisitInputs;
    }

    for (auto& expression : node->node_expressions) {
      expression_bind_placeholders_impl(expression, parameters, visited_nodes);
    }

    return LQPVisitation::VisitInputs;
  });
}

}  // namespace

namespace hyrise {

PreparedPlan::PreparedPlan(const std::shared_ptr<AbstractLQPNode>& init_lqp,
                           const std::vector<ParameterID>& init_parameter_ids)
    : lqp(init_lqp), parameter_ids(init_parameter_ids) {}

std::shared_ptr<PreparedPlan> PreparedPlan::deep_copy() const {
  const auto lqp_copy = lqp->deep_copy();
  return std::make_shared<PreparedPlan>(lqp_copy, parameter_ids);
}

size_t PreparedPlan::hash() const {
  auto hash = size_t{0};
  boost::hash_combine(hash, lqp->hash());
  for (const auto& parameter_id : parameter_ids) {
    boost::hash_combine(hash, parameter_id);
  }
  return hash;
}

std::shared_ptr<AbstractLQPNode> PreparedPlan::instantiate(
    const std::vector<std::shared_ptr<AbstractExpression>>& parameters) const {
  Assert(parameters.size() == parameter_ids.size(), std::string("Incorrect number of parameters supplied - expected ") +
                                                        std::to_string(parameter_ids.size()) + " got " +
                                                        std::to_string(parameters.size()));

  auto parameters_by_id = std::unordered_map<ParameterID, std::shared_ptr<AbstractExpression>>{};
  for (auto parameter_idx = size_t{0}; parameter_idx < parameters.size(); ++parameter_idx) {
    parameters_by_id.emplace(parameter_ids[parameter_idx], parameters[parameter_idx]);
  }

  auto instantiated_lqp = lqp->deep_copy();

  auto visited_nodes = std::unordered_set<std::shared_ptr<AbstractLQPNode>>{};
  lqp_bind_placeholders_impl(instantiated_lqp, parameters_by_id, visited_nodes);

  return instantiated_lqp;
}

bool PreparedPlan::operator==(const PreparedPlan& rhs) const {
  return *lqp == *rhs.lqp && parameter_ids == rhs.parameter_ids;
}

std::ostream& operator<<(std::ostream& stream, const PreparedPlan& prepared_plan) {
  stream << "ParameterIDs: [";
  for (auto parameter_idx = size_t{0}; parameter_idx < prepared_plan.parameter_ids.size(); ++parameter_idx) {
    stream << prepared_plan.parameter_ids[parameter_idx];
    if (parameter_idx + 1 < prepared_plan.parameter_ids.size()) {
      stream << ", ";
    }
  }
  stream << "]\n";
  stream << *prepared_plan.lqp;
  return stream;
}

}  // namespace hyrise
