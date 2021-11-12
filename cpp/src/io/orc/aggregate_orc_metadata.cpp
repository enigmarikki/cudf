/*
 * Copyright (c) 2021, NVIDIA CORPORATION.
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
 */

#include "aggregate_orc_metadata.hpp"

#include <algorithm>
#include <numeric>

namespace cudf::io::orc::detail {

column_hierarchy::column_hierarchy(nesting_map child_map) : children{std::move(child_map)}
{
  // Sort columns by nesting levels
  std::function<void(size_type, int32_t)> levelize = [&](size_type id, int32_t level) {
    if (static_cast<int32_t>(levels.size()) == level) levels.emplace_back();

    levels[level].push_back({id, static_cast<int32_t>(children[id].size())});

    for (auto child_id : children[id]) {
      levelize(child_id, level + 1);
    }
  };

  std::for_each(
    children[0].cbegin(), children[0].cend(), [&](auto col_id) { levelize(col_id, 0); });
}

namespace {

/**
 * @brief Goes up to the root to include the column with the given id and its parents.
 */
void update_parent_mapping(std::map<size_type, std::vector<size_type>>& selected_columns,
                           metadata const& metadata,
                           size_type id)
{
  auto current_id = id;
  while (metadata.column_has_parent(current_id)) {
    auto parent_id = metadata.parent_id(current_id);
    if (std::find(selected_columns[parent_id].cbegin(),
                  selected_columns[parent_id].cend(),
                  current_id) == selected_columns[parent_id].end()) {
      selected_columns[parent_id].push_back(current_id);
    }
    current_id = parent_id;
  }
}

/**
 * @brief Adds all columns nested under the column with the given id to the nesting map.
 */
void add_nested_columns(std::map<size_type, std::vector<size_type>>& selected_columns,
                        std::vector<SchemaType> const& types,
                        size_type id)
{
  for (auto child_id : types[id].subtypes) {
    if (std::find(selected_columns[id].cbegin(), selected_columns[id].cend(), child_id) ==
        selected_columns[id].end()) {
      selected_columns[id].push_back(child_id);
    }
    add_nested_columns(selected_columns, types, child_id);
  }
}

/**
 * @brief Adds the column with the given id to the mapping
 *
 * All nested columns and direct ancestors of column `id` are included.
 * Columns that are not on the direct path are excluded, which may result in prunning.
 */
void add_column_to_mapping(std::map<size_type, std::vector<size_type>>& selected_columns,
                           metadata const& metadata,
                           size_type id)
{
  update_parent_mapping(selected_columns, metadata, id);
  add_nested_columns(selected_columns, metadata.ff.types, id);
}

/**
 * @brief Create a metadata object from each element in the source vector
 */
auto metadatas_from_sources(std::vector<std::unique_ptr<datasource>> const& sources)
{
  std::vector<metadata> metadatas;
  std::transform(
    sources.cbegin(), sources.cend(), std::back_inserter(metadatas), [](auto const& source) {
      return metadata(source.get());
    });
  return metadatas;
}

}  // namespace

size_type aggregate_orc_metadata::calc_num_rows() const
{
  return std::accumulate(
    per_file_metadata.begin(), per_file_metadata.end(), 0, [](auto const& sum, auto const& pfm) {
      return sum + pfm.get_total_rows();
    });
}

size_type aggregate_orc_metadata::calc_num_stripes() const
{
  return std::accumulate(
    per_file_metadata.begin(), per_file_metadata.end(), 0, [](auto const& sum, auto const& pfm) {
      return sum + pfm.get_num_stripes();
    });
}

aggregate_orc_metadata::aggregate_orc_metadata(
  std::vector<std::unique_ptr<datasource>> const& sources)
  : per_file_metadata(metadatas_from_sources(sources)),
    num_rows(calc_num_rows()),
    num_stripes(calc_num_stripes())
{
  // Verify that the input files have the same number of columns,
  // as well as matching types, compression, and names
  for (auto const& pfm : per_file_metadata) {
    CUDF_EXPECTS(per_file_metadata[0].get_num_columns() == pfm.get_num_columns(),
                 "All sources must have the same number of columns");
    CUDF_EXPECTS(per_file_metadata[0].ps.compression == pfm.ps.compression,
                 "All sources must have the same compression type");

    // Check the types, column names, and decimal scale
    for (size_t i = 0; i < pfm.ff.types.size(); i++) {
      CUDF_EXPECTS(pfm.ff.types[i].kind == per_file_metadata[0].ff.types[i].kind,
                   "Column types across all input sources must be the same");
      CUDF_EXPECTS(std::equal(pfm.ff.types[i].fieldNames.begin(),
                              pfm.ff.types[i].fieldNames.end(),
                              per_file_metadata[0].ff.types[i].fieldNames.begin()),
                   "All source column names must be the same");
      CUDF_EXPECTS(
        pfm.ff.types[i].scale.value_or(0) == per_file_metadata[0].ff.types[i].scale.value_or(0),
        "All scale values must be the same");
    }
  }
}

std::vector<metadata::stripe_source_mapping> aggregate_orc_metadata::select_stripes(
  std::vector<std::vector<size_type>> const& user_specified_stripes,
  size_type& row_start,
  size_type& row_count)
{
  std::vector<metadata::stripe_source_mapping> selected_stripes_mapping;

  if (!user_specified_stripes.empty()) {
    CUDF_EXPECTS(user_specified_stripes.size() == per_file_metadata.size(),
                 "Must specify stripes for each source");
    // row_start is 0 if stripes are set. If this is not true anymore, then
    // row_start needs to be subtracted to get the correct row_count
    CUDF_EXPECTS(row_start == 0, "Start row index should be 0");

    row_count = 0;
    // Each vector entry represents a source file; each nested vector represents the
    // user_defined_stripes to get from that source file
    for (size_t src_file_idx = 0; src_file_idx < user_specified_stripes.size(); ++src_file_idx) {
      std::vector<OrcStripeInfo> stripe_infos;

      // Coalesce stripe info at the source file later since that makes downstream processing much
      // easier in impl::read
      for (const size_t& stripe_idx : user_specified_stripes[src_file_idx]) {
        CUDF_EXPECTS(stripe_idx < per_file_metadata[src_file_idx].ff.stripes.size(),
                     "Invalid stripe index");
        stripe_infos.push_back(
          std::make_pair(&per_file_metadata[src_file_idx].ff.stripes[stripe_idx], nullptr));
        row_count += per_file_metadata[src_file_idx].ff.stripes[stripe_idx].numberOfRows;
      }
      selected_stripes_mapping.push_back({static_cast<int>(src_file_idx), stripe_infos});
    }
  } else {
    row_start = std::max(row_start, 0);
    if (row_count < 0) {
      row_count = static_cast<size_type>(
        std::min<int64_t>(get_num_rows(), std::numeric_limits<size_type>::max()));
    }
    row_count = std::min(row_count, get_num_rows() - row_start);
    CUDF_EXPECTS(row_count >= 0, "Invalid row count");
    CUDF_EXPECTS(row_start <= get_num_rows(), "Invalid row start");

    size_type count            = 0;
    size_type stripe_skip_rows = 0;
    // Iterate all source files, each source file has corelating metadata
    for (size_t src_file_idx = 0;
         src_file_idx < per_file_metadata.size() && count < row_start + row_count;
         ++src_file_idx) {
      std::vector<OrcStripeInfo> stripe_infos;

      for (size_t stripe_idx = 0; stripe_idx < per_file_metadata[src_file_idx].ff.stripes.size() &&
                                  count < row_start + row_count;
           ++stripe_idx) {
        count += per_file_metadata[src_file_idx].ff.stripes[stripe_idx].numberOfRows;
        if (count > row_start || count == 0) {
          stripe_infos.push_back(
            std::make_pair(&per_file_metadata[src_file_idx].ff.stripes[stripe_idx], nullptr));
        } else {
          stripe_skip_rows = count;
        }
      }

      selected_stripes_mapping.push_back({static_cast<int>(src_file_idx), stripe_infos});
    }
    // Need to remove skipped rows from the stripes which are not selected.
    row_start -= stripe_skip_rows;
  }

  // Read each stripe's stripefooter metadata
  if (not selected_stripes_mapping.empty()) {
    for (auto& mapping : selected_stripes_mapping) {
      // Resize to all stripe_info for the source level
      per_file_metadata[mapping.source_idx].stripefooters.resize(mapping.stripe_info.size());

      for (size_t i = 0; i < mapping.stripe_info.size(); i++) {
        const auto stripe         = mapping.stripe_info[i].first;
        const auto sf_comp_offset = stripe->offset + stripe->indexLength + stripe->dataLength;
        const auto sf_comp_length = stripe->footerLength;
        CUDF_EXPECTS(
          sf_comp_offset + sf_comp_length < per_file_metadata[mapping.source_idx].source->size(),
          "Invalid stripe information");
        const auto buffer =
          per_file_metadata[mapping.source_idx].source->host_read(sf_comp_offset, sf_comp_length);
        size_t sf_length = 0;
        auto sf_data     = per_file_metadata[mapping.source_idx].decompressor->Decompress(
          buffer->data(), sf_comp_length, &sf_length);
        ProtobufReader(sf_data, sf_length)
          .read(per_file_metadata[mapping.source_idx].stripefooters[i]);
        mapping.stripe_info[i].second = &per_file_metadata[mapping.source_idx].stripefooters[i];
        if (stripe->indexLength == 0) { row_grp_idx_present = false; }
      }
    }
  }

  return selected_stripes_mapping;
}

column_hierarchy aggregate_orc_metadata::select_columns(
  std::vector<std::string> const& column_paths)
{
  auto const& pfm = per_file_metadata[0];

  column_hierarchy::nesting_map selected_columns;
  if (column_paths.empty()) {
    for (auto const& col_id : pfm.ff.types[0].subtypes) {
      add_column_to_mapping(selected_columns, pfm, col_id);
    }
  } else {
    for (const auto& path : column_paths) {
      bool name_found = false;
      for (auto col_id = 1; col_id < pfm.get_num_columns(); ++col_id) {
        if (pfm.column_path(col_id) == path) {
          name_found = true;
          add_column_to_mapping(selected_columns, pfm, col_id);
          break;
        }
      }
      CUDF_EXPECTS(name_found, "Unknown column name: " + std::string(path));
    }
  }
  return {std::move(selected_columns)};
}

}  // namespace cudf::io::orc::detail