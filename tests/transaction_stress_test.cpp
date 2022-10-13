#include <numeric>
#include <random>
#include <thread>
#include <mutex>
#include <chrono>
#include <charconv>
#include <filesystem>

#include <gtest/gtest.h>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

static char const* path() {
    char* path = std::getenv("UKV_TEST_PATH");
    if (path)
        return path;

#if defined(UKV_FLIGHT_CLIENT)
    return nullptr;
#elif defined(UKV_TEST_PATH)
    return UKV_TEST_PATH;
#else
    return nullptr;
#endif
}

thread_local std::random_device random_device;
thread_local std::mt19937 random_generator(random_device());

/**
 * @brief Tests the atomicity of transactions.
 *
 * T threads are created. Each tries to insert B identical values for B consecutive keys.
 * As all threads have their own way of selecting which value to write, we then test,
 * that after the ingestion, every consecutive set of B keys maps to the same values.
 */
template <std::size_t threads_count_ak, std::size_t batch_size_ak>
void insert_atomic_isolated(std::size_t count_batches) {
    database_t db;
    EXPECT_TRUE(db.open(path()));
    EXPECT_TRUE(db.clear());

    std::array<ukv_key_t, batch_size_ak> keys;

    auto task = [&](size_t thread_idx) {
        for (std::size_t idx_batch = 0; idx_batch != count_batches; ++idx_batch) {
            while (true) {
                transaction_t txn = db.transact().throw_or_release();
                auto collection = txn.collection().throw_or_release();

                ukv_key_t const first_key_in_batch = idx_batch * batch_size_ak;
                std::iota(keys.begin(), keys.end(), first_key_in_batch);

                std::uint64_t const num_value = idx_batch * threads_count_ak + thread_idx;
                value_view_t value((byte_t const*)&num_value, sizeof(num_value));
                collection[keys] = value;
                status_t status = txn.commit();
                if (status)
                    break;
            }
        }
    };

    std::array<std::thread, threads_count_ak> threads;
    for (std::size_t i = 0; i < threads_count_ak; ++i)
        threads[i] = std::thread(task, i);
    for (std::size_t i = 0; i < threads_count_ak; ++i)
        threads[i].join();

    bins_collection_t collection = db.collection().throw_or_release();

    for (std::size_t idx_batch = 0; idx_batch != count_batches; ++idx_batch) {
        ukv_key_t const first_key_in_batch = idx_batch * batch_size_ak;
        std::iota(keys.begin(), keys.end(), first_key_in_batch);

        embedded_bins_t retrieved = collection[keys].value().throw_or_release();
        for (std::size_t idx_in_batch = 1; idx_in_batch != batch_size_ak; ++idx_in_batch)
            EXPECT_EQ(retrieved[0], retrieved[idx_in_batch]);
    }

    EXPECT_TRUE(db.clear());
    db.close();
}

enum class operation_code_t {
    select_k,
    insert_k,
    remove_k,
};

template <std::size_t array_size_ak>
struct operation_gt {

    operation_code_t type;
    std::size_t count;
    std::array<ukv_key_t, array_size_ak> keys;
    std::array<std::uint64_t, array_size_ak> values;

    inline operation_gt(operation_code_t op_type, std::size_t op_count) noexcept : type(op_type), count(op_count) {}
};

template <typename element_at, std::size_t arr_size_ak>
void random_fill( //
    std::array<element_at, arr_size_ak>& arr,
    std::size_t size,
    element_at max = std::numeric_limits<element_at>::max()) noexcept {

    std::uniform_int_distribution<element_at> dist(0, max);
    std::generate(arr.begin(), arr.begin() + size, [&dist]() { return dist(random_generator); });
}

