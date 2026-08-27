#include "fr_face_detector.hpp"
#include "fr_face_recognizer.hpp"
#include "fr_face_db.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

void TestFaceDatabase() {
    std::printf("[TEST] Running FaceDatabase Tests...\n");
    const std::string test_db = "/tmp/test_fr_database.json";
    unlink(test_db.c_str());

    fr::FaceDatabase db(test_db);
    assert(db.Count() == 0);

    // Create feature 1 (Person A)
    fr::FaceFeature feat_a;
    for (size_t i = 0; i < fr::kFeatureDim; ++i) {
        feat_a.data[i] = static_cast<float>(i + 1);
    }
    // Normalize
    float norm_a = 0.0f;
    for (float v : feat_a.data) norm_a += v * v;
    norm_a = std::sqrt(norm_a);
    for (float& v : feat_a.data) v /= norm_a;

    // Create feature 2 (Person B)
    fr::FaceFeature feat_b;
    for (size_t i = 0; i < fr::kFeatureDim; ++i) {
        feat_b.data[i] = static_cast<float>(fr::kFeatureDim - i);
    }
    // Normalize
    float norm_b = 0.0f;
    for (float v : feat_b.data) norm_b += v * v;
    norm_b = std::sqrt(norm_b);
    for (float& v : feat_b.data) v /= norm_b;

    std::string id_a, id_b;
    assert(db.AddPerson("Nguyễn Văn A", {feat_a}, &id_a));
    assert(!id_a.empty());
    assert(db.AddPerson("Trần Thị B", {feat_b}, &id_b));
    assert(!id_b.empty());
    assert(db.Count() == 2);

    // Test Search Exact Match A
    auto match_a = db.FindMatch(feat_a, 0.70f);
    assert(match_a.matched);
    assert(match_a.name == "Nguyễn Văn A");
    assert(match_a.similarity >= 0.99f);
    std::printf("  - Exact match Person A: %s (Similarity: %.4f) [PASSED]\n",
                match_a.name.c_str(), match_a.similarity);

    // Test Search Exact Match B
    auto match_b = db.FindMatch(feat_b, 0.70f);
    assert(match_b.matched);
    assert(match_b.name == "Trần Thị B");
    assert(match_b.similarity >= 0.99f);
    std::printf("  - Exact match Person B: %s (Similarity: %.4f) [PASSED]\n",
                match_b.name.c_str(), match_b.similarity);

    // Test Search with Noisy Vector of Person A
    fr::FaceFeature feat_noisy = feat_a;
    for (size_t i = 0; i < fr::kFeatureDim; ++i) {
        feat_noisy.data[i] += 0.02f * ((i % 2 == 0) ? 1.0f : -1.0f);
    }
    auto match_noisy = db.FindMatch(feat_noisy, 0.70f);
    assert(match_noisy.matched);
    assert(match_noisy.name == "Nguyễn Văn A");
    assert(match_noisy.similarity > 0.85f);
    std::printf("  - Noisy match Person A: %s (Similarity: %.4f) [PASSED]\n",
                match_noisy.name.c_str(), match_noisy.similarity);

    // Test Search with Unknown/Random Vector
    fr::FaceFeature feat_unknown;
    for (size_t i = 0; i < fr::kFeatureDim; ++i) {
        feat_unknown.data[i] = ((i % 3 == 0) ? -1.0f : 1.0f);
    }
    float norm_u = 0.0f;
    for (float v : feat_unknown.data) norm_u += v * v;
    norm_u = std::sqrt(norm_u);
    for (float& v : feat_unknown.data) v /= norm_u;

    auto match_unknown = db.FindMatch(feat_unknown, 0.70f);
    assert(!match_unknown.matched);
    std::printf("  - Unknown person rejection: Matched=%s (Similarity: %.4f) [PASSED]\n",
                match_unknown.matched ? "true" : "false", match_unknown.similarity);

    // Test Persistence by reloading from file
    fr::FaceDatabase db_reload(test_db);
    assert(db_reload.Count() == 2);
    auto persons = db_reload.ListPersons();
    assert(persons.size() == 2);
    std::printf("  - Database reload from file: %zu records [PASSED]\n", persons.size());

    // Test Delete Person
    assert(db.DeletePerson(id_a));
    assert(db.Count() == 1);
    auto match_after_del = db.FindMatch(feat_a, 0.70f);
    assert(!match_after_del.matched);
    std::printf("  - Delete Person A: Count=%zu [PASSED]\n", db.Count());

    unlink(test_db.c_str());
    std::printf("[TEST] FaceDatabase Tests PASSED!\n\n");
}

void TestFaceRecognizer() {
    std::printf("[TEST] Running FaceRecognizer Tests...\n");

    fr::FaceDetection face;
    face.bbox = {100.0f, 100.0f, 300.0f, 320.0f};
    face.score = 0.95f;
    face.landmarks[0] = {150.0f, 170.0f};  // Left eye
    face.landmarks[1] = {250.0f, 170.0f};  // Right eye
    face.landmarks[2] = {200.0f, 220.0f};  // Nose
    face.landmarks[3] = {160.0f, 270.0f};  // Left mouth
    face.landmarks[4] = {240.0f, 270.0f};  // Right mouth

    std::string reason;
    bool quality_ok = fr::FaceRecognizer::CheckFaceQuality(face, 640, 640, &reason);
    assert(quality_ok);
    std::printf("  - Quality check on valid face: %s [PASSED]\n", quality_ok ? "OK" : reason.c_str());

    // Quality check on small face
    fr::FaceDetection small_face = face;
    small_face.bbox = {100.0f, 100.0f, 130.0f, 130.0f};
    bool small_ok = fr::FaceRecognizer::CheckFaceQuality(small_face, 640, 640, &reason);
    assert(!small_ok);
    std::printf("  - Quality check on small face correctly rejected: '%s' [PASSED]\n", reason.c_str());

    // Feature extraction on synthetic buffer
    std::vector<uint8_t> img(640 * 640 * 3, 128);
    fr::FaceRecognizer recognizer;
    fr::FaceFeature feat;
    assert(recognizer.ExtractFeature(img.data(), 640, 640, face, feat));
    assert(feat.IsValid());

    float sim_self = fr::FaceRecognizer::ComputeSimilarity(feat, feat);
    assert(sim_self >= 0.999f);
    std::printf("  - Self-similarity calculation: %.4f [PASSED]\n", sim_self);

    std::printf("[TEST] FaceRecognizer Tests PASSED!\n\n");
}

int main() {
    std::printf("========================================\n");
    std::printf("  Facial Recognition AI Test Suite\n");
    std::printf("========================================\n\n");

    TestFaceDatabase();
    TestFaceRecognizer();

    std::printf("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
