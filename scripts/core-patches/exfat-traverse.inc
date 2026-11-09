// Replaces the upstream exFAT walker for firmware installation (Vita3K-UWP).
// The upstream walker assumed directories are contiguous, treated deleted
// entries as the end of a directory, kept reading a parent's entries inside
// the child's stack frame (so recursion grew with the number of directories),
// and trusted every size and cluster. A bad or unusual pd0.img could recurse
// or allocate without bound. This version follows the FAT when a stream is
// not marked contiguous, checks every offset against the image, limits depth,
// refuses directory loops, copies files in chunks and logs what it does.
namespace {

constexpr uint32_t kMaxDepth = 48;
constexpr uint64_t kMaxDirBytes = 64ULL * 1024 * 1024;

struct ExfatReader {
    std::ifstream &img;
    // The whole image, when it was read into memory (the usual case): reads
    // are then copies instead of a seek and a read on the file each.
    const std::vector<char> *memory = nullptr;
    uint64_t img_size = 0;
    uint64_t sector_size = 0;
    uint64_t cluster_size = 0;
    uint64_t heap_offset = 0;
    uint64_t fat_offset = 0;
    uint32_t cluster_count = 0;
    std::vector<uint32_t> fat;
    bool fat_loaded = false;
    std::set<uint32_t> visited_dirs;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;

    bool valid_cluster(uint32_t cluster) const {
        return cluster >= 2 && cluster < static_cast<uint64_t>(cluster_count) + 2;
    }

    uint64_t cluster_offset(uint32_t cluster) const {
        return heap_offset + (static_cast<uint64_t>(cluster) - 2) * cluster_size;
    }

    bool read_at(uint64_t offset, char *out, uint64_t size) {
        if (offset > img_size || size > img_size - offset)
            return false;
        if (memory) {
            std::memcpy(out, memory->data() + offset, static_cast<size_t>(size));
            return true;
        }
        img.clear();
        img.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        img.read(out, static_cast<std::streamsize>(size));
        return static_cast<uint64_t>(img.gcount()) == size;
    }

    bool load_fat() {
        if (fat_loaded)
            return !fat.empty();
        fat_loaded = true;
        const uint64_t entries = static_cast<uint64_t>(cluster_count) + 2;
        if (entries * 4 > img_size) {
            LOG_ERROR("exFAT: FAT larger than the image ({} entries)", entries);
            return false;
        }
        fat.resize(static_cast<size_t>(entries));
        if (!read_at(fat_offset, reinterpret_cast<char *>(fat.data()), entries * 4)) {
            LOG_ERROR("exFAT: could not read the FAT at offset {}", fat_offset);
            fat.clear();
            return false;
        }
        return true;
    }

    // The clusters of a stream, in order. Contiguous streams need no FAT.
    bool chain(uint32_t start, uint64_t size, bool contiguous, std::vector<uint32_t> &out) {
        out.clear();
        if (size == 0)
            return true;
        if (!valid_cluster(start)) {
            LOG_ERROR("exFAT: invalid start cluster {}", start);
            return false;
        }
        const uint64_t needed = (size + cluster_size - 1) / cluster_size;
        if (needed > cluster_count) {
            LOG_ERROR("exFAT: stream of {} bytes is larger than the volume", size);
            return false;
        }
        if (contiguous) {
            if (static_cast<uint64_t>(start) + needed > static_cast<uint64_t>(cluster_count) + 2) {
                LOG_ERROR("exFAT: contiguous stream at cluster {} runs past the volume", start);
                return false;
            }
            for (uint64_t i = 0; i < needed; ++i)
                out.push_back(static_cast<uint32_t>(start + i));
            return true;
        }
        if (!load_fat())
            return false;
        uint32_t cluster = start;
        while (out.size() < needed) {
            if (!valid_cluster(cluster)) {
                LOG_ERROR("exFAT: FAT chain from cluster {} breaks at {}", start, cluster);
                return false;
            }
            out.push_back(cluster);
            cluster = fat[cluster];
        }
        return true;
    }

    // A directory whose size is unknown (the root) ends at the end of its
    // FAT chain.
    bool root_chain(uint32_t start, std::vector<uint32_t> &out) {
        out.clear();
        if (!load_fat()) {
            if (!valid_cluster(start))
                return false;
            out.push_back(start);
            return true;
        }
        uint32_t cluster = start;
        while (valid_cluster(cluster) && out.size() * cluster_size < kMaxDirBytes) {
            if (std::find(out.begin(), out.end(), cluster) != out.end())
                break;
            out.push_back(cluster);
            cluster = fat[cluster];
        }
        return !out.empty();
    }

