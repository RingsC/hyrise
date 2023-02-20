#pragma once

#include "dependency_discovery/dependency_candidates.hpp"
#include "storage/constraints/abstract_table_constraint.hpp"

namespace hyrise {

enum class ValidationStatus { Uncertain, Valid, Invalid, AlreadyKnown };

struct ValidationResult {
 public:
  ValidationResult(const ValidationStatus init_status);
  ValidationResult() = delete;

  const ValidationStatus status;
  std::optional<std::vector<AbstractTableConstraint>> constraints{};
};

class AbstractDependencyValidationRule {
 public:
  AbstractDependencyValidationRule(const DependencyType init_dependency_type);

  AbstractDependencyValidationRule() = delete;
  virtual ~AbstractDependencyValidationRule() = default;

  ValidationResult validate(const AbstractDependencyCandidate& candidate) const;

  const DependencyType dependency_type;

 protected:
  bool _is_known(const std::string& table_name, const std::shared_ptr<AbstractTableConstraint>& constraint) const;

  virtual ValidationResult _on_validate(const AbstractDependencyCandidate& candidate) const = 0;

  // Pointer is required for polymorphism.
  std::shared_ptr<AbstractTableConstraint> _constraint_from_candidate(
      const AbstractDependencyCandidate& candidate) const;
};

}  // namespace hyrise
