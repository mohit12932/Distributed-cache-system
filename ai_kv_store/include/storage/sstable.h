// ai_kv_store/include/storage/sstable.h
// ────────────────────────────────────────────────────────────────
// Sorted String Table: immutable on-disk sorted key-value file.
//
// File Format:
//   [DataBlock 0][DataBlock 1]...[DataBlock N]
//   [MetaBlock (bloom filter)]
//   [IndexBlock (block handles)]
//   [Footer (offsets + magic)]
//
// DataBlock Format:
//   [Entry 0][Entry 1]...[Entry K][RestartArray][NumRestarts:4]
//   Entry: [SharedLen:varint][NonSharedLen:varint][ValLen:varint]
//          [NonSharedKey][Value]
// ────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "memtable.h"  // For InternalKey, ValueType

namespace ai_kv {
namespace storage {

// ── Bloom Filter (for SSTable) ──

class BloomFilter {
public:
    explicit BloomFilter(size_t expected_keys, double fp_rate = 0.01)
        : num_hashes_(OptimalHashes(fp_rate)),
          bits_(OptimalBits(expected_keys, fp_rate), false) {}

    // Reconstructed from serialized bits
    BloomFilter(std::vector<bool> bits, uint32_t num_hashes)
        : num_hashes_(num_hashes), bits_(std::move(bits)) {}

    void Add(absl::string_view key) {
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            size_t idx = Hash(key, i) % bits_.size();
            bits_[idx] = true;
        }
    }

    bool MayContain(absl::string_view key) const {
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            size_t idx = Hash(key, i) % bits_.size();
            if (!bits_[idx]) return false;
        }
        return true;
    }

    // Serialize to string for on-disk storage
    std::string Serialize() const {
        std::string out;
        uint32_t num_bits = static_cast<uint32_t>(bits_.size());
        out.append(reinterpret_cast<const char*>(&num_bits), 4);
        out.append(reinterpret_cast<const char*>(&num_hashes_), 4);
        // Pack bits into bytes
        size_t num_bytes = (num_bits + 7) / 8;
        out.resize(8 + num_bytes, '\0');
        for (size_t i = 0; i < num_bits; ++i) {
            if (bits_[i]) {
                out[8 + i / 8] |= (1 << (i % 8));
            }
        }
        return out;
    }

    static BloomFilter Deserialize(const std::string& data) {
        uint32_t num_bits = 0, num_hashes = 0;
        std::memcpy(&num_bits, data.data(), 4);
        std::memcpy(&num_hashes, data.data() + 4, 4);
        std::vector<bool> bits(num_bits, false);
        for (size_t i = 0; i < num_bits; ++i) {
            if (data[8 + i / 8] & (1 << (i % 8))) {
                bits[i] = true;
            }
        }
        return BloomFilter(std::move(bits), num_hashes);
    }

private:
    static size_t Hash(absl::string_view key, uint32_t seed) {
        // MurmurHash3-inspired mixing
        size_t h = seed * 0xcc9e2d51;
        for (char c : key) {
            h ^= static_cast<size_t>(c);
            h *= 0x01000193;
            h ^= (h >> 16);
        }
        return h;
    }

    static uint32_t OptimalHashes(double fp) {
        return std::max(1u, static_cast<uint32_t>(-std::log(fp) / std::log(2.0)));
    }

    static size_t OptimalBits(size_t n, double fp) {
        double ln2 = 0.693147;
        return std::max(size_t(64),
                        static_cast<size_t>(-(double)n * std::log(fp) / (ln2 * ln2)));
    }

    uint32_t          num_hashes_;
    std::vector<bool> bits_;
};

// ── Block Handle: points to a region in the SSTable file ──

struct BlockHandle {
    uint64_t offset;
    uint64_t size;

    std::string Encode() const {
        std::string out(16, '\0');
        std::memcpy(&out[0], &offset, 8);
        std::memcpy(&out[8], &size, 8);
        return out;
    }

    static BlockHandle Decode(const char* data) {
        BlockHandle h;
        std::memcpy(&h.offset, data, 8);
        std::memcpy(&h.size, data + 8, 8);
        return h;
    }
};

// ── SSTable Footer ──

static constexpr uint64_t kSSTableMagic = 0x4B56535354424C45ULL;  // "KVSSTBLE"

struct Footer {
    BlockHandle meta_index_handle;
    BlockHandle index_handle;

    std::string Encode() const {
        std::string out;
        out += meta_index_handle.Encode();
        out += index_handle.Encode();
        uint64_t magic = kSSTableMagic;
        out.append(reinterpret_cast<const char*>(&magic), 8);
        return out;  // 40 bytes total
    }

