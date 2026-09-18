/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Collects textual implementation inputs for the global module fragment.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

// Textual input to the global module fragment; consumers import everett.
#include <everett/backend.h>
#include <everett/catalog_bindings.h>
#include <everett/cola_adaptive_index.h>
#include <everett/cola_file_index.h>
#include <everett/cola_index.h>
#include <everett/cola_local_merge.h>
#include <everett/cola_query.h>
#include <everett/cola_runtime.h>
#include <everett/cola_sections.h>
#include <everett/crc32c.h>
#include <everett/durability.h>
#include <everett/elias_fano.h>
#include <everett/error_detail.h>
#include <everett/file.h>
#include <everett/file_index_builder.h>
#include <everett/file_index_pipeline.h>
#include <everett/fingerprint.h>
#include <everett/fixed_search.h>
#include <everett/index_builder.h>
#include <everett/index_pipeline.h>
#include <everett/index_pipeline_detail.h>
#include <everett/key_detail.h>
#include <everett/mapped_blob.h>
#include <everett/mapped_cola.h>
#include <everett/mapped_file.h>
#include <everett/multiverse.h>
#include <everett/native_file_merge.h>
#include <everett/native_file_writer.h>
#include <everett/native_merge.h>
#include <everett/native_sweep.h>
#include <everett/native_writer.h>
#include <everett/nursery_map.h>
#include <everett/object_path.h>
#include <everett/object_stream.h>
#include <everett/object_writer.h>
#include <everett/output_budget.h>
#include <everett/pins.h>
#include <everett/policy.h>
#include <everett/profile.h>
#include <everett/profile_blob.h>
#include <everett/profile_file_output.h>
#include <everett/profile_index.h>
#include <everett/query.h>
#include <everett/rank.h>
#include <everett/rank15.h>
#include <everett/rank_groups.h>
#include <everett/redundant_runtime.h>
#include <everett/registry.h>
#include <everett/replacement_rebuild.h>
#include <everett/runtime_registry.h>
#include <everett/runtime_seal.h>
#include <everett/sampling.h>
#include <everett/sections.h>
#include <everett/session.h>
#include <everett/sort_codec.h>
#include <everett/sort_profile.h>
#include <everett/sort_profile_adaptive.h>
#include <everett/sort_profile_file.h>
#include <everett/sort_profile_file_merge.h>
#include <everett/sort_profile_file_writer.h>
#include <everett/sort_profile_merge.h>
#include <everett/sort_runtime.h>
#include <everett/typed_scan.h>
#include <everett/typed_world.h>
#include <everett/word_view.h>
#include <everett/world.h>
