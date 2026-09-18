/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exports the optional durable catalog interface.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

// Explicit exports of global-module entities preserve identity across profiles.
export namespace everett {
  using ::everett::active_engine;
  using ::everett::catalog_admission;
  using ::everett::catalog_auxiliary_roots;
  using ::everett::catalog_error;
  using ::everett::catalog_native_merge;
  using ::everett::catalog_native_merge_kind;
  using ::everett::catalog_object_reservation;
  using ::everett::catalog_operation;
  using ::everett::catalog_options;
  using ::everett::catalog_saved_root;
  using ::everett::catalog_session_head;
  using ::everett::catalog_session_publication;
  using ::everett::catalog_timeline_head;
  using ::everett::catalog_timeline_publication;
  using ::everett::connect;
  using ::everett::connection;
  using ::everett::connection_options;
  using ::everett::decoded_runtime_checkpoint;
  using ::everett::persistent_engine;
  using ::everett::private_construction;
  using ::everett::random_object_ids;
  using ::everett::runtime_checkpoint;
  using ::everett::runtime_output_options;
  using ::everett::runtime_storage_codec;
  using ::everett::runtime_store;
  using ::everett::sort_file_runtime_storage;
  using ::everett::sort_runtime_context;
  using ::everett::sort_runtime_store;
  using ::everett::sqlite_catalog;
  using ::everett::sqlite_catalog_ops;
  using ::everett::stored_runtime;
  using ::everett::stored_world;
  using ::everett::streaming_sort_runtime_family;
  using ::everett::transaction;
  using ::everett::transaction_conflict;
}
