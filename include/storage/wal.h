#pragma once
// ────────────────────────────────────────────────────────────────
// Write-Ahead Log: crash-safe sequential append log.
// Format per record:  [CRC32:4][Length:4][Type:1][Data:Length]
// ────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "../compat/threading.h"

namespace dcs {
namespace storage {

enum class WALRecordType : uint8_t {
    kPut    = 0x01,
    kDelete = 0x02,
    kBatch  = 0x03,
};

struct WALRecord {
    WALRecordType type;
    std::string   key;
    std::string   value;
    uint64_t      sequence;
};

class WALWriter {
public:
    explicit WALWriter(const std::string& filepath)
        : filepath_(filepath), bytes_written_(0) {
        file_.open(filepath, std::ios::binary | std::ios::app);
    }

    ~WALWriter() {
        if (file_.is_open()) file_.close();
    }

    bool Append(const WALRecord& record) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        std::string serialized = Serialize(record);
        return WriteFrame(serialized);
    }

    bool AppendBatch(const std::vector<WALRecord>& records) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        for (const auto& rec : records) {
            std::string serialized = Serialize(rec);
            if (!WriteFrame(serialized)) return false;
        }
        return Sync();
    }

    bool Sync() {
        if (!file_.is_open()) return false;
        file_.flush();
        return file_.good();
    }

    size_t BytesWritten() const { return bytes_written_; }
    const std::string& Filepath() const { return filepath_; }

    void Close() {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

private:
    bool WriteFrame(const std::string& payload) {
        if (!file_.is_open()) return false;
        uint32_t length = static_cast<uint32_t>(payload.size());
        uint32_t crc    = ComputeCRC(payload);
        file_.write(reinterpret_cast<const char*>(&crc), 4);
        file_.write(reinterpret_cast<const char*>(&length), 4);
        file_.write(payload.data(), length);
        bytes_written_ += 8 + length;
        return file_.good();
    }

    static std::string Serialize(const WALRecord& rec) {
        std::string buf;
        buf.reserve(1 + 8 + 4 + rec.key.size() + 4 + rec.value.size());
        buf.push_back(static_cast<char>(rec.type));
        buf.append(reinterpret_cast<const char*>(&rec.sequence), 8);
        uint32_t key_len = static_cast<uint32_t>(rec.key.size());
        buf.append(reinterpret_cast<const char*>(&key_len), 4);
        buf.append(rec.key);
        uint32_t val_len = static_cast<uint32_t>(rec.value.size());
        buf.append(reinterpret_cast<const char*>(&val_len), 4);
        buf.append(rec.value);
        return buf;
    }

    static uint32_t ComputeCRC(const std::string& data) {
        uint32_t crc = 0;
        for (unsigned char c : data) {
            crc = (crc >> 8) ^ ((crc ^ c) * 0x01000193);
        }
        return crc;
    }

    std::string       filepath_;
    std::ofstream     file_;
    size_t            bytes_written_;
    compat::Mutex     mu_;
};

class WALReader {
public:
    explicit WALReader(const std::string& filepath) : filepath_(filepath) {}

    using RecordCallback = std::function<void(const WALRecord&)>;

    size_t Replay(RecordCallback cb) {
        std::ifstream file(filepath_, std::ios::binary);
        if (!file.is_open()) return 0;
        size_t count = 0;
        while (file.good() && !file.eof()) {
            uint32_t stored_crc = 0, length = 0;
            file.read(reinterpret_cast<char*>(&stored_crc), 4);
            file.read(reinterpret_cast<char*>(&length), 4);
            if (!file.good() || length == 0 || length > 64 * 1024 * 1024) break;
            std::string payload(length, '\0');
            file.read(&payload[0], length);
            if (!file.good()) break;
            uint32_t computed = ComputeCRC(payload);
            if (computed != stored_crc) break;
            WALRecord rec = Deserialize(payload);
            cb(rec);
            ++count;
        }
        return count;
    }

private:
    static uint32_t ComputeCRC(const std::string& data) {
        uint32_t crc = 0;
        for (unsigned char c : data) {
            crc = (crc >> 8) ^ ((crc ^ c) * 0x01000193);
        }
        return crc;
    }

    static WALRecord Deserialize(const std::string& payload) {
        WALRecord rec;
        size_t pos = 0;
        rec.type = static_cast<WALRecordType>(payload[pos++]);
        std::memcpy(&rec.sequence, payload.data() + pos, 8); pos += 8;
        uint32_t key_len = 0;
        std::memcpy(&key_len, payload.data() + pos, 4); pos += 4;
        rec.key = payload.substr(pos, key_len); pos += key_len;
        uint32_t val_len = 0;
        std::memcpy(&val_len, payload.data() + pos, 4); pos += 4;
        rec.value = payload.substr(pos, val_len);
        return rec;
    }

    std::string filepath_;
};

}  // namespace storage
}  // namespace dcs
