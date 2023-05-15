#pragma once

#include <memory>

#include "base_test.hpp"

namespace hyrise {

class AbstractLQPNode;
class AbstractRule;

class StrategyBaseTest : public BaseTest {
 protected:
  /**
   * Helper method for applying a single rule to an LQP. Creates the temporary LogicalPlanRootNode and applies the rule.
   */
  void apply_rule(const std::shared_ptr<AbstractRule>& rule, const std::shared_ptr<AbstractLQPNode>& input);
};

}  // namespace hyrise
