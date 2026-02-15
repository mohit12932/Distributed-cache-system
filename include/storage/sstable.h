#pragma once
// ────────────────────────────────────────────────────────────────
// SSTable: Sorted String Table — immutable on-disk key-value storage.
// Format:
//   [DataBlock 0][DataBlock 1]...[IndexBlock][MetaBlock(Bloom)][Footer]
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dcs {
namespace storage {

// ──── Bloom Filter ─────────────────────────────────────────────
class BloomFilter {
public:
    explicit BloomFilter(size_t num_keys, double fp_rate = 0.01)
        : num_hashes_(OptimalHashes(fp_rate)) {
        size_t num_bits = std::max<size_t>(64, num_keys * 10);
        bits_.resize((num_bits + 7) / 8, 0);
        num_bits_ = bits_.size() * 8;
    }

    BloomFilter() : num_bits_(0), num_hashes_(7) {}

    void Add(const std::string& key) {
        for (int i = 0; i < num_hashes_; i++) {
            size_t h = Hash(key, i) % num_bits_;
            bits_[h / 8] |= (1u << (h % 8));
        }
    }

    bool MayContain(const std::string& key) const {
        for (int i = 0; i < num_hashes_; i++) {
            size_t h = Hash(key, i) % num_bits_;
            if (!(bits_[h / 8] & (1u << (h % 8)))) return false;
        }
        return true;
    }

    std::string Serialize() const {
        std::string buf;
        uint32_t n = static_cast<uint32_t>(bits_.size());
        buf.append(reinterpret_cast<const char*>(&num_hashes_), 4);
        buf.append(reinterpret_cast<const char*>(&n), 4);
        buf.append(reinterpret_cast<const char*>(bits_.data()), n);
        return buf;
    }

    static BloomFilter Deserialize(const std::string& data) {
        BloomFilter bf;
        int nh = 0; uint32_t n = 0;
        std::memcpy(&nh, data.data(), 4);
        std::memcpy(&n, data.data() + 4, 4);
        bf.num_hashes_ = nh;
        bf.bits_.assign(data.data() + 8, data.data() + 8 + n);
        bf.num_bits_ = n * 8;
        return bf;
    }

private:
    static int OptimalHashes(double fp) {
        return std::max(1, static_cast<int>(-std::log(fp) / std::log(2)));
    }

    static size_t Hash(const std::string& key, int seed) {
        size_t h = static_cast<size_t>(seed) * 0x9e3779b97f4a7c15ULL;
        for (unsigned char c : key) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    std::vector<uint8_t> bits_;
    size_t               num_bits_;
    int                  num_hashes_;
};

// ──── Block Handle ─────────────────────────────────────────────
struct BlockHandle {
    uint64_t offset;
    uint64_t size;
};

// ──── Footer ───────────────────────────────────────────────────
struct Footer {
    BlockHandle index_handle;
    BlockHandle meta_handle;
    uint64_t    num_entries;
    uint64_t    magic = 0xDC5F00DAULL;

    std::string Serialize() const {
        std::string buf(sizeof(Footer), '\0');
        std::memcpy(&buf[0], this, sizeof(Footer));
        return buf;
    }
    static Footer Deserialize(const std::string& data) {
        Footer f;
        std::memcpy(&f, data.data(), sizeof(Footer));
        return f;
    }
};

// ──── SSTable Writer ───────────────────────────────────────────
class SSTableWriter {
public:
    explicit SSTableWriter(const std::string& filepath)
        : filepath_(filepath), current_offset_(0), entry_count_(0) {
        file_.open(filepath, std::ios::binary | std::ios::trunc);
    }

    bool Add(const std::string& key, const std::string& value) {
        if (!file_.is_open()) return false;
        entries_.push_back({key, value});
        bloom_.Add(key);
        entry_count_++;
        return true;
    }

    bool Finish() {
        if (!file_.is_open()) return false;
        // Sort entries
        std::sort(entries_.begin(), entries_.end(),
            [](const KV& a, const KV& b) { return a.key < b.key; });

        // Write data blocks (one entry per record for simplicity)
        std::vector<std::pair<std::string, BlockHandle>> index_entries;
        for (const auto& kv : entries_) {
            BlockHandle bh;
            bh.offset = current_offset_;
            std::string record = EncodeKV(kv.key, kv.value);
            bh.size = record.size();
            file_.write(record.data(), record.size());
            current_offset_ += record.size();
            index_entries.push_back({kv.key, bh});
        }

        // Write index block
        BlockHandle index_handle;
        index_handle.offset = current_offset_;
        std::string index_data = EncodeIndex(index_entries);
        file_.write(index_data.data(), index_data.size());
        index_handle.size = index_data.size();
        current_offset_ += index_data.size();

        // Write bloom filter (meta block)
        BlockHandle meta_handle;
        meta_handle.offset = current_offset_;
        std::string bloom_data = bloom_.Serialize();
        file_.write(bloom_data.data(), bloom_data.size());
        meta_handle.size = bloom_data.size();
        current_offset_ += bloom_data.size();

        // Write footer
        Footer footer;
        footer.index_handle = index_handle;
        footer.meta_handle  = meta_handle;
        footer.num_entries  = entry_count_;
        std::string footer_data = footer.Serialize();
        file_.write(footer_data.data(), footer_data.size());
        file_.flush();
        file_.close();
        return true;
    }

    size_t EntryCount() const { return entry_count_; }

private:
    struct KV { std::string key, value; };

    static std::string EncodeKV(const std::string& key, const std::string& value) {
        std::string buf;
        uint32_t klen = static_cast<uint32_t>(key.size());
        uint32_t vlen = static_cast<uint32_t>(value.size());
        buf.append(reinterpret_cast<const char*>(&klen), 4);
        buf.append(key);
        buf.append(reinterpret_cast<const char*>(&vlen), 4);
        buf.append(value);
        return buf;
    }

    static std::string EncodeIndex(const std::vector<std::pair<std::string, BlockHandle>>& entries) {
        std::string buf;
        uint32_t n = static_cast<uint32_t>(entries.size());
        buf.append(reinterpret_cast<const char*>(&n), 4);
        for (const auto& e : entries) {
            uint32_t klen = static_cast<uint32_t>(e.first.size());
            buf.append(reinterpret_cast<const char*>(&klen), 4);
            buf.append(e.first);
            buf.append(reinterpret_cast<const char*>(&e.second.offset), 8);
            buf.append(reinterpret_cast<const char*>(&e.second.size), 8);
        }
        return buf;
    }

    std::string        filepath_;
    std::ofstream      file_;
    uint64_t           current_offset_;
    size_t             entry_count_;
    std::vector<KV>    entries_;
    BloomFilter        bloom_{1024};
};

// ──── SSTable Reader ───────────────────────────────────────────
class SSTableReader {
public:
    explicit SSTableReader(const std::string& filepath) : filepath_(filepath) {
        Load();
    }

    bool Get(const std::string& key, std::string& value) const {
        if (!valid_ || !bloom_.MayContain(key)) return false;
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        return ReadKVAt(it->second, key, value);
    }

    bool Valid()  const { return valid_; }
    size_t Size() const { return index_.size(); }
    const std::string& Filepath() const { return filepath_; }

    std::vector<std::string> AllKeys() const {
        std::vector<std::string> keys;
        keys.reserve(index_.size());
        for (const auto& kv : index_) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    }

private:
    void Load() {
        std::ifstream file(filepath_, std::ios::binary | std::ios::ate);
        if (!file.is_open()) { valid_ = false; return; }

        auto file_size = file.tellg();
        if (static_cast<size_t>(file_size) < sizeof(Footer)) { valid_ = false; return; }

        // Read footer
        file.seekg(-static_cast<int>(sizeof(Footer)), std::ios::end);
        std::string footer_buf(sizeof(Footer), '\0');
        file.read(&footer_buf[0], sizeof(Footer));
        Footer footer = Footer::Deserialize(footer_buf);
        if (footer.magic != 0xDC5F00DAULL) { valid_ = false; return; }

        // Read bloom filter
        file.seekg(footer.meta_handle.offset);
        std::string bloom_buf(footer.meta_handle.size, '\0');
        file.read(&bloom_buf[0], footer.meta_handle.size);
        bloom_ = BloomFilter::Deserialize(bloom_buf);

        // Read index
        file.seekg(footer.index_handle.offset);
        std::string index_buf(footer.index_handle.size, '\0');
        file.read(&index_buf[0], footer.index_handle.size);
        DecodeIndex(index_buf);

        valid_ = true;
    }

    void DecodeIndex(const std::string& data) {
        size_t pos = 0;
        uint32_t n = 0;
        std::memcpy(&n, data.data(), 4); pos += 4;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t klen = 0;
            std::memcpy(&klen, data.data() + pos, 4); pos += 4;
            std::string key(data.data() + pos, klen); pos += klen;
            BlockHandle bh;
            std::memcpy(&bh.offset, data.data() + pos, 8); pos += 8;
            std::memcpy(&bh.size, data.data() + pos, 8); pos += 8;
            index_[key] = bh;
        }
    }

    bool ReadKVAt(const BlockHandle& bh, const std::string& expected_key,
                  std::string& value) const {
        std::ifstream file(filepath_, std::ios::binary);
        if (!file.is_open()) return false;
        file.seekg(bh.offset);
        uint32_t klen = 0;
        file.read(reinterpret_cast<char*>(&klen), 4);
        std::string key(klen, '\0');
        file.read(&key[0], klen);
        if (key != expected_key) return false;
        uint32_t vlen = 0;
        file.read(reinterpret_cast<char*>(&vlen), 4);
        value.resize(vlen);
        file.read(&value[0], vlen);
        return file.good();
    }

    std::string filepath_;
    bool        valid_ = false;
    BloomFilter bloom_;
    std::unordered_map<std::string, BlockHandle> index_;
};

}  // namespace storage
}  // namespace dcs