    static Footer Decode(const char* data) {
        Footer f;
        f.meta_index_handle = BlockHandle::Decode(data);
        f.index_handle      = BlockHandle::Decode(data + 16);
        // Verify magic
        uint64_t magic = 0;
        std::memcpy(&magic, data + 32, 8);
        assert(magic == kSSTableMagic);
        return f;
    }
};

// ── SSTable Writer ──

class SSTableWriter {
public:
    explicit SSTableWriter(const std::string& filepath,
                           size_t block_size = 4096,
                           size_t expected_keys = 10000)
        : filepath_(filepath),
          block_size_(block_size),
          bloom_(expected_keys),
          entry_count_(0),
          data_offset_(0) {
        file_.open(filepath, std::ios::binary | std::ios::trunc);
    }

    // Add entries in sorted order. MUST be called in key-ascending order.
    void Add(const InternalKey& key, absl::string_view value) {
        std::string encoded_key = EncodeInternalKey(key);

        bloom_.Add(key.user_key);

        // Check if we should start a new data block
        if (current_block_.size() >= block_size_) {
            FlushDataBlock();
        }

        // Append entry to current block (no prefix compression for simplicity)
        AppendToBlock(encoded_key, value);
        ++entry_count_;

        if (first_key_in_block_.empty()) {
            first_key_in_block_ = encoded_key;
        }
        last_key_ = encoded_key;
    }

    // Finalize and close the file. Returns total file size.
    size_t Finish() {
        // Flush remaining data block
        if (!current_block_.empty()) {
            FlushDataBlock();
        }

        // Write meta block (bloom filter)
        std::string bloom_data = bloom_.Serialize();
        BlockHandle meta_handle{data_offset_, bloom_data.size()};
        file_.write(bloom_data.data(), bloom_data.size());
        data_offset_ += bloom_data.size();

        // Write index block
        std::string index_data;
        for (const auto& entry : index_entries_) {
            uint32_t key_len = static_cast<uint32_t>(entry.largest_key.size());
            index_data.append(reinterpret_cast<const char*>(&key_len), 4);
            index_data += entry.largest_key;
            index_data += entry.handle.Encode();
        }
        BlockHandle index_handle{data_offset_, index_data.size()};
        file_.write(index_data.data(), index_data.size());
        data_offset_ += index_data.size();

        // Write footer
        Footer footer{meta_handle, index_handle};
        std::string footer_data = footer.Encode();
        file_.write(footer_data.data(), footer_data.size());
        data_offset_ += footer_data.size();

        file_.flush();
        file_.close();
        return data_offset_;
    }

    size_t EntryCount() const { return entry_count_; }

private:
    struct IndexEntry {
        std::string largest_key;
        BlockHandle handle;
    };

    void AppendToBlock(const std::string& key, absl::string_view value) {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        uint32_t val_len = static_cast<uint32_t>(value.size());
        current_block_.append(reinterpret_cast<const char*>(&key_len), 4);
        current_block_ += key;
        current_block_.append(reinterpret_cast<const char*>(&val_len), 4);
        current_block_.append(value.data(), value.size());
    }

    void FlushDataBlock() {
        BlockHandle handle{data_offset_, current_block_.size()};
        file_.write(current_block_.data(), current_block_.size());
        data_offset_ += current_block_.size();

        index_entries_.push_back(IndexEntry{last_key_, handle});

        current_block_.clear();
        first_key_in_block_.clear();
    }

    static std::string EncodeInternalKey(const InternalKey& k) {
        // Format: [user_key][sequence:8][type:1]
        std::string out = k.user_key;
        out.append(reinterpret_cast<const char*>(&k.sequence), 8);
        out.push_back(static_cast<char>(k.type));
        return out;
    }

    std::string               filepath_;
    std::ofstream             file_;
    size_t                    block_size_;
    BloomFilter               bloom_;
    size_t                    entry_count_;
    uint64_t                  data_offset_;
    std::string               current_block_;
    std::string               first_key_in_block_;
    std::string               last_key_;
    std::vector<IndexEntry>   index_entries_;
};

// ── SSTable Reader ──

class SSTableReader {
public:
    static std::unique_ptr<SSTableReader> Open(const std::string& filepath) {
        auto reader = std::unique_ptr<SSTableReader>(new SSTableReader(filepath));
        if (!reader->Init()) return nullptr;
        return reader;
    }

    // Point lookup using bloom filter + index.
    struct ReadResult {
        bool        found;
        bool        is_deletion;
        std::string value;
    };

