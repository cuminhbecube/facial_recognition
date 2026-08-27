#include "fr_face_db.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fr {

namespace {

std::string EscapeJson(const std::string& str) {
    std::ostringstream ss;
    for (char c : str) {
        if (c == '"') ss << "\\\"";
        else if (c == '\\') ss << "\\\\";
        else if (c == '\b') ss << "\\b";
        else if (c == '\f') ss << "\\f";
        else if (c == '\n') ss << "\\n";
        else if (c == '\r') ss << "\\r";
        else if (c == '\t') ss << "\\t";
        else if (static_cast<unsigned char>(c) >= 0x20) ss << c;
    }
    return ss.str();
}

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

uint64_t CurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string GenerateId(const std::string& /*name*/) {
    uint64_t ts = CurrentTimestamp();
    static uint32_t counter = 100;
    ++counter;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "usr_%llu_%u", static_cast<unsigned long long>(ts), counter % 10000);
    return std::string(buf);
}

FaceFeature ParseFeatureArray(const std::string& f_str) {
    FaceFeature feat;
    std::istringstream fss(f_str);
    std::string token;
    while (std::getline(fss, token, ',')) {
        token = Trim(token);
        if (!token.empty()) {
            feat.data.push_back(std::strtof(token.c_str(), nullptr));
        }
    }
    return feat;
}

}  // namespace

FaceDatabase::FaceDatabase() = default;

FaceDatabase::FaceDatabase(const std::string& db_path) : db_path_(db_path) {
    Load(db_path_);
}

FaceDatabase::~FaceDatabase() = default;

bool FaceDatabase::Load(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_path.empty()) db_path_ = db_path;

    records_.clear();

    std::ifstream file(db_path_);
    if (!file.is_open()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    // Simple custom JSON parser for database format
    // Format: {"version":1,"persons":[{"id":"...","name":"...","created_at":123,"feature":[...]}, ...]}
    size_t persons_pos = content.find("\"persons\"");
    if (persons_pos == std::string::npos) return true;

    size_t arr_start = content.find('[', persons_pos);
    if (arr_start == std::string::npos) return true;

    size_t pos = arr_start + 1;
    while (pos < content.size()) {
        size_t obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;

        size_t obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj_str = content.substr(obj_start, obj_end - obj_start + 1);

        PersonRecord rec;

        // Parse id
        size_t id_key = obj_str.find("\"id\"");
        if (id_key != std::string::npos) {
            size_t val_start = obj_str.find('"', id_key + 4);
            if (val_start != std::string::npos) {
                size_t val_end = obj_str.find('"', val_start + 1);
                if (val_end != std::string::npos) {
                    rec.id = obj_str.substr(val_start + 1, val_end - val_start - 1);
                }
            }
        }

        // Parse name
        size_t name_key = obj_str.find("\"name\"");
        if (name_key != std::string::npos) {
            size_t val_start = obj_str.find('"', name_key + 6);
            if (val_start != std::string::npos) {
                size_t val_end = obj_str.find('"', val_start + 1);
                if (val_end != std::string::npos) {
                    rec.name = obj_str.substr(val_start + 1, val_end - val_start - 1);
                }
            }
        }

        // Parse created_at
        size_t time_key = obj_str.find("\"created_at\"");
        if (time_key != std::string::npos) {
            size_t val_start = obj_str.find(':', time_key + 12);
            if (val_start != std::string::npos) {
                rec.created_at = std::strtoull(obj_str.c_str() + val_start + 1, nullptr, 10);
            }
        }

        // Parse "features" array-of-arrays (new multi-sample format)
        size_t feats_key = obj_str.find("\"features\"");
        if (feats_key != std::string::npos) {
            size_t f_arr_start = obj_str.find('[', feats_key);
            // Find the opening [ after the key, then iterate nested arrays
            if (f_arr_start != std::string::npos) {
                std::string inner = obj_str.substr(f_arr_start + 1);
                size_t pos = 0;
                while (pos < inner.size()) {
                    size_t sub_start = inner.find('[', pos);
                    if (sub_start == std::string::npos) break;
                    size_t sub_end = inner.find(']', sub_start);
                    if (sub_end == std::string::npos) break;
                    rec.features.push_back(ParseFeatureArray(
                        inner.substr(sub_start + 1, sub_end - sub_start - 1)));
                    pos = sub_end + 1;
                }
            }
        }

        // Backward compatibility: single "feature" array
        if (rec.features.empty()) {
            size_t feat_key = obj_str.find("\"feature\"");
            if (feat_key != std::string::npos) {
                size_t f_start = obj_str.find('[', feat_key);
                size_t f_end = obj_str.find(']', f_start);
                if (f_start != std::string::npos && f_end != std::string::npos) {
                    rec.features.push_back(ParseFeatureArray(
                        obj_str.substr(f_start + 1, f_end - f_start - 1)));
                }
            }
        }

        if (!rec.name.empty() && !rec.features.empty()) {
            if (rec.id.empty()) rec.id = GenerateId(rec.name);
            records_.push_back(rec);
        }

        pos = obj_end + 1;
    }

    std::printf("[FaceDatabase] Loaded %zu persons from %s\n", records_.size(), db_path_.c_str());
    return true;
}