auto now() noexcept {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

/**
 * @brief Checks serializability of concurrent transactions.
 *
 * Serializability is the strongest guarantee of concurrent consistency.
 * We run many transactions concurrently, logging their contents and then
 * repeat them from a single thread. The results of both simulations are
 * checked to match exactly.
 *
 * @tparam threads_count_ak Number of concurrent threads.
 * @tparam max_batch_size_ak Maximum number of operations in each transaction.
 * @param iteration_count
 */
template <std::size_t threads_count_ak, std::size_t max_batch_size_ak>
void serializable_transactions(std::size_t iteration_count) {

    database_t db;
    EXPECT_TRUE(db.open(path()));
    EXPECT_TRUE(db.clear());
    std::mutex mutex;

    using time_point_t = std::uint64_t;
    using operation_t = operation_gt<max_batch_size_ak>;

    std::vector<std::pair<time_point_t, operation_t>> operations;
    constexpr ukv_length_t value_length = sizeof(std::uint64_t);
    std::array<ukv_length_t, max_batch_size_ak> value_offsets;
    for (std::size_t i = 0; i != max_batch_size_ak; ++i)
        value_offsets[i] = i * value_length;

    std::uniform_int_distribution<> choose_batch_size(1, max_batch_size_ak);
    auto biggest_key = static_cast<ukv_key_t>(iteration_count * max_batch_size_ak / 4);

    auto task_insert = [&]() {
        contents_arg_t contents {
            .offsets_begin = {value_offsets.data(), sizeof(ukv_length_t)},
            .lengths_begin = {&value_length, 0},
            .count = max_batch_size_ak,
        };

        for (std::size_t iteration_idx = 0; iteration_idx != iteration_count; ++iteration_idx) {

            std::size_t batch_size = choose_batch_size(random_generator);
            operation_t operation(operation_code_t::insert_k, batch_size);
            random_fill(operation.keys, batch_size, biggest_key);
            random_fill(operation.values, batch_size);
            auto batch_keys = strided_range(operation.keys).subspan(0u, batch_size);
            auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(operation.values.data());
            contents.contents_begin = {&vals_begin, 0};

            transaction_t txn = db.transact().throw_or_release();
            status_t status = txn[batch_keys].assign(contents);
            if (!status)
                continue;
            status = txn.commit();
            time_point_t time = now();
            if (!status)
                continue;

            mutex.lock();
            operations.emplace_back(time, std::move(operation));
            mutex.unlock();
        }
    };

    auto task_remove = [&]() {
        for (std::size_t iteration_idx = 0; iteration_idx != iteration_count; ++iteration_idx) {
            std::size_t batch_size = choose_batch_size(random_generator);
            operation_t operation(operation_code_t::remove_k, batch_size);
            random_fill(operation.keys, batch_size, biggest_key);
            auto batch_keys = strided_range(operation.keys).subspan(0u, batch_size);

            transaction_t txn = db.transact().throw_or_release();
            status_t status = txn[batch_keys].erase();
            if (!status)
                continue;
            status = txn.commit();
            time_point_t time = now();
            if (!status)
                continue;

            mutex.lock();
            operations.emplace_back(time, std::move(operation));
            mutex.unlock();
        }
    };

    auto task_select = [&]() {
        for (std::size_t iteration_idx = 0; iteration_idx != iteration_count; ++iteration_idx) {
            std::size_t batch_size = choose_batch_size(random_generator);
            operation_t operation(operation_code_t::select_k, batch_size);
            random_fill(operation.keys, batch_size, biggest_key);
            auto batch_keys = strided_range(operation.keys).subspan(0u, batch_size);

            transaction_t txn = db.transact().throw_or_release();
            auto const& retrieved = txn[batch_keys].value().throw_or_release();
            status_t status = txn.commit();
            time_point_t time = now();
            if (!status)
                continue;

            auto it = retrieved.begin();
            for (std::size_t i = 0; i != batch_size; ++i, ++it) {
                value_view_t val_view = *it;
                if (val_view)
                    std::memcpy((void*)&operation.values[i], (const void*)val_view.data(), sizeof(std::uint64_t));
                else
                    operation.values[i] = 0;
            }

            mutex.lock();
            operations.emplace_back(time, std::move(operation));
            mutex.unlock();
        }
    };

    std::vector<std::thread> threads(threads_count_ak);
    for (std::size_t i = 0; i != (threads_count_ak * 30) / 100; ++i)
        threads[i] = std::thread(task_insert);
    for (std::size_t i = (threads_count_ak * 30) / 100; i != (threads_count_ak * 30) / 100 + threads_count_ak / 10; ++i)
        threads[i] = std::thread(task_remove);
    for (std::size_t i = (threads_count_ak * 30) / 100 + threads_count_ak / 10; i != threads_count_ak; ++i)
        threads[i] = std::thread(task_select);

    for (std::size_t i = 0; i != threads_count_ak; ++i)
        threads[i].join();

    // Recover absolute order
    std::sort(operations.begin(), operations.end(), [](auto& left, auto& right) { return left.first < right.first; });

    // Make a new
    std::filesystem::path second_db_path(path());
    if (second_db_path.has_filename())
        second_db_path += "_simulation";
    else {
        second_db_path = second_db_path.parent_path();
        second_db_path += "_simulation/";
    }

    database_t db_simulation;
    EXPECT_TRUE(db_simulation.open(second_db_path.c_str()));
    EXPECT_TRUE(db_simulation.clear());

    bins_collection_t collection_simulation = db_simulation.collection().throw_or_release();
    for (auto& time_and_operation : operations) {
        auto operation = time_and_operation.second;
        auto ref = collection_simulation[strided_range(operation.keys).subspan(0u, operation.count)];
        contents_arg_t contents {
            .offsets_begin = {value_offsets.data(), sizeof(ukv_length_t)},
            .lengths_begin = {&value_length, 0},
            .count = operation.count,
        };

        if (operation.type == operation_code_t::remove_k)
            EXPECT_TRUE(ref.erase());
        else if (operation.type == operation_code_t::insert_k) {
            auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(operation.values.data());
            contents.contents_begin = {&vals_begin, 0};
            EXPECT_TRUE(ref.assign(contents));
        }
        else {
            auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(operation.values.data());
            contents.contents_begin = {&vals_begin, 0};
            auto const& retrieved = ref.value().throw_or_release();
            auto it = retrieved.begin();
            for (std::size_t i = 0; i != operation.count; ++i, ++it) {
                value_view_t val_view = *it;
                if (!val_view) {
                    EXPECT_EQ(operation.values[i], 0);
                    continue;
                }

                auto expected_len = static_cast<std::size_t>(contents.lengths_begin[i]);
                auto expected_begin =
                    reinterpret_cast<byte_t const*>(contents.contents_begin[i]) + contents.offsets_begin[i];
                value_view_t expected_view(expected_begin, expected_begin + expected_len);
                EXPECT_EQ(val_view.size(), expected_len);
                EXPECT_EQ(val_view, expected_view);
            }
        }
    }

    bins_collection_t collection = db.collection().throw_or_release();
    keys_range_t present_keys = collection.keys();
    keys_stream_t present_it = present_keys.begin();
    keys_range_t present_keys_simulation = collection_simulation.keys();
    keys_stream_t present_it_simulation = present_keys_simulation.begin();

    for (; !present_it.is_end() && !present_it_simulation.is_end(); ++present_it, ++present_it_simulation)
        EXPECT_EQ(*present_it, *present_it_simulation);
    EXPECT_TRUE(present_it.is_end());
    EXPECT_TRUE(present_it_simulation.is_end());
}

TEST(db, insert_atomic_isolated) {
    insert_atomic_isolated<4, 100>(1'000);
    insert_atomic_isolated<8, 100>(1'000);
    insert_atomic_isolated<16, 1000>(1'000);
}

TEST(db, serializable_transactions) {
    serializable_transactions<4, 100>(1'000);
    serializable_transactions<8, 100>(1'000);
    serializable_transactions<16, 1000>(1'000);
}

int main(int argc, char** argv) {
    std::filesystem::create_directory("./tmp");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}