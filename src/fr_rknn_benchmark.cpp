#include <rknn_api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

uint64_t MonotonicUs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + now.tv_nsec / 1000ULL;
}

uint64_t NonZeroBytes(rknn_tensor_mem* const* outputs, const std::vector<rknn_tensor_attr>& attrs) {
    uint64_t total = 0;
    for (size_t index = 0; index < attrs.size(); ++index) {
        const auto* bytes = static_cast<const unsigned char*>(outputs[index]->virt_addr);
        for (uint32_t offset = 0; bytes && offset < attrs[index].size_with_stride; ++offset) total += bytes[offset] != 0;
    }
    return total;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: %s /path/to/model.rknn [iterations]\n", argv[0]);
        return 64;
    }
    const long requested = argc == 3 ? std::strtol(argv[2], nullptr, 10) : 100;
    if (requested < 1 || requested > 10000) {
        std::fprintf(stderr, "iterations must be from 1 to 10000\n");
        return 64;
    }

    rknn_context context = 0;
    if (rknn_init(&context, argv[1], 0, 0, nullptr) != RKNN_SUCC) {
        std::fprintf(stderr, "rknn_init failed\n");
        return 1;
    }
    rknn_input_output_num io{};
    rknn_tensor_attr input_attr{};
    const bool metadata_ok = rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) == RKNN_SUCC &&
        io.n_input == 1 && io.n_output > 0;
    input_attr.index = 0;
    if (!metadata_ok || rknn_query(context, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attr, sizeof(input_attr)) != RKNN_SUCC) {
        std::fprintf(stderr, "unsupported model I/O for generic benchmark\n");
        rknn_destroy(context);
        return 1;
    }

    // This is intentionally an all-zero synthetic tensor: it validates RKNN
    // execution only and makes no claim about detector preprocessing/results.
    rknn_tensor_mem* input_memory = rknn_create_mem(context, input_attr.size_with_stride);
    std::vector<rknn_tensor_attr> output_attrs(io.n_output);
    std::vector<rknn_tensor_mem*> output_memories(io.n_output, nullptr);
    bool setup_ok = input_memory != nullptr && rknn_set_io_mem(context, input_memory, &input_attr) == RKNN_SUCC;
    for (uint32_t index = 0; setup_ok && index < io.n_output; ++index) {
        output_attrs[index].index = index;
        setup_ok = rknn_query(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &output_attrs[index], sizeof(output_attrs[index])) == RKNN_SUCC;
        if (setup_ok) output_memories[index] = rknn_create_mem(context, output_attrs[index].size_with_stride);
        if (setup_ok) setup_ok = output_memories[index] != nullptr &&
            rknn_set_io_mem(context, output_memories[index], &output_attrs[index]) == RKNN_SUCC;
    }
    if (!setup_ok) {
        std::fprintf(stderr, "RKNN zero-copy setup failed\n");
        for (rknn_tensor_mem* memory : output_memories) if (memory) rknn_destroy_mem(context, memory);
        if (input_memory) rknn_destroy_mem(context, input_memory);
        rknn_destroy(context);
        return 1;
    }
    std::memset(input_memory->virt_addr, 0, input_attr.size_with_stride);

    uint64_t elapsed_us = 0;
    uint64_t nonzero = 0;
    for (long iteration = 0; iteration < requested; ++iteration) {
        const uint64_t started = MonotonicUs();
        if (rknn_run(context, nullptr) != RKNN_SUCC) {
            std::fprintf(stderr, "RKNN execution failed at iteration %ld\n", iteration);
            for (rknn_tensor_mem* memory : output_memories) rknn_destroy_mem(context, memory);
            rknn_destroy_mem(context, input_memory);
            rknn_destroy(context);
            return 1;
        }
        elapsed_us += MonotonicUs() - started;
        nonzero += NonZeroBytes(output_memories.data(), output_attrs);
    }
    std::printf("iterations=%ld total_ms=%.3f average_ms=%.3f fps=%.2f output_nonzero_bytes=%llu\n",
                requested, elapsed_us / 1000.0, elapsed_us / static_cast<double>(requested) / 1000.0,
                requested * 1000000.0 / elapsed_us, static_cast<unsigned long long>(nonzero));
    for (rknn_tensor_mem* memory : output_memories) rknn_destroy_mem(context, memory);
    rknn_destroy_mem(context, input_memory);
    rknn_destroy(context);
    return 0;
}
