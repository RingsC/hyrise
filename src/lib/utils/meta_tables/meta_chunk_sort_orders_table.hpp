#pragma once

#include <memory>
#include <string>

#include "utils/meta_tables/abstract_meta_table.hpp"

namespace hyrise {

/**
 * This is a class for showing all sort orders of stored chunks via a meta table.
 */
class MetaChunkSortOrdersTable : public AbstractMetaTable {
 public:
  MetaChunkSortOrdersTable();

  const std::string& name() const final;

 protected:
  std::shared_ptr<Table> _on_generate() const final;
};
}  // namespace hyrise