    bool read_stream(const std::vector<uint32_t> &clusters, uint64_t size, std::vector<char> &out) {
        out.resize(static_cast<size_t>(size));
        uint64_t done = 0;
        for (const uint32_t cluster : clusters) {
            if (done >= size)
                break;
            const uint64_t take = std::min<uint64_t>(cluster_size, size - done);
            if (!read_at(cluster_offset(cluster), out.data() + done, take))
                return false;
            done += take;
        }
        return done == size;
    }

    bool copy_file(const std::vector<uint32_t> &clusters, uint64_t size, const fs::path &target) {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_ERROR("exFAT: could not create {}", fs_utils::path_to_utf8(target));
            return false;
        }
        std::vector<char> chunk(static_cast<size_t>(std::min<uint64_t>(cluster_size, 1ULL << 20)));
        uint64_t done = 0;
        for (const uint32_t cluster : clusters) {
            uint64_t in_cluster = 0;
            while (in_cluster < cluster_size && done < size) {
                const uint64_t take = std::min<uint64_t>({ static_cast<uint64_t>(chunk.size()), cluster_size - in_cluster, size - done });
                if (!read_at(cluster_offset(cluster) + in_cluster, chunk.data(), take)) {
                    LOG_ERROR("exFAT: read failed inside {}", fs_utils::path_to_utf8(target));
                    return false;
                }
                file.write(chunk.data(), static_cast<std::streamsize>(take));
                if (!file) {
                    LOG_ERROR("exFAT: write failed for {}", fs_utils::path_to_utf8(target));
                    return false;
                }
                in_cluster += take;
                done += take;
            }
        }
        bytes += done;
        return done == size;
    }

    static bool safe_name(const std::u16string &name) {
        if (name.empty() || name == u"." || name == u"..")
            return false;
        for (const char16_t c : name)
            if (c < 0x20 || c == u'/' || c == u'\\' || c == u':')
                return false;
        return true;
    }

    void walk(const std::vector<char> &dir, const fs::path &out_dir, const std::string &label, uint32_t depth) {
        constexpr size_t E = 32;
        for (size_t i = 0; i + E <= dir.size(); i += E) {
            const uint8_t type = static_cast<uint8_t>(dir[i]);
            if (type == 0x00)
                break; // end of directory
            if (type != EXFAT_ENTRY_FILE)
                continue; // bitmap, up-case table, label, deleted or stray entries
            ExFATFileEntry entry;
            std::memcpy(&entry, &dir[i], sizeof(entry));
            const size_t secondaries = entry.continuations;
            if (secondaries < 2 || i + E * (secondaries + 1) > dir.size()) {
                LOG_ERROR("exFAT: malformed entry set in {}", label);
                ++errors;
                break;
            }
            ExFATFileEntryInfo info;
            std::memcpy(&info, &dir[i + E], sizeof(info));
            if (info.type != EXFAT_ENTRY_FILE_INFO) {
                LOG_ERROR("exFAT: entry set in {} has no stream extension", label);
                ++errors;
                i += E * secondaries;
                continue;
            }
            std::u16string name;
            size_t remaining = info.name_length;
            for (size_t k = 2; k <= secondaries && remaining > 0; ++k) {
                ExFATEntryName part;
                std::memcpy(&part, &dir[i + E * k], sizeof(part));
                if (part.type != EXFAT_ENTRY_FILE_NAME)
                    break;
                const size_t take = std::min<size_t>(remaining, EXFAT_ENAME_MAX);
                name.append(reinterpret_cast<const char16_t *>(part.name), take);
                remaining -= take;
            }
            i += E * secondaries;
            if (!safe_name(name)) {
                LOG_ERROR("exFAT: skipped an entry with an unusable name in {}", label);
                ++errors;
                continue;
            }
            const fs::path target = out_dir / fs::path(std::wstring(name.begin(), name.end()));
            const std::string child_label = label + "/" + fs_utils::path_to_utf8(fs::path(std::wstring(name.begin(), name.end())));
            const bool contiguous = (info.flags & 0x02) != 0;
            std::vector<uint32_t> clusters;
            if (entry.attrib & EXFAT_ATTRIB_DIR) {
                if (depth >= kMaxDepth) {
                    LOG_ERROR("exFAT: {} is nested too deeply; skipped", child_label);
                    ++errors;
                    continue;
                }
                if (!visited_dirs.insert(info.start_cluster).second) {
                    LOG_ERROR("exFAT: directory loop at {}; skipped", child_label);
                    ++errors;
                    continue;
                }
                std::error_code ec;
                fs::create_directories(target, ec);
                if (ec) {
                    LOG_ERROR("exFAT: could not create {}: {}", child_label, ec.message());
                    ++errors;
                    continue;
                }
                ++dirs;
                const uint64_t size = std::min<uint64_t>(info.size, kMaxDirBytes);
                std::vector<char> child;
                if (!chain(info.start_cluster, size, contiguous, clusters) || !read_stream(clusters, size, child)) {
                    LOG_ERROR("exFAT: could not read directory {}", child_label);
                    ++errors;
                    continue;
                }
                if (depth < 3)
                    LOG_INFO("exFAT: {}", child_label);
                walk(child, target, child_label, depth + 1);
            } else {
                const uint64_t size = info.valid_size <= info.size ? info.valid_size : info.size;
                if (size > img_size) {
                    LOG_ERROR("exFAT: {} claims {} bytes, more than the image; skipped", child_label, size);
                    ++errors;
                    continue;
                }
                if (!chain(info.start_cluster, size, contiguous, clusters) || !copy_file(clusters, size, target)) {
                    LOG_ERROR("exFAT: could not extract {}", child_label);
                    ++errors;
                    continue;
                }
                ++files;
            }
        }
    }
};

} // namespace

