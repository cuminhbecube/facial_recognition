#pragma once

#include "fr_face_recognizer.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fr {

struct PersonRecord {
    std::string id;
    std::string name;
    uint64_t created_at{0};
    std::vector<FaceFeature> features;   // multiple pose/lighting samples per person
};

class FaceDatabase {
public:
    FaceDatabase();
    explicit FaceDatabase(const std::string& db_path);
    ~FaceDatabase();

    bool Load(const std::string& db_path = "");
    bool Save(const std::string& db_path = "");

    // Add a new enrolled person with multiple extracted face feature samples
    bool AddPerson(const std::string& name, const std::vector<FaceFeature>& features, std::string* out_id = nullptr);

    // Delete an enrolled person by ID
    bool DeletePerson(const std::string& id);

    // Get list of all enrolled persons
    std::vector<PersonRecord> ListPersons() const;

    // Find closest matching enrolled person for a detected face feature
    MatchResult FindMatch(const FaceFeature& feature, float threshold = 0.70f) const;

    // Database count
    size_t Count() const;

    // Clear all records
    bool Clear();

    const std::string& GetDbPath() const { return db_path_; }
    void SetDbPath(const std::string& path) { db_path_ = path; }

private:
    std::string db_path_{"/oem/usr/etc/facial-recognition/database.json"};
    std::vector<PersonRecord> records_;
    mutable std::mutex mutex_;
};

}  // namespace fr
