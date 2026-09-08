#include <rknn_api.h>

#include <cstdio>
#include <cstring>

namespace {

void PrintDimensions(const rknn_tensor_attr& attr) {
    std::printf("[");
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        std::printf("%u%s", attr.dims[i], i + 1 == attr.n_dims ? "" : ", ");
    }
    std::printf("]");
}

void PrintTensor(const char* role, uint32_t index, const rknn_tensor_attr& attr) {
    std::printf("%s[%u]: name=%s dims=", role, index, attr.name);
    PrintDimensions(attr);
    std::printf(" format=%s type=%s quant=%s scale=%g zero_point=%d size=%u stride_size=%u\n",
                get_format_string(attr.fmt), get_type_string(attr.type),
                get_qnt_type_string(attr.qnt_type), attr.scale, attr.zp,
                attr.size, attr.size_with_stride);
}

bool QueryTensor(rknn_context context, rknn_query_cmd command, const char* role, uint32_t index) {
    rknn_tensor_attr attr{};
    attr.index = index;
    const int result = rknn_query(context, command, &attr, sizeof(attr));
    if (result != RKNN_SUCC) {
        std::printf("%s[%u]: query command %d unavailable (result=%d)\n", role, index, command, result);
        return false;
    }
    PrintTensor(role, index, attr);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /path/to/model.rknn\n", argv[0]);
        return 64;
    }

    rknn_context context = 0;
    const int init_result = rknn_init(&context, argv[1], 0, 0, nullptr);
    if (init_result != RKNN_SUCC) {
        std::fprintf(stderr, "rknn_init failed for %s: %d\n", argv[1], init_result);
        return 1;
    }

    int exit_code = 0;
    rknn_sdk_version version{};
    if (rknn_query(context, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == RKNN_SUCC) {
        std::printf("RKNN API Version    : %s\n", version.api_version);
        std::printf("RKNN Driver Version : %s\n", version.drv_version);
    } else {
        std::printf("RKNN SDK version query unavailable\n");
        exit_code = 1;
    }

    rknn_input_output_num io{};
    if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != RKNN_SUCC) {
        std::fprintf(stderr, "RKNN_QUERY_IN_OUT_NUM failed\n");
        rknn_destroy(context);
        return 1;
    }
    std::printf("Inputs : %u\nOutputs: %u\n", io.n_input, io.n_output);
    for (uint32_t index = 0; index < io.n_input; ++index) {
        QueryTensor(context, RKNN_QUERY_INPUT_ATTR, "input", index);
        QueryTensor(context, RKNN_QUERY_NATIVE_INPUT_ATTR, "native_input", index);
    }
    for (uint32_t index = 0; index < io.n_output; ++index) {
        QueryTensor(context, RKNN_QUERY_OUTPUT_ATTR, "output", index);
        QueryTensor(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR, "native_output", index);
    }

    rknn_destroy(context);
    return exit_code;
}
