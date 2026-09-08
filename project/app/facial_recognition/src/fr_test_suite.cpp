#include "fr_face_detector.hpp"
#include "fr_face_recognizer.hpp"
#include "fr_face_db.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

// ============================================================================
// Helper Utilities & Reference Implementations from Media & WebConfig
// ============================================================================

std::string VietnamTimestamp(time_t t) {
    t += 7 * 3600;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

bool SafeDate(const std::string& value) {
    if (value.size() != 8) return false;
    for (unsigned char c : value) if (!std::isdigit(c)) return false;
    return true;
}

bool SafeSegment(const std::string& value) {
    if (value.rfind("ai_", 0) != 0) return false;
    const size_t dot = value.rfind('.');
    if (dot == std::string::npos || dot < 18) return false;
    const std::string ext = value.substr(dot);
    if (ext != ".h264" && ext != ".h265") return false;
    const std::string stem = value.substr(3, dot - 3); // YYYYMMDD_HHMMSS
    if (stem.size() != 15 || stem[8] != '_') return false;
    for (unsigned char c : stem) if (c != '_' && !std::isdigit(c)) return false;
    return true;
}

struct NalStartInfo {
    bool vps = false;
    bool sps = false;
    bool pps = false;
    bool idr = false;
};

NalStartInfo ScanNalStart(const uint8_t* data, size_t size, bool hevc) {
    NalStartInfo info;
    for (size_t i = 0; i + 4 < size && !info.idr; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            header = i + 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                   data[i + 3] == 1) {
            header = i + 4;
        } else {
            continue;
        }
        if (header >= size) continue;
        if (hevc) {
            const unsigned type = (data[header] >> 1) & 0x3f;
            info.vps |= type == 32;
            info.sps |= type == 33;
            info.pps |= type == 34;
            info.idr |= type >= 16 && type <= 21;
        } else {
            const unsigned type = data[header] & 0x1f;
            info.sps |= type == 7;
            info.pps |= type == 8;
            info.idr |= type == 5;
        }
    }
    return info;
}

bool CaptureParamSets(const uint8_t* data, size_t size, bool hevc, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i + 4 <= size) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            header = i + 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                   data[i + 3] == 1) {
            header = i + 4;
        } else {
            ++i;
            continue;
        }
        size_t nal_end = header + 1;
        for (size_t j = header + 1; j + 1 < size; ++j) {
            if (data[j - 1] == 0 && data[j] == 0 && data[j + 1] == 1) {
                nal_end = j - 1;
                break;
            }
            if (j + 2 < size && data[j] == 0 && data[j + 1] == 0 &&
                data[j + 2] == 0 && j + 3 < size && data[j + 3] == 1) {
                nal_end = j;
                break;
            }
        }
        if (nal_end == header + 1) nal_end = size;
        if (nal_end > (header + 1) && nal_end <= size) {
            const unsigned type = hevc ? ((data[header] >> 1) & 0x3f)
                                       : (data[header] & 0x1f);
            const bool keep = hevc ? (type == 32 || type == 33 || type == 34)
                                   : (type == 7 || type == 8);
            if (keep) out.append(reinterpret_cast<const char*>(data + i), nal_end - i);
        }
        i = nal_end + (nal_end < size ? 1 : 0);
        if (i >= size) break;
    }
    return !out.empty();
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

int B64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string Base64Decode(const std::string& input) {
    std::string out; uint32_t value = 0; int bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        const int decoded = B64(c);
        if (decoded < 0) return {};
        value = (value << 6) | static_cast<uint32_t>(decoded); bits += 6;
        if (bits >= 0) { out += static_cast<char>((value >> bits) & 0xff); bits -= 8; }
    }
    return out;
}

fr::FaceFeature CreateNormalizedFeature(int seed) {
    fr::FaceFeature feat;
    float sum_sq = 0.0f;
    for (size_t i = 0; i < fr::kFeatureDim; ++i) {
        float val = std::sin(static_cast<float>(seed * 17 + i * 31));
        feat.data[i] = val;
        sum_sq += val * val;
    }
    float norm = std::sqrt(sum_sq);
    if (norm > 1e-6f) {
        for (float& v : feat.data) v /= norm;
    }
    return feat;
}

} // namespace

