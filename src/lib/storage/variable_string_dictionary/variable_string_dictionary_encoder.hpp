#pragma once

#include <algorithm>
#include <limits>
#include <memory>

#include "storage/base_segment_encoder.hpp"
#include "storage/dictionary_segment.hpp"
#include "storage/fixed_string_dictionary_segment.hpp"
#include "storage/segment_iterables/any_segment_iterable.hpp"
#include "storage/value_segment.hpp"
#include "storage/variable_string_dictionary_segment.hpp"
#include "storage/vector_compression/base_compressed_vector.hpp"
#include "storage/vector_compression/vector_compression.hpp"
#include "types.hpp"
#include "utils/enum_constant.hpp"

namespace hyrise {

/**
 * @brief Encodes a segment using variable string dictionary encoding and compresses its attribute vector using vector compression.
 *
 * The algorithm first creates an attribute vector of standard size (uint32_t) and then compresses it
 * using fixed-width integer encoding.
 */
class VariableStringDictionaryEncoder : public SegmentEncoder<VariableStringDictionaryEncoder> {
 public:
  static constexpr auto _encoding_type = enum_c<EncodingType, EncodingType::VariableStringDictionary>;
  static constexpr auto _uses_vector_compression = true;  // see base_segment_encoder.hpp for details

  std::shared_ptr<AbstractEncodedSegment> _on_encode(const AnySegmentIterable<pmr_string> segment_iterable,
                                                     const PolymorphicAllocator<pmr_string>& allocator) {
    // Vectors to gather the input segment's data. This data is used in a later step to
    // construct the actual dictionary and attribute vector.
    auto dense_values = std::vector<pmr_string>();  // contains the actual values (no NULLs)
    auto null_values = std::vector<bool>();         // bitmap to mark NULL values
    auto string_to_positions = std::unordered_map<pmr_string, std::vector<ChunkOffset>>();
    auto segment_size = uint32_t{0};

    segment_iterable.with_iterators([&](auto segment_it, const auto segment_end) {
      segment_size = std::distance(segment_it, segment_end);
      dense_values.reserve(segment_size);  // potentially overallocate for segments with NULLs
      null_values.resize(segment_size);    // resized to size of segment

      for (auto current_position = size_t{0}; segment_it != segment_end; ++segment_it, ++current_position) {
        const auto segment_item = *segment_it;
        if (!segment_item.is_null()) {
          const auto segment_value = segment_item.value();
          dense_values.push_back(segment_value);
          string_to_positions[segment_value].push_back(ChunkOffset(current_position));
        } else {
          null_values[current_position] = true;
        }
      }
    });

    std::sort(dense_values.begin(), dense_values.end());
    dense_values.erase(std::unique(dense_values.begin(), dense_values.end()), dense_values.cend());
    dense_values.shrink_to_fit();

    auto total_size = uint32_t{0};
    for (const auto& value : dense_values) {
      total_size += value.size() + 1;
    }

    // TODO::(us) reserve instead of resize, beware null terminators
    auto klotz = std::make_shared<pmr_vector<char>>(pmr_vector<char>(total_size));
    // We assume segment size up to 4 GByte.
    auto string_offsets = std::unordered_map<pmr_string, uint32_t>();
    auto current_offset = uint32_t{0};

    for (const auto& value : dense_values) {
      memcpy(klotz->data() + current_offset, value.c_str(), value.size());
      string_offsets[value] = current_offset;
      current_offset += value.size() + 1;
    }

    auto position_to_offset = std::make_shared<pmr_vector<uint32_t>>(pmr_vector<uint32_t>(segment_size));

    for (const auto& [string, positions] : string_to_positions) {
      const auto here = string_offsets[string];
      for (const auto position : positions) {
        (*position_to_offset)[position] = here;
      }
    }

    const auto max_value_id = dense_values.size();
    const auto compressed_position_to_offset = std::shared_ptr<const BaseCompressedVector>(
        compress_vector(*position_to_offset, SegmentEncoder<VariableStringDictionaryEncoder>::vector_compression_type(),
                        allocator, {max_value_id}));

    return std::make_shared<VariableStringDictionarySegment>(klotz, compressed_position_to_offset, position_to_offset);
  }

 private:
  template <typename U, typename T>
  static ValueID _get_value_id(const U& dictionary, const T& value) {
    return ValueID{static_cast<ValueID::base_type>(
        std::distance(dictionary->cbegin(), std::lower_bound(dictionary->cbegin(), dictionary->cend(), value)))};
  }
};

}  // namespace hyrise
