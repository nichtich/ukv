/**
 * @file ukv_docs_nlohmann.cpp
 * @author Ashot Vardanian
 *
 * @brief Document storage using "nlohmann/JSON" lib.
 * Sits on top of any @see "ukv.h"-compatiable system.
 */

#include <vector>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ukv/docs.hpp"
#include "helpers.hpp"

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

using json_t = nlohmann::json;
using json_ptr_t = json_t::json_pointer;
using serializer_t = nlohmann::detail::serializer<json_t>;

/**
 * @brief Extracts a select subset of keys by from input document.
 *
 * Can be implemented through flattening, sampling and unflattening.
 * https://json.nlohmann.me/api/json_pointer/
 */
json_t sample_fields(json_t&& original,
                     std::vector<json_ptr_t> const& json_pointers,
                     std::vector<std::string> const& json_pointers_strs) {

    if (json_pointers.empty())
        return std::move(original);

    json_t empty {nullptr};
    json_t result = json_t::object();
    for (std::size_t ptr_idx = 0; ptr_idx != json_pointers.size(); ++ptr_idx) {

        auto const& ptr = json_pointers[ptr_idx];
        auto const& ptr_str = json_pointers_strs[ptr_idx];

        // An exception-safe approach to searching for JSON-pointers:
        // https://json.nlohmann.me/api/basic_json/at/#exceptions
        // https://json.nlohmann.me/api/basic_json/operator%5B%5D/#exceptions
        // https://json.nlohmann.me/api/basic_json/value/#exception-safety
        auto found = original.value(ptr, empty);
        if (found != empty)
            result[ptr_str] = std::move(found);
    }

    // https://json.nlohmann.me/features/json_pointer/
    // https://json.nlohmann.me/api/basic_json/flatten/
    // https://json.nlohmann.me/api/basic_json/unflatten/
    return result.unflatten();
}

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

json_t parse_any(value_view_t bytes, ukv_format_t const c_format, ukv_error_t* c_error) {
    auto str = reinterpret_cast<char const*>(bytes.begin());
    auto len = bytes.size();
    switch (c_format) {
    case ukv_format_json_k:
    case ukv_format_json_patch_k: return json_t::parse(str, str + len, nullptr, true, false);
    case ukv_format_msgpack_k: return json_t::from_msgpack(str, str + len, true, false);
    case ukv_format_bson_k: return json_t::from_bson(str, str + len, true, false);
    case ukv_format_cbor_k: return json_t::from_cbor(str, str + len, true, false);
    case ukv_format_ubjson_k: return json_t::from_ubjson(str, str + len, true, false);
    default: *c_error = "Unsupported unput format"; return {};
    }
}

buffer_t dump_any(json_t const& json, ukv_format_t const c_format, ukv_error_t* c_error) {
    buffer_t result;
    // Yes, it's a dirty hack, but it works :)
    // nlohmann::detail::output_vector_adapter<byte_t> output(result);
    auto& result_chars = reinterpret_cast<std::vector<char>&>(result);
    switch (c_format) {
    case ukv_format_json_k: {
        auto adapt = std::make_shared<nlohmann::detail::output_vector_adapter<char>>(result_chars);
        serializer_t(adapt, ' ').dump(json, false, false, 0, 0);
        break;
    }
    case ukv_format_msgpack_k: json_t::to_msgpack(json, result_chars); break;
    case ukv_format_bson_k: json_t::to_bson(json, result_chars); break;
    case ukv_format_cbor_k: json_t::to_cbor(json, result_chars); break;
    case ukv_format_ubjson_k: json_t::to_ubjson(json, result_chars); break;
    default: *c_error = "Unsupported unput format"; break;
    }

    return result;
}

void update_docs( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const>,
    ukv_size_t const n,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) {

    for (ukv_size_t i = 0; i != n; ++i) {
        auto task = tasks[i];
        if (task.is_deleted()) {
            arena.updated_vals[i].reset();
            continue;
        }

        auto parsed = parse_any(task.view(), c_format, c_error);
        if (parsed.is_discarded()) {
            *c_error = "Couldn't parse inputs";
            return;
        }

        auto serial = dump_any(parsed, ukv_format_msgpack_k, c_error);
        if (*c_error)
            return;
    }

    ukv_val_len_t offset = 0;
    ukv_arena_t arena_ptr = &arena;
    ukv_write( //
        c_db,
        c_txn,
        tasks.cols.get(),
        tasks.cols.stride(),
        tasks.keys.get(),
        n,
        tasks.keys.stride(),
        arena.updated_vals.front().internal_cptr(),
        sizeof(value_t),
        &offset,
        0,
        arena.updated_vals.front().internal_length(),
        sizeof(value_t),
        c_options,
        &arena_ptr,
        c_error);
}

void update_fields( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const> fields,
    ukv_size_t const n,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) {

    std::vector<json_t> parsed(n);
    std::vector<json_ptr_t> fields_ptrs;

    std::vector<buffer_t> serialized(n);

    if (parsed[0].is_discarded()) {
        *c_error = "Couldn't parse inputs";
        return;
    }
}

void ukv_docs_write( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_count,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_ptr_t const* c_vals,
    ukv_size_t const c_vals_stride,

    ukv_val_len_t const* c_lens,
    ukv_size_t const c_lens_stride,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    if (!c_db) {
        *c_error = "DataBase is NULL!";
        return;
    }

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};
    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    strided_iterator_gt<ukv_val_ptr_t const> vals {c_vals, c_vals_stride};
    strided_iterator_gt<ukv_val_len_t const> offs {nullptr, 0};
    strided_iterator_gt<ukv_val_len_t const> lens {c_lens, c_lens_stride};
    write_tasks_soa_t tasks {cols, keys, vals, offs, lens};

    try {
        auto func = fields ? &update_fields : &update_docs;
        func(c_db, c_txn, tasks, fields, c_keys_count, c_options, c_format, arena, c_error);
    }
    catch (std::bad_alloc) {
        *c_error = "Failed to allocate memory!";
    }
}

void ukv_docs_read( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_count,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_count,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_len_t** c_found_lengths,
    ukv_val_ptr_t* c_found_values,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {
}