// ============================================================================
// TEST 1: Concurrency, Thread Safety & Multi-Sample Database (20 Iterations)
// ============================================================================
void RunConcurrentDatabaseStressTests(int num_iterations = 20) {
    std::printf("\n======================================================================\n");
    std::printf("  [TEST 1] Concurrency & FaceDatabase Stress Testing (%d Iterations)\n", num_iterations);
    std::printf("======================================================================\n");

    const std::string db_file = "/tmp/stress_test_database.json";

    for (int iter = 1; iter <= num_iterations; ++iter) {
        unlink(db_file.c_str());
        fr::FaceDatabase db(db_file);

        constexpr int kNumThreads = 8;
        constexpr int kOpsPerThread = 25;
        std::atomic<int> total_enrolled{0};
        std::atomic<int> total_searches{0};
        std::atomic<int> total_deletions{0};
        std::atomic<bool> start_flag{false};

        std::vector<std::thread> workers;
        workers.reserve(kNumThreads);

        for (int t = 0; t < kNumThreads; ++t) {
            workers.emplace_back([&, t]() {
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (int op = 0; op < kOpsPerThread; ++op) {
                    int op_type = (op + t) % 3;
                    if (op_type == 0) {
                        // Enroll Person with 3 multi-sample feature vectors
                        std::string name = "Người dùng " + std::to_string(t) + "_" + std::to_string(op) + " (Đỗ Văn " + std::to_string(iter) + ")";
                        std::vector<fr::FaceFeature> samples;
                        for (int s = 0; s < 3; ++s) {
                            samples.push_back(CreateNormalizedFeature(t * 1000 + op * 10 + s + iter));
                        }
                        std::string id;
                        if (db.AddPerson(name, samples, &id)) {
                            total_enrolled.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (op_type == 1) {
                        // Perform Search
                        auto query_feat = CreateNormalizedFeature(t * 1000 + op * 10 + iter);
                        auto match = db.FindMatch(query_feat, 0.70f);
                        (void)match;
                        total_searches.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // List & Random Delete
                        auto list = db.ListPersons();
                        if (!list.empty()) {
                            size_t idx = (op + t) % list.size();
                            if (db.DeletePerson(list[idx].id)) {
                                total_deletions.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            });
        }

        auto start_time = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);

        for (auto& th : workers) {
            th.join();
        }
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();

        // Verify Database Persistence & Consistency by Reloading
        fr::FaceDatabase reloaded(db_file);
        size_t in_mem_count = db.Count();
        size_t disk_count = reloaded.Count();
        assert(in_mem_count == disk_count);

        std::printf("  Iteration %02d/%02d: Enrolled=%d, Searches=%d, Deletions=%d, Remaining=%zu in %lldus [PASS]\n",
                    iter, num_iterations, total_enrolled.load(), total_searches.load(), total_deletions.load(), in_mem_count, static_cast<long long>(duration_us));
    }
    unlink(db_file.c_str());
    std::printf("--> ALL %d CONCURRENCY & DATABASE STRESS ITERATIONS PASSED!\n", num_iterations);
}

// ============================================================================
// TEST 2: Face Recognizer, Geometry, Feature Extraction & Invariants (20 Iterations)
// ============================================================================
void RunFaceRecognizerMathInvariantsTests(int num_iterations = 20) {
    std::printf("\n======================================================================\n");
    std::printf("  [TEST 2] Face Recognizer, Geometry & Math Invariants (%d Iterations)\n", num_iterations);
    std::printf("======================================================================\n");

    fr::FaceRecognizer recognizer;
    std::vector<uint8_t> rgb_buffer(640 * 640 * 3, 128);

    std::mt19937 rng(42);

    for (int iter = 1; iter <= num_iterations; ++iter) {
        // 1. Valid face setup with slight random jitter
        std::uniform_real_distribution<float> jitter(-10.0f, 10.0f);
        fr::FaceDetection face;
        face.score = 0.90f + std::abs(jitter(rng)) * 0.005f;
        face.bbox.x1 = 150.0f + jitter(rng);
        face.bbox.y1 = 120.0f + jitter(rng);
        face.bbox.x2 = 350.0f + jitter(rng);
        face.bbox.y2 = 380.0f + jitter(rng);

        face.landmarks[0] = {200.0f + jitter(rng), 200.0f + jitter(rng)}; // Left eye
        face.landmarks[1] = {300.0f + jitter(rng), 200.0f + jitter(rng)}; // Right eye
        face.landmarks[2] = {250.0f + jitter(rng), 260.0f + jitter(rng)}; // Nose
        face.landmarks[3] = {210.0f + jitter(rng), 310.0f + jitter(rng)}; // Left mouth
        face.landmarks[4] = {290.0f + jitter(rng), 310.0f + jitter(rng)}; // Right mouth

        std::string reason;
        bool quality = fr::FaceRecognizer::CheckFaceQuality(face, 640, 640, &reason);
        assert(quality);

        // 2. Feature Extraction & L2 Normalization Invariant Check
        fr::FaceFeature feat;
        bool extracted = recognizer.ExtractFeature(rgb_buffer.data(), 640, 640, face, feat);
        assert(extracted);
        assert(feat.IsValid());

        float sum_sq = 0.0f;
        for (float v : feat.data) sum_sq += v * v;
        float norm = std::sqrt(sum_sq);
        assert(std::abs(norm - 1.0f) < 1e-4f); // Must be strictly unit vector

        // 3. Cosine Invariant: sim(feat, feat) == 1.000
        float self_sim = fr::FaceRecognizer::ComputeSimilarity(feat, feat);
        assert(std::abs(self_sim - 1.0f) < 1e-4f);

        // 4. Boundary Rejection Tests
        fr::FaceDetection bad_small = face;
        bad_small.bbox = {100.0f, 100.0f, 130.0f, 130.0f}; // < 50px
        assert(!fr::FaceRecognizer::CheckFaceQuality(bad_small, 640, 640));

        fr::FaceDetection bad_margin = face;
        bad_margin.bbox.x1 = 2.0f; // < 5px margin
        assert(!fr::FaceRecognizer::CheckFaceQuality(bad_margin, 640, 640));

        fr::FaceDetection bad_eyes = face;
        bad_eyes.landmarks[1] = bad_eyes.landmarks[0]; // distance = 0
        assert(!fr::FaceRecognizer::CheckFaceQuality(bad_eyes, 640, 640));

        std::printf("  Iteration %02d/%02d: L2-Norm=%.6f, Self-Similarity=%.6f, QualityChecks=PASSED [PASS]\n",
                    iter, num_iterations, norm, self_sim);
    }
    std::printf("--> ALL %d FACE RECOGNIZER & INVARIANT ITERATIONS PASSED!\n", num_iterations);
}

// ============================================================================
// TEST 3: NAL Parsing, Video Stream & Header Cache Verification (20 Iterations)
// ============================================================================
void RunNalStreamParserTests(int num_iterations = 20) {
    std::printf("\n======================================================================\n");
    std::printf("  [TEST 3] Video Bitstream & NAL Parser Verification (%d Iterations)\n", num_iterations);
    std::printf("======================================================================\n");

    for (int iter = 1; iter <= num_iterations; ++iter) {
        // 1. Synthetic H.264 bitstream with SPS (7), PPS (8), IDR (5), P-Slice (1)
        std::vector<uint8_t> h264_stream = {
            0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x28, 0x8d, 0x95, 0x01, // SPS (type 7)
            0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,                   // PPS (type 8)
            0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10,                   // IDR (type 5)
            0x00, 0x00, 0x01, 0x41, 0x9a, 0x20,                               // Non-IDR P-Slice (type 1)
        };

        auto h264_info = ScanNalStart(h264_stream.data(), h264_stream.size(), false);
        assert(h264_info.sps);
        assert(h264_info.pps);
        assert(h264_info.idr);
        assert(!h264_info.vps);

        std::string h264_ps;
        bool h264_captured = CaptureParamSets(h264_stream.data(), h264_stream.size(), false, h264_ps);
        assert(h264_captured);
        assert(h264_ps.size() >= 15);

        // 2. Synthetic H.265 bitstream with VPS (32), SPS (33), PPS (34), IDR (19), TRAIL_R (1)
        std::vector<uint8_t> h265_stream = {
            0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff,       // VPS (type 32: 0x40 >> 1 = 32)
            0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60,             // SPS (type 33: 0x42 >> 1 = 33)
            0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0,                         // PPS (type 34: 0x44 >> 1 = 34)
            0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x08,                   // IDR (type 19: 0x26 >> 1 = 19)
            0x00, 0x00, 0x01, 0x02, 0x01,                                     // TRAIL_R (type 1: 0x02 >> 1 = 1)
        };

        auto h265_info = ScanNalStart(h265_stream.data(), h265_stream.size(), true);
        assert(h265_info.vps);
        assert(h265_info.sps);
        assert(h265_info.pps);
        assert(h265_info.idr);

        std::string h265_ps;
        bool h265_captured = CaptureParamSets(h265_stream.data(), h265_stream.size(), true, h265_ps);
        assert(h265_captured);
        assert(h265_ps.size() >= 18);

        // 3. Fuzzed / Corrupted stream robustness (Must NOT crash or infinite loop)
        std::vector<uint8_t> corrupted = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
        auto corrupt_info = ScanNalStart(corrupted.data(), corrupted.size(), false);
        (void)corrupt_info;

        std::printf("  Iteration %02d/%02d: H.264 PS=%zu bytes, H.265 PS=%zu bytes, FuzzResistance=OK [PASS]\n",
                    iter, num_iterations, h264_ps.size(), h265_ps.size());
    }
    std::printf("--> ALL %d NAL BITSTREAM & PARSER ITERATIONS PASSED!\n", num_iterations);
}

// ============================================================================
// TEST 4: Date, Segment Naming & Path Traversal Security Fuzzing (20 Iterations)
// ============================================================================
void RunSecurityAndPathFuzzingTests(int num_iterations = 20) {
    std::printf("\n======================================================================\n");
    std::printf("  [TEST 4] Path Security, Date & Traversal Fuzzing (%d Iterations)\n", num_iterations);
    std::printf("======================================================================\n");

    for (int iter = 1; iter <= num_iterations; ++iter) {
        // 1. Valid Dates & Segments
        assert(SafeDate("20260904"));
        assert(SafeDate("20251231"));
        assert(SafeSegment("ai_20260904_120000.h264"));
        assert(SafeSegment("ai_20260904_235959.h265"));

        // 2. Path Traversal & Injection Attack Vectors (Must ALL be REJECTED)
        const std::vector<std::string> malicious_dates = {
            "../2026", "..", "/etc/pas", "20260904/", "2026090", "202609041",
            "2026-09-", "2026/094", "';DROP--", "\0\0\0\0\0\0\0\0", "........"
        };
        for (const auto& md : malicious_dates) {
            assert(!SafeDate(md));
        }

        const std::vector<std::string> malicious_segments = {
            "ai_../../etc/passwd.h264",
            "ai_20260904_120000.h264/../secret",
            "ai_20260904_120000.exe",
            "ai_20260904_120000.sh",
            "ai_20260904_120000",
            "ai_20260904-120000.h264",
            "../ai_20260904_120000.h264",
            "ai_20260904_120000.h264.tmp",
            "segment_20260904_120000.h264",
            ""
        };
        for (const auto& ms : malicious_segments) {
            assert(!SafeSegment(ms));
        }

        // 3. UTC+7 Wall-Clock Timestamp Determinism
        time_t test_time = 1700000000 + iter * 3600;
        std::string ts = VietnamTimestamp(test_time);
        assert(ts.size() == 15);
        assert(ts[8] == '_');

        std::printf("  Iteration %02d/%02d: Tested %zu Traversal Vectors -> 100%% Blocked, Timestamp=%s [PASS]\n",
                    iter, num_iterations, malicious_dates.size() + malicious_segments.size(), ts.c_str());
    }
    std::printf("--> ALL %d PATH SECURITY & TRAVERSAL FUZZING ITERATIONS PASSED!\n", num_iterations);
}

// ============================================================================
// TEST 5: HTTP Protocol, Base64 & JSON Escaping Tests (20 Iterations)
// ============================================================================
void RunHttpProtocolAndEncodingTests(int num_iterations = 20) {
    std::printf("\n======================================================================\n");
    std::printf("  [TEST 5] HTTP Protocol, Base64 & Encoding Tests (%d Iterations)\n", num_iterations);
    std::printf("======================================================================\n");

    for (int iter = 1; iter <= num_iterations; ++iter) {
        // 1. Base64 Authentication Decoding
        assert(Base64Decode("cm9vdDpyb290") == "root:root");
        assert(Base64Decode("cm9vdDoxMjM0NTY=") == "root:123456");
        assert(Base64Decode("").empty());
        assert(Base64Decode("INVALID#@!").empty());

        // 2. URL-Decoding with Vietnamese UTF-8 & Special Chars
        assert(UrlDecode("Nguyen+Van+A") == "Nguyen Van A");
        assert(UrlDecode("Nguy%E1%BB%85n+V%C4%83n+A") == "Nguyễn Văn A");
        assert(UrlDecode("test%20name%2B123") == "test name+123");
        assert(UrlDecode("malformed%2") == "malformed%2");

        // 3. Atomic JSON State File Write/Read Verification
        std::string test_state_file = "/tmp/test_fr_ai_state_" + std::to_string(iter) + ".json";
        std::string json_payload = "{\"running\":true,\"iteration\":" + std::to_string(iter) + ",\"fps\":29.8,\"name\":\"Nguyễn Văn " + std::to_string(iter) + "\"}";

        std::string tmp_file = test_state_file + ".tmp";
        std::ofstream ofs(tmp_file);
        assert(ofs.is_open());
        ofs << json_payload;
        ofs.close();
        assert(rename(tmp_file.c_str(), test_state_file.c_str()) == 0);

        std::ifstream ifs(test_state_file);
        assert(ifs.is_open());
        std::string read_back((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();
        assert(read_back == json_payload);
        unlink(test_state_file.c_str());

        std::printf("  Iteration %02d/%02d: Base64=OK, UrlDecode=OK, AtomicJSON=OK [PASS]\n",
                    iter, num_iterations);
    }
    std::printf("--> ALL %d HTTP PROTOCOL & ENCODING ITERATIONS PASSED!\n", num_iterations);
}

// ============================================================================
// Main Harness
// ============================================================================
int main(int argc, char* argv[]) {
    int iterations = 20;
    if (argc > 1) {
        iterations = std::max(1, std::atoi(argv[1]));
    }

    std::printf("======================================================================\n");
    std::printf("  FACIAL RECOGNITION DEEP MULTI-THREADED TEST SUITE\n");
    std::printf("  Running %d Complete Iterations Across All Subsystems\n", iterations);
    std::printf("======================================================================\n");

    auto t0 = std::chrono::steady_clock::now();

    RunConcurrentDatabaseStressTests(iterations);
    RunFaceRecognizerMathInvariantsTests(iterations);
    RunNalStreamParserTests(iterations);
    RunSecurityAndPathFuzzingTests(iterations);
    RunHttpProtocolAndEncodingTests(iterations);

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n======================================================================\n");
    std::printf("  GRAND SUMMARY: 100%% OF TESTS PASSED ACROSS ALL %d ITERATIONS!\n", iterations);
    std::printf("  Total Execution Time: %lld ms\n", static_cast<long long>(total_ms));
    std::printf("======================================================================\n");

    return 0;
}
