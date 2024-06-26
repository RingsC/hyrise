#include "base_test.hpp"
#include "expression/expression_utils.hpp"
#include "logical_query_plan/delete_node.hpp"

namespace hyrise {

class DeleteNodeTest : public BaseTest {
 protected:
  void SetUp() override {
    _delete_node = DeleteNode::make();
  }

  std::shared_ptr<DeleteNode> _delete_node;
};

TEST_F(DeleteNodeTest, Description) {
  EXPECT_EQ(_delete_node->description(), "[Delete]");
}

TEST_F(DeleteNodeTest, HashingAndEqualityCheck) {
  const auto another_delete_node = DeleteNode::make();
  EXPECT_EQ(*_delete_node, *another_delete_node);

  EXPECT_EQ(_delete_node->hash(), another_delete_node->hash());
}

TEST_F(DeleteNodeTest, NodeExpressions) {
  EXPECT_TRUE(_delete_node->node_expressions.empty());
}

TEST_F(DeleteNodeTest, ColumnExpressions) {
  EXPECT_TRUE(_delete_node->output_expressions().empty());
}

TEST_F(DeleteNodeTest, Copy) {
  EXPECT_EQ(*_delete_node, *_delete_node->deep_copy());
}

TEST_F(DeleteNodeTest, NoUniqueColumnCombinations) {
  EXPECT_THROW(_delete_node->unique_column_combinations(), std::logic_error);
}

TEST_F(DeleteNodeTest, NoOrderDependencies) {
  EXPECT_THROW(_delete_node->order_dependencies(), std::logic_error);
}

}  // namespace hyrise
