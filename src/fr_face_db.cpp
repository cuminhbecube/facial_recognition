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
#include <map>
#include <sstream>
#include <utility>

namespace fr {

namespace {

constexpr const char* kUserdataDbPath =
    "/userdata/facial-recognition/database.json";

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
    feat.data.clear();
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

static bool EnsureParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;
    std::string dir = path.substr(0, slash);
    std::string current;
    std::istringstream ss(dir);
    std::string part;
    if (dir[0] == '/') current = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        current += part + "/";
        mkdir(current.c_str(), 0755);
    }
    return true;
}

static bool AtomicWriteFile(const std::string& target_path, const std::string& data) {
    if (!EnsureParentDir(target_path)) return false;
    std::string tmp_path = target_path + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    ssize_t written = write(fd, data.data(), data.size());
    fsync(fd);
    close(fd);
    if (written != static_cast<ssize_t>(data.size())) {
        unlink(tmp_path.c_str());
        return false;
    }
    if (rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

static bool ParseDbContent(const std::string& content, std::vector<PersonRecord>& out_records) {
    out_records.clear();
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

        // Parse "features" array-of-arrays
        size_t feats_key = obj_str.find("\"features\"");
        if (feats_key != std::string::npos) {
            size_t f_arr_start = obj_str.find('[', feats_key);
            if (f_arr_start != std::string::npos) {
                std::string inner = obj_str.substr(f_arr_start + 1);
                size_t p = 0;
                while (p < inner.size()) {
                    size_t sub_start = inner.find('[', p);
                    if (sub_start == std::string::npos) break;
                    size_t sub_end = inner.find(']', sub_start);
                    if (sub_end == std::string::npos) break;
                    rec.features.push_back(ParseFeatureArray(
                        inner.substr(sub_start + 1, sub_end - sub_start - 1)));
                    p = sub_end + 1;
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
            out_records.push_back(rec);
        }

        pos = obj_end + 1;
    }
    return true;
}

// Serialize persons to the on-disk JSON format. Shared by Save() and by the
// migration that re-homes the active DB onto the persistent /userdata volume.
std::string BuildPersonsJson(const std::vector<PersonRecord>& records) {
    std::ostringstream json;
    json << "{\n  \"version\": 1,\n  \"count\": " << records.size()
         << ",\n  \"persons\": [\n";

    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
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
        json << "]\n    }" << (i + 1 < records.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    return json.str();
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

    std::vector<std::string> candidate_paths = {
        db_path_,
        "/userdata/facial-recognition/database.json",
        "/oem/usr/etc/facial-recognition/database.json",
        "/data/facial-recognition/database.json",
        "config/database.json",
        "/tmp/database.json"
    };

    // Merge every readable copy instead of first-file-wins. A stale shadow
    // copy previously masked the live store, so enrolled faces appeared to be
    // "forgotten" after reboots. Same-id duplicates are deduplicated, keeping
    // the instance with the most samples.
    std::vector<PersonRecord> merged;
    std::map<std::string, size_t> index_by_id;
    std::string first_loaded;

    for (const auto& path : candidate_paths) {
        if (path.empty() || access(path.c_str(), R_OK) != 0) continue;

        std::ifstream file(path);
        if (!file.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();

        std::vector<PersonRecord> parsed;
        if (!ParseDbContent(content, parsed)) continue;
        if (first_loaded.empty()) first_loaded = path;

        for (auto& rec : parsed) {
            auto it = index_by_id.find(rec.id);
            if (it == index_by_id.end()) {
                index_by_id.emplace(rec.id, merged.size());
                merged.push_back(std::move(rec));
                continue;
            }
            PersonRecord& cur = merged[it->second];
            if (rec.features.size() > cur.features.size()) {
                cur.features = std::move(rec.features);
                if (rec.created_at) cur.created_at = rec.created_at;
                if (!rec.name.empty()) cur.name = rec.name;
            }
        }
    }

    if (!first_loaded.empty() || !merged.empty()) {
        records_ = std::move(merged);
        db_path_ = first_loaded.empty() ? db_path_ : first_loaded;

        // Re-home to the persistent writable volume so later enrollments never
        // target the reflashable firmware partition (which would also make
        // faces vanish on the next update).
        if (db_path_ != kUserdataDbPath && access("/userdata", W_OK) == 0) {
            AtomicWriteFile(kUserdataDbPath, BuildPersonsJson(records_));
            db_path_ = kUserdataDbPath;
        }
        std::printf("[FaceDatabase] Loaded %zu persons from %s\n",
                    records_.size(), db_path_.c_str());
    } else {
        records_.clear();
        db_path_ = kUserdataDbPath;
        if (access("/userdata", W_OK) == 0) {
            AtomicWriteFile(kUserdataDbPath, BuildPersonsJson(records_));
        }
        std::printf("[FaceDatabase] No existing database found at %s. Initialized empty.\n",
                    db_path_.c_str());
    }

    return true;
}

bool FaceDatabase::Save(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_path.empty()) db_path_ = db_path;

    const std::string json_str = BuildPersonsJson(records_);

    // Primary write
    bool ok = AtomicWriteFile(db_path_, json_str);

    // If primary failed (e.g. read-only filesystem on /oem), fallback to writable persistent path
    if (!ok) {
        std::vector<std::string> fallbacks = {
            kUserdataDbPath,
            "/data/facial-recognition/database.json",
            "/tmp/database.json"
        };
        for (const auto& fb : fallbacks) {
            if (fb == db_path_) continue;
            if (AtomicWriteFile(fb, json_str)) {
                std::printf("[FaceDatabase] Primary save failed for %s. Fallback saved to %s\n",
                            db_path_.c_str(), fb.c_str());
                db_path_ = fb;
                ok = true;
                break;
            }
        }
    } else {
        // Also mirror to persistent /userdata if primary was /oem
        if (db_path_ != kUserdataDbPath &&
            (access("/userdata", W_OK) == 0 || access("/data", W_OK) == 0)) {
            AtomicWriteFile(kUserdataDbPath, json_str);
        }
    }

    if (!ok) {
        std::fprintf(stderr, "[FaceDatabase] CRITICAL: Failed to save database to any persistent path!\n");
    }

    return ok;
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