void extract_exfat(const fs::path &partition_path, const std::string &partition, const fs::path &vita_fs_path) {
    const fs::path image_path = partition_path / partition;
    std::ifstream img(image_path, std::ios::binary);
    if (!img.is_open()) {
        LOG_ERROR("exFAT: failed to open {}", fs_utils::path_to_utf8(image_path));
        return;
    }
    std::error_code size_ec;
    const uint64_t img_size = fs::file_size(image_path, size_ec);
    if (size_ec || img_size < sizeof(ExFATSuperBlock)) {
        LOG_ERROR("exFAT: {} is too small to be a volume", partition);
        return;
    }

    ExFATSuperBlock super_block;
    img.read(reinterpret_cast<char *>(&super_block), sizeof(ExFATSuperBlock));
    if (std::memcmp(super_block.oem_name, "EXFAT   ", 8) != 0) {
        LOG_ERROR("exFAT: {} has no exFAT signature; the firmware package may not have decrypted correctly", partition);
        return;
    }
    if (super_block.sector_bits < 9 || super_block.sector_bits > 12 || super_block.spc_bits > 25 - super_block.sector_bits) {
        LOG_ERROR("exFAT: {} has an invalid geometry (sector bits {}, cluster bits {})", partition, super_block.sector_bits, super_block.spc_bits);
        return;
    }

    ExfatReader reader{ img };
    reader.img_size = img_size;
    std::vector<char> whole;
    if (img_size <= (1ULL << 30)) {
        whole.resize(static_cast<size_t>(img_size));
        img.clear();
        img.seekg(0, std::ios::beg);
        img.read(whole.data(), static_cast<std::streamsize>(img_size));
        if (static_cast<uint64_t>(img.gcount()) == img_size)
            reader.memory = &whole;
        else
            whole.clear();
    }
    reader.sector_size = 1ULL << super_block.sector_bits;
    reader.cluster_size = reader.sector_size << super_block.spc_bits;
    reader.heap_offset = static_cast<uint64_t>(super_block.cluster_sector_start) * reader.sector_size;
    reader.fat_offset = static_cast<uint64_t>(super_block.fat_sector_start) * reader.sector_size;
    reader.cluster_count = super_block.cluster_count;
    if (reader.heap_offset >= img_size) {
        LOG_ERROR("exFAT: cluster heap of {} starts past the end of the image", partition);
        return;
    }
    const uint64_t heap_clusters = (img_size - reader.heap_offset) / reader.cluster_size;
    if (reader.cluster_count > heap_clusters)
        reader.cluster_count = static_cast<uint32_t>(heap_clusters);

    const fs::path output_path{ vita_fs_path / partition.substr(0, 3) };
    std::error_code ec;
    fs::create_directories(output_path, ec);
    LOG_INFO("exFAT: extracting {} ({} bytes, {} byte clusters) to {}", partition, img_size, reader.cluster_size, fs_utils::path_to_utf8(output_path));

    std::vector<uint32_t> clusters;
    std::vector<char> root;
    reader.visited_dirs.insert(super_block.rootdir_cluster);
    if (!reader.root_chain(super_block.rootdir_cluster, clusters) || !reader.read_stream(clusters, clusters.size() * reader.cluster_size, root)) {
        LOG_ERROR("exFAT: could not read the root directory of {}", partition);
        return;
    }
    reader.walk(root, output_path, partition.substr(0, 3), 0);
    LOG_INFO("exFAT: {} done: {} files, {} directories, {} bytes, {} problems", partition, reader.files, reader.dirs, reader.bytes, reader.errors);
}

} // namespace exfat