    ReadResult Get(absl::string_view user_key) const {
        // Bloom filter short-circuit
        if (!bloom_->MayContain(user_key)) {
            return {false, false, ""};
        }

        // Binary search the index to find the data block
        const IndexEntry* target_block = FindBlock(user_key);
        if (!target_block) return {false, false, ""};

        // Scan the data block for the key
        return ScanBlock(target_block->handle, user_key);
    }

    size_t FileSize() const { return file_size_; }
    const std::string& Filepath() const { return filepath_; }

private:
    struct IndexEntry {
        std::string largest_key;  // user_key portion only
        BlockHandle handle;
    };

    explicit SSTableReader(const std::string& filepath)
        : filepath_(filepath), file_size_(0) {}

    bool Init() {
        file_.open(filepath_, std::ios::binary);
        if (!file_.is_open()) return false;

        // Get file size
        file_.seekg(0, std::ios::end);
        file_size_ = static_cast<size_t>(file_.tellg());
        if (file_size_ < 40) return false;  // Minimum: footer

        // Read footer (last 40 bytes)
        file_.seekg(-40, std::ios::end);
        char footer_buf[40];
        file_.read(footer_buf, 40);
        Footer footer = Footer::Decode(footer_buf);

        // Read bloom filter
        std::string bloom_data(footer.meta_index_handle.size, '\0');
        file_.seekg(footer.meta_index_handle.offset);
        file_.read(&bloom_data[0], bloom_data.size());
        bloom_ = std::make_unique<BloomFilter>(BloomFilter::Deserialize(bloom_data));

        // Read index block
        std::string index_data(footer.index_handle.size, '\0');
        file_.seekg(footer.index_handle.offset);
        file_.read(&index_data[0], index_data.size());
        ParseIndex(index_data);

        return true;
    }

    void ParseIndex(const std::string& data) {
        size_t pos = 0;
        while (pos + 4 < data.size()) {
            uint32_t key_len = 0;
            std::memcpy(&key_len, data.data() + pos, 4);
            pos += 4;
            if (pos + key_len + 16 > data.size()) break;

            std::string encoded_key = data.substr(pos, key_len);
            pos += key_len;

            BlockHandle handle = BlockHandle::Decode(data.data() + pos);
            pos += 16;

            // Extract user_key from encoded key (strip sequence + type)
            std::string user_key = encoded_key.substr(0, encoded_key.size() - 9);
            index_.push_back(IndexEntry{user_key, handle});
        }
    }

    const IndexEntry* FindBlock(absl::string_view user_key) const {
        // First block whose largest_key >= user_key
        for (const auto& entry : index_) {
            if (entry.largest_key >= user_key) {
                return &entry;
            }
        }
        return index_.empty() ? nullptr : &index_.back();
    }

    ReadResult ScanBlock(const BlockHandle& handle, absl::string_view user_key) const {
        std::string block_data(handle.size, '\0');
        // const_cast needed for seekg on mutable ifstream
        auto& f = const_cast<std::ifstream&>(file_);
        f.seekg(handle.offset);
        f.read(&block_data[0], handle.size);

        size_t pos = 0;
        ReadResult best{false, false, ""};
        uint64_t best_seq = 0;

        while (pos + 8 < block_data.size()) {
            uint32_t key_len = 0, val_len = 0;
            std::memcpy(&key_len, block_data.data() + pos, 4); pos += 4;
            if (pos + key_len + 4 > block_data.size()) break;

            std::string encoded_key = block_data.substr(pos, key_len); pos += key_len;

            std::memcpy(&val_len, block_data.data() + pos, 4); pos += 4;
            if (pos + val_len > block_data.size()) break;

            std::string value = block_data.substr(pos, val_len); pos += val_len;

            // Decode internal key
            if (encoded_key.size() < 9) continue;
            std::string uk = encoded_key.substr(0, encoded_key.size() - 9);
            uint64_t seq = 0;
            std::memcpy(&seq, encoded_key.data() + uk.size(), 8);
            auto type = static_cast<ValueType>(encoded_key.back());

            if (uk == user_key && seq > best_seq) {
                best_seq = seq;
                best.found = true;
                best.is_deletion = (type == ValueType::kDeletion);
                best.value = best.is_deletion ? "" : value;
            }
        }
        return best;
    }

    std::string                      filepath_;
    mutable std::ifstream            file_;
    size_t                           file_size_;
    std::unique_ptr<BloomFilter>     bloom_;
    std::vector<IndexEntry>          index_;
};

}  // namespace storage
}  // namespace ai_kv