bool FaceDatabase::Save(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_path.empty()) db_path_ = db_path;

    std::ostringstream json;
    json << "{\n  \"version\": 1,\n  \"count\": " << records_.size() << ",\n  \"persons\": [\n";

    for (size_t i = 0; i < records_.size(); ++i) {
        const auto& rec = records_[i];
        json << "    {\n"
             << "      \"id\": \"" << EscapeJson(rec.id) << "\",\n"
             << "      \"name\": \"" << EscapeJson(rec.name) << "\",\n"
             << "      \"created_at\": " << rec.created_at << ",\n"
             << "      \"features\": [";

        for (size_t s = 0; s < rec.features.size(); ++s) {
            if (s) json << ", ";
            json << "[";
            const auto& f = rec.features[s].data;
            for (size_t i = 0; i < f.size(); ++i) {
                if (i) json << ", ";
                json << std::fixed << std::setprecision(6) << f[i];
            }
            json << "]";
        }
        json << "]\n    }" << (i + 1 < records_.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";

    std::string json_str = json.str();
    std::string tmp_path = db_path_ + ".tmp";

    // Atomic write
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[FaceDatabase] Failed to open temp file for writing: %s\n", tmp_path.c_str());
        return false;
    }

    ssize_t written = write(fd, json_str.data(), json_str.size());
    fsync(fd);
    close(fd);

    if (written != static_cast<ssize_t>(json_str.size())) {
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), db_path_.c_str()) != 0) {
        unlink(tmp_path.c_str());
        return false;
    }

    return true;
}

bool FaceDatabase::AddPerson(const std::string& name, const std::vector<FaceFeature>& features, std::string* out_id) {
    std::vector<FaceFeature> valid;
    for (const auto& f : features) {
        if (f.IsValid()) valid.push_back(f);
    }
    if (name.empty() || valid.empty()) return false;

    PersonRecord rec;
    rec.id = GenerateId(name);
    rec.name = name;
    rec.created_at = CurrentTimestamp();
    rec.features = std::move(valid);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(rec);
    }

    if (out_id) *out_id = rec.id;
    Save();
    std::printf("[FaceDatabase] Enrolled new person: '%s' (ID: %s, samples=%zu)\n",
               name.c_str(), rec.id.c_str(), rec.features.size());
    return true;
}

bool FaceDatabase::DeletePerson(const std::string& id) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::remove_if(records_.begin(), records_.end(), [&](const PersonRecord& r) {
            return r.id == id;
        });
        if (it != records_.end()) {
            records_.erase(it, records_.end());
            found = true;
        }
    }

    if (found) {
        Save();
        std::printf("[FaceDatabase] Deleted person ID: %s\n", id.c_str());
    }
    return found;
}

std::vector<PersonRecord> FaceDatabase::ListPersons() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

MatchResult FaceDatabase::FindMatch(const FaceFeature& feature, float threshold) const {
    MatchResult result;
    result.matched = false;
    result.similarity = 0.0f;

    if (!feature.IsValid()) return result;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& rec : records_) {
        // Best-of-N: similarity to a person is the max over all enrolled samples
        float best = 0.0f;
        for (const auto& sample : rec.features) {
            float s = FaceRecognizer::ComputeSimilarity(feature, sample);
            if (s > best) best = s;
        }
        if (best > result.similarity) {
            result.similarity = best;
            result.person_id = rec.id;
            result.name = rec.name;
        }
    }

    if (result.similarity >= threshold) {
        result.matched = true;
    }

    return result;
}

size_t FaceDatabase::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

bool FaceDatabase::Clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
    }
    return Save();
}

}  // namespace fr
