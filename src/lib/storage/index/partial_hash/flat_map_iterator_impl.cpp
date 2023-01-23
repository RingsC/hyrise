#include "flat_map_iterator_impl.hpp"
#include "flat_map_iterator.hpp"

#include "resolve_type.hpp"

namespace hyrise {

template <typename DataType>
FlatMapIteratorImpl<DataType>::FlatMapIteratorImpl(MapIterator it) : _map_iterator(it), _vector_index(0) {}

template <typename DataType>
const RowID& FlatMapIteratorImpl<DataType>::operator*() const {
  return _map_iterator->second[_vector_index];
}

template <typename DataType>
FlatMapIteratorImpl<DataType>& FlatMapIteratorImpl<DataType>::operator++() {
  if (++_vector_index >= _map_iterator->second.size()) {
    ++_map_iterator;
    _vector_index = 0;
  }
  return *this;
}

template <typename DataType>
bool FlatMapIteratorImpl<DataType>::operator==(const BaseFlatMapIteratorImpl& other) const {
  auto other_iterator = dynamic_cast<const FlatMapIteratorImpl*>(&other);
  return other_iterator && _map_iterator == other_iterator->_map_iterator &&
         _vector_index == other_iterator->_vector_index;
}

template <typename DataType>
bool FlatMapIteratorImpl<DataType>::operator!=(const BaseFlatMapIteratorImpl& other) const {
  auto other_iterator = dynamic_cast<const FlatMapIteratorImpl*>(&other);
  return !other_iterator || _map_iterator != other_iterator->_map_iterator ||
         _vector_index != other_iterator->_vector_index;
}

template <typename DataType>
std::shared_ptr<BaseFlatMapIteratorImpl> FlatMapIteratorImpl<DataType>::clone() const {
  return std::make_shared<FlatMapIteratorImpl<DataType>>(*this);
}

template <typename DataType>
FlatMapIterator FlatMapIteratorImpl<DataType>::flat_map_iterator(MapIterator it) {
  return FlatMapIterator(std::make_shared<FlatMapIteratorImpl<DataType>>(it));
}

EXPLICITLY_INSTANTIATE_DATA_TYPES(FlatMapIteratorImpl);

}  // namespace hyrise