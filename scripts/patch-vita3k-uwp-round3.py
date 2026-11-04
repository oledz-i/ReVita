#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Firmware installation hardening for the Xbox port.

The first console test stopped right after "install progress: 70%", which is
where install_pup unpacks the decrypted partition images. This makes that
step bounded and logged, replaces the exFAT walker (pd0, the preinstalled
package) with one that cannot recurse or allocate without bound, checks every
file it opens, installs the system firmware before the optional parts, and on
Windows turns a hardware fault during installation into a logged failure
instead of a silent crash.
"""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).resolve()
repo = pathlib.Path(__file__).resolve().parent.parent


def read(relative):
    return (source / relative).read_text(encoding='utf-8')


def write(relative, text):
    (source / relative).write_text(text, encoding='utf-8')


def replace(relative, old, new, count=1):
    text = read(relative)
    assert text.count(old) == count, f'{relative}: expected {count} matches for {old!r}'
    write(relative, text.replace(old, new))


# ── exFAT (pd0) ─────────────────────────────────────────────────────────────
exfat = 'vita3k/packages/src/exfat.cpp'
text = read(exfat)
marker = 'static uint64_t get_cluster_offset('
assert text.count(marker) == 1, 'exfat.cpp layout changed'
head = text[:text.index(marker)]
inc = (repo / 'scripts/core-patches/exfat-traverse.inc').read_text(encoding='utf-8')
head = head.replace('#include <packages/exfat.h>',
                    '#include <packages/exfat.h>\n#include <util/fs.h>\n\n#include <algorithm>\n'
                    '#include <cstring>\n#include <fstream>\n#include <set>\n#include <system_error>\n#include <vector>', 1)
# get_exfat_file_name is unused by the new walker; keep it from warning.
head = head.replace('static fs::path get_exfat_file_name(', '[[maybe_unused]] static fs::path get_exfat_file_name(', 1)
write(exfat, head + inc)

# ── FAT16 (os0, vs0, sa0) ───────────────────────────────────────────────────
sce = 'vita3k/packages/src/sce_utils.cpp'
replace(sce, '#include <util/string_utils.h>\n', '#include <util/fs.h>\n#include <util/string_utils.h>\n\n#include <system_error>\n')
replace(sce, '''    FILE *f = FOPEN(filename.native().c_str(), "wb");

    static constexpr std::uint32_t CHUNK_SIZE = 0x10000;''', '''    FILE *f = FOPEN(filename.native().c_str(), "wb");
    if (!f) {
        LOG_ERROR("FAT: could not create {}", fs_utils::path_to_utf8(filename));
        return;
    }

    static constexpr std::uint32_t CHUNK_SIZE = 0x10000;''')
replace(sce, '''        if (img.read_from_cluster(&temp_buf[0], offset, entry.entry.starting_cluster, size_to_take) != size_to_take) {
            break;
        }''', '''        if (img.read_from_cluster(&temp_buf[0], offset, entry.entry.starting_cluster, size_to_take) != size_to_take) {
            LOG_ERROR("FAT: short read in {}", fs_utils::path_to_utf8(filename));
            break;
        }''')
replace(sce, '''static void traverse_directory(Fat16::Image &img, Fat16::Entry mee, const fs::path &dir_path) {
    fs::create_directories(dir_path);
''', '''static void traverse_directory(Fat16::Image &img, Fat16::Entry mee, const fs::path &dir_path, int depth = 0) {
    std::error_code dir_ec;
    fs::create_directories(dir_path, dir_ec);
    if (dir_ec) {
        LOG_ERROR("FAT: could not create {}: {}", fs_utils::path_to_utf8(dir_path), dir_ec.message());
        return;
    }
    if (depth > 48) {
        LOG_ERROR("FAT: {} is nested too deeply; skipped", fs_utils::path_to_utf8(dir_path));
        return;
    }
''')
replace(sce, '''                traverse_directory(img, baby, dir_path / std::wstring(dir_name.begin(), dir_name.end()));''',
        '''                traverse_directory(img, baby, dir_path / std::wstring(dir_name.begin(), dir_name.end()), depth + 1);''')
replace(sce, '''    FILE *f = FOPEN((partition_path / partition).native().c_str(), "rb");
    Fat16::Image img(''', '''    FILE *f = FOPEN((partition_path / partition).native().c_str(), "rb");
    if (!f) {
        LOG_ERROR("FAT: failed to open {}", partition);
        return;
    }
    LOG_INFO("FAT: extracting {} to {}", partition, fs_utils::path_to_utf8(vita_fs_path / partition.substr(0, 3)));
    Fat16::Image img(''')
replace(sce, '''    traverse_directory(img, first, vita_fs_path / partition.substr(0, 3));

    fclose(f);''', '''    traverse_directory(img, first, vita_fs_path / partition.substr(0, 3));

    fclose(f);
    LOG_INFO("FAT: {} done", partition);''')

# ── install_pup: check each step and say which partition it is on ──────────
pup = 'vita3k/packages/src/pup.cpp'
replace(pup, '#include <fstream>\n', '#include <fstream>\n#include <system_error>\n')
replace(pup, '''    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr);
    int dec_len = 0;''', '''    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr);
    if (!cipher_ctx || !cipher) {
        LOG_ERROR("PUP: AES-128-CTR is not available from OpenSSL; cannot decrypt {}", fs_utils::path_to_utf8(filename));
        EVP_CIPHER_CTX_free(cipher_ctx);
        EVP_CIPHER_free(cipher);
        return;
    }
    int dec_len = 0;''')
replace(pup, '''    update_progress(70);
    if (fs::file_size(pup_dec / "os0.img") > 0)
        extract_fat(pup_dec, "os0.img", vita_fs_path);
    if (fs::file_size(pup_dec / "pd0.img") > 0)
        exfat::extract_exfat(pup_dec, "pd0.img", vita_fs_path);
    if (fs::file_size(pup_dec / "sa0.img") > 0)
        extract_fat(pup_dec, "sa0.img", vita_fs_path);
    if (fs::file_size(pup_dec / "vs0.img") > 0)
        extract_fat(pup_dec, "vs0.img", vita_fs_path);
    update_progress(100);''', '''    update_progress(70);
    const auto image_size = [&](const char *name) -> uint64_t {
        std::error_code ec;
        const uint64_t size = fs::file_size(pup_dec / name, ec);
        return ec ? 0 : size;
    };
    LOG_INFO("PUP: decrypted images: os0 {} bytes, pd0 {} bytes, sa0 {} bytes, vs0 {} bytes",
        image_size("os0.img"), image_size("pd0.img"), image_size("sa0.img"), image_size("vs0.img"));
    if (image_size("os0.img") > 0)
        extract_fat(pup_dec, "os0.img", vita_fs_path);
    update_progress(75);
    if (image_size("pd0.img") > 0)
        exfat::extract_exfat(pup_dec, "pd0.img", vita_fs_path);
    update_progress(85);
    if (image_size("sa0.img") > 0)
        extract_fat(pup_dec, "sa0.img", vita_fs_path);
    update_progress(90);
    if (image_size("vs0.img") > 0)
        extract_fat(pup_dec, "vs0.img", vita_fs_path);
    update_progress(100);''')
replace(pup, '''    fs::remove_all(pup_dec_root);

    return fw_version;''', '''    std::error_code cleanup_ec;
    fs::remove_all(pup_dec_root, cleanup_ec);
    if (cleanup_ec)
        LOG_WARN("PUP: could not remove the working folder {} yet ({}); the next install uses its own", fs_utils::path_to_utf8(pup_dec_root), cleanup_ec.message());

    return fw_version;''')
# Several packages in one go (preinstall, fonts, then the system firmware)
# failed on the later ones but worked alone or after a restart. Every
# install reused vita3k/PUP_DEC: the previous one deleted it on the way
# out, and on Windows a folder whose files were just deleted can stay
# "delete pending" for a moment (a scanner or the indexer still holds one),
# so the next install's remove_all/create_directory threw. Each install now
# works in its own folder, clears old ones without throwing, and reports a
# folder it cannot create.
replace(pup, '''    fs::path pup_dec_root = vita_fs_path / "PUP_DEC";
    if (fs::exists(pup_dec_root)) {
        LOG_WARN("Path already exists, deleting it and reinstalling");
        fs::remove_all(pup_dec_root);
    }''', '''    {
        std::error_code sweep_ec;
        for (fs::directory_iterator it(vita_fs_path, sweep_ec), end; !sweep_ec && it != end; it.increment(sweep_ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind("PUP_DEC", 0) == 0) {
                std::error_code remove_ec;
                fs::remove_all(it->path(), remove_ec);
            }
        }
    }
    static std::atomic<uint32_t> install_serial{ 0 };
    fs::path pup_dec_root = vita_fs_path / fmt::format("PUP_DEC-{}-{}", static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFF), ++install_serial);
    {
        std::error_code mk_ec;
        fs::create_directories(pup_dec_root, mk_ec);
        if (mk_ec)
            throw std::runtime_error(fmt::format("cannot create the working folder {}: {}", fs_utils::path_to_utf8(pup_dec_root), mk_ec.message()));
    }''')
replace(pup, '#include <system_error>\n', '#include <system_error>\n#include <atomic>\n#include <chrono>\n#include <stdexcept>\n')

# ── Installer: system firmware first, fault guard, no half-installed parts ──
inst = 'vita3k/libretro/libretro_installer.cpp'
replace(inst, '#include <vector>\n', '''#include <vector>

#if defined(_WIN32)
#include <excpt.h>
#include <malloc.h>
#include <stdexcept>
// A fault inside the firmware unpacker (bad data, an out-of-memory write,
// a stack overflow) would otherwise end the app with nothing in the log.
// The __try lives in a function with no C++ objects, as MSVC requires.
static unsigned long g_install_fault = 0;
static int install_fault_filter(unsigned long code) {
    g_install_fault = code;
    return EXCEPTION_EXECUTE_HANDLER;
}
static bool run_fault_guarded(void (*fn)(void *), void *ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (install_fault_filter(GetExceptionCode())) {
        if (g_install_fault == 0xC00000FDu)
            _resetstkoflw();
        return false;
    }
}
#endif
''', 1)
replace(inst, '''    std::string fw_version;
    try {
        fw_version = install_pup(pref_path, pup_path, [label](uint32_t progress) {
            lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", label, progress);
        });
    } catch (const std::exception &e) {''', '''    std::string fw_version;
    try {
#if defined(_WIN32)
        struct Job {
            const fs::path *pref;
            const fs::path *pup;
            const char *label;
            std::string *version;
            std::string error;
        } job{ &pref_path, &pup_path, label, &fw_version, {} };
        const bool completed = run_fault_guarded([](void *raw) {
            auto *j = static_cast<Job *>(raw);
            try {
                const char *job_label = j->label;
                *j->version = install_pup(*j->pref, *j->pup, [job_label](uint32_t progress) {
                    lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", job_label, progress);
                });
            } catch (const std::exception &e) {
                j->error = e.what();
            } catch (...) {
                j->error = "unknown exception";
            }
        }, &job);
        if (!completed) {
            lr_log(RETRO_LOG_ERROR, "%s installation crashed (Windows exception 0x%08lX); nothing further was installed from it\\n", label, g_install_fault);
            lr_msg(fmt::format("{} installation crashed. See vita3k-runtime.log.", label).c_str(), 600);
            return false;
        }
        if (!job.error.empty())
            throw std::runtime_error(job.error);
#else
        fw_version = install_pup(pref_path, pup_path, [label](uint32_t progress) {
            lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", label, progress);
        });
#endif
    } catch (const std::exception &e) {''')
replace(inst, '''    const std::pair<PupKind, const char *> parts[] = {
        { PupKind::Preinst, "Preinstalled package" },
        { PupKind::Firmware, "System firmware" },
        { PupKind::Font, "Font package" },
    };''', '''    // The system firmware first: games need it, and a problem with an
    // optional part must not stop it from being installed.
    const std::pair<PupKind, const char *> parts[] = {
        { PupKind::Firmware, "System firmware" },
        { PupKind::Preinst, "Preinstalled package" },
        { PupKind::Font, "Font package" },
    };''')
replace(inst, '''        (install_pup_file(pref_path, pup, label) ? installed : failed)++;''', '''        if (install_pup_file(pref_path, pup, label)) {
            installed++;
        } else {
            failed++;
            // A half-written partition would read as installed next time.
            const char *partition = kind == PupKind::Preinst ? "pd0" : kind == PupKind::Font ? "sa0" : "vs0";
            std::error_code ec;
            fs::remove_all(pref_path / partition, ec);
            lr_log(RETRO_LOG_ERROR, "Removed the partial %s after the failed install so it is retried\\n", partition);
        }''')
print('Hardened firmware installation: exFAT walker, FAT checks, per-partition logging, fault guard.')

# ── Firmware for the frontend: what a PUP holds, and installing one now ─────
# The Xbox frontend installs firmware the moment the user picks it, and tells
# the parts apart by what is inside the package (not by its file name, which
# Sony's downloads all share). These are plain C exports it finds with
# GetProcAddress; nothing here runs unless the frontend calls it.
replace(pup, '''std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback) {''',
'''bool read_pup_contents(const fs::path &pup, uint32_t &firmware_version, uint32_t &partitions) {
    firmware_version = 0;
    partitions = 0;
    std::ifstream in(pup, std::ios::binary);
    if (!in)
        return false;
    char header[0x80];
    in.read(header, sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header)) || strncmp(header, "SCEUF", 5) != 0)
        return false;
    uint32_t count = 0;
    memcpy(&count, &header[0x18], 4);
    memcpy(&firmware_version, &header[0x10], 4);
    if (count > 4096)
        return false;
    std::vector<unsigned char> hdr(HEADER_LENGTH);
    for (uint32_t x = 0; x < count; x++) {
        char rec[0x20];
        in.clear();
        in.seekg(0x80 + static_cast<std::streamoff>(x) * 0x20, std::ios::beg);
        in.read(rec, sizeof(rec));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(rec)))
            return false;
        uint64_t filetype = 0, offset = 0;
        memcpy(&filetype, &rec[0], 8);
        memcpy(&offset, &rec[8], 8);
        if (PUP_TYPES.contains(filetype))
            continue;
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        in.read(reinterpret_cast<char *>(hdr.data()), HEADER_LENGTH);
        if (in.gcount() != HEADER_LENGTH)
            continue;
        uint32_t magic = 0, version = 0, flags = 0;
        uint64_t metaoffs = 0;
        memcpy(&magic, &hdr[0], 4);
        memcpy(&version, &hdr[4], 4);
        memcpy(&flags, &hdr[8], 4);
        memcpy(&metaoffs, &hdr[16], 8);
        // The same test make_filename uses to name the package
        if (magic != SCE_MAGIC || version != 3 || flags != 0x30040 || metaoffs + 4 >= HEADER_LENGTH)
            continue;
        switch (hdr[metaoffs + 4]) {
        case 0x01: partitions |= 1; break; // os0
        case 0x0A: partitions |= 2; break; // vs0
        case 0x18: partitions |= 4; break; // pd0
        case 0x17: partitions |= 8; break; // sa0
        default: break;
        }
    }
    return true;
}

std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback) {''')
replace('vita3k/packages/include/packages/functions.h',
        'std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback = nullptr);',
        '''std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback = nullptr);
// Which partitions a PUP carries (bit 0 os0, 1 vs0, 2 pd0, 3 sa0) and its firmware version, read from its headers only.
bool read_pup_contents(const fs::path &pup, uint32_t &firmware_version, uint32_t &partitions);''')

replace(inst, '#endif\n\nenum class PupKind', '''#endif

// Progress of the install in flight, for a frontend that shows it; and the
// version the last successful install reported.
static std::function<void(uint32_t)> g_install_progress;
static std::string g_last_fw_version;

enum class PupKind''') if read(inst).count('#endif\n\nenum class PupKind') == 1 else None
text = read(inst)
if 'static std::function<void(uint32_t)> g_install_progress;' not in text:
    # Place it after the fault-guard block added above.
    marker = '#endif\n'
    first_guard_end = text.index(marker, text.index('static bool run_fault_guarded')) + len(marker)
    text = text[:first_guard_end] + '''
// Progress of the install in flight, for a frontend that shows it; and the
// version the last successful install reported.
static std::function<void(uint32_t)> g_install_progress;
static std::string g_last_fw_version;
''' + text[first_guard_end:]
    write(inst, text)
replace(inst, '''                    lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", job_label, progress);
                });''', '''                    lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", job_label, progress);
                    if (g_install_progress)
                        g_install_progress(progress);
                });''')
replace(inst, '''        fw_version = install_pup(pref_path, pup_path, [label](uint32_t progress) {
            lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", label, progress);
        });''', '''        fw_version = install_pup(pref_path, pup_path, [label](uint32_t progress) {
            lr_log(RETRO_LOG_INFO, "%s install progress: %u%%\\n", label, progress);
            if (g_install_progress)
                g_install_progress(progress);
        });''')
replace(inst, '''        lr_log(RETRO_LOG_INFO, "%s version %s installed successfully!\\n", label, fw_version.c_str());
    }

    return true;
}''', '''        lr_log(RETRO_LOG_INFO, "%s version %s installed successfully!\\n", label, fw_version.c_str());
    }
    g_last_fw_version = fw_version;

    return true;
}

#if defined(VITA3K_UWP)
extern "C" __declspec(dllexport) int vita3k_uwp_pup_info(const char *pup_utf8, uint32_t *firmware_version, uint32_t *partitions) {
    uint32_t version = 0, mask = 0;
    bool ok = false;
    try {
        ok = pup_utf8 && read_pup_contents(fs_utils::utf8_to_path(pup_utf8), version, mask);
    } catch (...) {
        ok = false;
    }
    if (firmware_version)
        *firmware_version = version;
    if (partitions)
        *partitions = mask;
    return ok ? 1 : 0;
}

extern "C" __declspec(dllexport) int vita3k_uwp_install_pup(const char *pref_utf8, const char *pup_utf8,
    void (*progress)(uint32_t, void *), void *user, char *version, uint32_t version_size) {
    if (!pref_utf8 || !pup_utf8)
        return 0;
    const fs::path pref_path = fs_utils::utf8_to_path(pref_utf8);
    const fs::path pup_path = fs_utils::utf8_to_path(pup_utf8);
    std::error_code ec;
    fs::create_directories(pref_path, ec);
    uint32_t fw = 0, parts = 0;
    try {
        read_pup_contents(pup_path, fw, parts);
    } catch (...) {
    }
    g_last_fw_version.clear();
    if (progress)
        g_install_progress = [progress, user](uint32_t value) { progress(value, user); };
    const bool ok = install_pup_file(pref_path, pup_path, "Firmware package");
    g_install_progress = nullptr;
    if (!ok) {
        // A half-written partition would read as installed.
        const char *names[] = { "os0", "vs0", "pd0", "sa0" };
        for (int bit = 0; bit < 4; ++bit)
            if (parts & (1u << bit))
                fs::remove_all(pref_path / names[bit], ec);
    }
    if (version && version_size)
        snprintf(version, version_size, "%s", g_last_fw_version.c_str());
    check_firmware_status(pref_path);
    return ok ? 1 : 0;
}
#endif''')

# ── NoNpDrm decryption: guarded, logged, with progress ─────────────────────
# The second console run stopped inside psvpfstools' execute() with nothing
# in the log: it reports only on std::cout, and nothing caught a failure.
replace(inst, 'static bool install_pup_file(', '''// psvpfstools reports on std::cout, which an app without a console never
// shows: each line goes to the libretro log instead.
class LineLogBuf : public std::streambuf {
public:
    ~LineLogBuf() override { flush_line(); }

protected:
    int overflow(int c) override {
        if (c == traits_type::eof())
            return 0;
        if (c == '\\n')
            flush_line();
        else if (line_.size() < 2000)
            line_.push_back(static_cast<char>(c));
        return c;
    }
    int sync() override {
        flush_line();
        return 0;
    }

private:
    void flush_line() {
        if (!line_.empty())
            lr_log(RETRO_LOG_INFO, "PFS: %s\\n", line_.c_str());
        line_.clear();
    }
    std::string line_;
};

// The algorithms psvpfstools fetches from OpenSSL; a missing one would make it
// use a null context.
static bool pfs_crypto_available() {
    bool ok = true;
    for (const char *name : { "AES-128-CBC", "AES-128-CTR", "AES-128-ECB" }) {
        EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, name, nullptr);
        if (!cipher) {
            lr_log(RETRO_LOG_ERROR, "OpenSSL has no %s\\n", name);
            ok = false;
        }
        EVP_CIPHER_free(cipher);
    }
    for (const char *name : { "SHA1", "SHA256" }) {
        EVP_MD *md = EVP_MD_fetch(nullptr, name, nullptr);
        if (!md) {
            lr_log(RETRO_LOG_ERROR, "OpenSSL has no %s\\n", name);
            ok = false;
        }
        EVP_MD_free(md);
    }
    for (const char *name : { "CMAC", "HMAC" }) {
        EVP_MAC *mac = EVP_MAC_fetch(nullptr, name, nullptr);
        if (!mac) {
            lr_log(RETRO_LOG_ERROR, "OpenSSL has no %s\\n", name);
            ok = false;
        }
        EVP_MAC_free(mac);
    }
    return ok;
}

static int run_pfs_decryption(std::string &zrif, std::string &src, std::string &dst) {
    if (!pfs_crypto_available()) {
        lr_log(RETRO_LOG_ERROR, "PFS decryption needs the OpenSSL algorithms above; not started\\n");
        return -1;
    }
    LineLogBuf log_buf;
    std::streambuf *previous = std::cout.rdbuf(&log_buf);
    int last_decile = -1;
    const auto started_at = std::chrono::steady_clock::now();
    PfsProgressCallback progress = [&last_decile, started_at](std::uint64_t processed, std::uint64_t total, const std::string &) {
        if (!total)
            return;
        // Every 2%: the frontend's loading screen shows it. The seconds and
        // the speed show in the log whether the time goes to the drive or to
        // the decryption itself.
        const int step = static_cast<int>(std::min<std::uint64_t>(processed * 50 / total, 50));
        if (step != last_decile) {
            last_decile = step;
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
            lr_log(RETRO_LOG_INFO, "PFS decryption: %d%% (%.1f s, %.1f MB/s)\\n", step * 2, secs, secs > 0.01 ? processed / 1048576.0 / secs : 0.0);
        }
    };
    int result = -1;
    std::string error;
    F00DEncryptorTypes type = F00DEncryptorTypes::native;
    std::string f00d_arg;
#if defined(_WIN32)
    struct Job {
        std::string *zrif, *src, *dst, *f00d_arg, *error;
        F00DEncryptorTypes type;
        PfsProgressCallback *progress;
        int *result;
    } job{ &zrif, &src, &dst, &f00d_arg, &error, type, &progress, &result };
    const bool completed = run_fault_guarded([](void *raw) {
        auto *j = static_cast<Job *>(raw);
        try {
            *j->result = execute(*j->zrif, *j->src, *j->dst, j->type, *j->f00d_arg, *j->progress);
        } catch (const std::exception &e) {
            *j->error = e.what();
            *j->result = -1;
        } catch (...) {
            *j->error = "unknown exception";
            *j->result = -1;
        }
    }, &job);
    if (!completed) {
        error = fmt::format("crashed (Windows exception 0x{:08X})", g_install_fault);
        result = -1;
    }
#else
    try {
        result = execute(zrif, src, dst, type, f00d_arg, progress);
    } catch (const std::exception &e) {
        error = e.what();
        result = -1;
    }
#endif
    std::cout.flush();
    std::cout.rdbuf(previous);
    lr_log(RETRO_LOG_INFO, "PFS decryption took %.1f s\\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count());
    if (!error.empty())
        lr_log(RETRO_LOG_ERROR, "PFS decryption %s\\n", error.c_str());
    return result;
}

static bool install_pup_file(''', 1)
replace(inst, '    int dec_result = execute(app_zRIF, src_str, dst_str, f00d_enc_type, f00d_arg);',
        '    (void)f00d_enc_type;\n    int dec_result = run_pfs_decryption(app_zRIF, src_str, dst_str);')
replace(inst, '#if defined(_WIN32)\n#include <excpt.h>', '#include <chrono>\n#include <iostream>\n#include <streambuf>\n#include <openssl/evp.h>\n\n#if defined(_WIN32)\n#include <excpt.h>', 1)
print('Added firmware exports for the frontend and guarded, logged NoNpDrm decryption.')

# ── Paths without canonicalisation ──────────────────────────────────────────
# The third console run got through firmware and the game copy, then
# psvpfstools stopped with "weakly_canonical: Access is denied" on the
# installed game's sce_sys. std::filesystem::relative() canonicalises both
# paths, which walks the parent folders of the app's storage; a UWP app may
# not open those. Every path compared here is built from the same root, so
# the lexical form gives the same answer without touching the disk.
pfs_utils = 'external/psvpfstools/psvpfsparser/Utils.cpp'
replace(pfs_utils, '''   psvpfs::path dest_path = dest_root / psvpfs::relative(source_path, source_root);''',
'''   psvpfs::path rel = psvpfs::path(source_path.generic_string()).lexically_normal().lexically_relative(
      psvpfs::path(source_root.generic_string()).lexically_normal());
   if (rel.empty())
      rel = psvpfs::relative(source_path, source_root);
   psvpfs::path dest_path = dest_root / rel;''')
replace('vita3k/util/src/fs_utils.cpp', '            const auto relative_path = fs::relative(src.path(), src_path);',
        '            const auto relative_path = src.path().lexically_normal().lexically_relative(src_path.lexically_normal());')
replace('vita3k/packages/src/sce_utils.cpp', '            const auto output_file_path = output_path / fs::relative(entry.path(), input_path);',
        '            const auto output_file_path = output_path / entry.path().lexically_normal().lexically_relative(input_path.lexically_normal());')
replace('vita3k/packages/src/sce_utils.cpp', '            const auto out_rel = fs::relative(output_file_path, cache_path);',
        '            const auto out_rel = output_file_path.lexically_normal().lexically_relative(cache_path.lexically_normal());')
replace('vita3k/libretro/libretro_lazy_pkg.cpp', '            e.name = fs::relative(it->path(), dir, ec).generic_string();',
        '            e.name = it->path().lexically_normal().lexically_relative(dir.lexically_normal()).generic_string();')
print('Replaced canonicalising path helpers with lexical ones.')

# ── Encrypted game folders: decrypt straight from where they are ───────────
# A NoNpDrm folder was first copied whole into ux0/app, then decrypted into
# a second folder that replaced the copy: twice the game's size on a console
# whose internal partition can be 5 GB, and two full passes over the data.
# The decrypted output replaces the copy entirely, so for a game (category gd)
# decrypt from the source folder itself; updates and DLC keep the old path.
replace(inst, '''static bool decrypt_nonpdrm_app(const fs::path &install_path, const fs::path &pref_path, const GameInstallResult &result,
    std::vector<uint8_t> &work_bin_data, std::string &app_zRIF, bool keep_license = true) {
    const auto work_bin = install_path / "sce_sys" / "package" / "work.bin";''',
'''static bool decrypt_nonpdrm_app(const fs::path &install_path, const fs::path &pref_path, const GameInstallResult &result,
    std::vector<uint8_t> &work_bin_data, std::string &app_zRIF, bool keep_license = true, const fs::path &source = {}) {
    // The encrypted files come from source when given (a game folder read
    // where it lies), else from the copy at install_path.
    const fs::path &encrypted = source.empty() ? install_path : source;
    const auto work_bin = encrypted / "sce_sys" / "package" / "work.bin";''')
replace(inst, '''    std::string src_str = install_path.generic_string();
    std::string dst_str = install_decrypt_folder(pref_path, install_path).generic_string();''',
'''    std::string src_str = encrypted.generic_string();
    std::string dst_str = install_decrypt_folder(pref_path, install_path).generic_string();''')
replace(inst, '''    lr_log(RETRO_LOG_INFO, "PFS decryption succeeded, replacing encrypted files...\\n");
    try {
        fs::remove_all(install_path);
        fs::rename(fs::path(dst_str), install_path);''', '''    lr_log(RETRO_LOG_INFO, "PFS decryption succeeded, replacing encrypted files...\\n");
    try {
        fs::remove_all(install_path);
        fs::create_directories(install_path.parent_path());
        fs::rename(fs::path(dst_str), install_path);''')
replace(inst, '''    // An encrypted one twice over for a while: decrypting writes a second copy
    if (!has_room(pref_path, folder_size(content_dir) * (fs::is_directory(content_dir / "sce_pfs") ? 2 : 1)))
        return result;
    try {
        fs::create_directories(install_path);
        fs::copy(content_dir, install_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        lr_log(RETRO_LOG_INFO, "App folder copy completed.\\n");
    } catch (const fs::filesystem_error &e) {
        lr_log(RETRO_LOG_ERROR, "Failed to copy game folder: %s\\n", e.what());
        lr_msg("Failed to copy the game's directory!", 300);
        return result;
    }

    // A NoNpDrm copy is decrypted in place: a game with its own license, kept; an update
    // or DLC with its own or the game's. An update stays in ux0/patch and is laid over the
    // game when it starts, as one from a PKG or VPK/ZIP (sco8487), never merged into it.
    if (fs::is_directory(install_path / "sce_pfs")) {''', '''    // An encrypted game is decrypted straight from its folder into ux0, so it
    // needs its size once; an encrypted update or DLC is still copied first
    // and so needs it twice for a while.
    const bool encrypted_source = fs::is_directory(content_dir / "sce_pfs");
    const bool decrypt_from_source = encrypted_source && app_info.app_category.find("gd") != std::string::npos
        && fs::exists(content_dir / "sce_sys" / "package" / "work.bin");
    if (!has_room(pref_path, folder_size(content_dir) * (encrypted_source && !decrypt_from_source ? 2 : 1)))
        return result;
    if (decrypt_from_source) {
        lr_log(RETRO_LOG_INFO, "Encrypted game folder: decrypting it straight into ux0, without copying it first\\n");
        std::vector<uint8_t> work_bin_data;
        std::string app_zRIF;
        if (!decrypt_nonpdrm_app(install_path, pref_path, result, work_bin_data, app_zRIF, true, content_dir)) {
            fs::remove_all(install_path);
            return result;
        }
    } else {
    try {
        fs::create_directories(install_path);
        fs::copy(content_dir, install_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        lr_log(RETRO_LOG_INFO, "App folder copy completed.\\n");
    } catch (const fs::filesystem_error &e) {
        lr_log(RETRO_LOG_ERROR, "Failed to copy game folder: %s\\n", e.what());
        lr_msg("Failed to copy the game's directory!", 300);
        return result;
    }
    }

    // A NoNpDrm copy is decrypted in place: a game with its own license, kept; an update
    // or DLC with its own or the game's. An update stays in ux0/patch and is laid over the
    // game when it starts, as one from a PKG or VPK/ZIP (sco8487), never merged into it.
    if (!decrypt_from_source && fs::is_directory(install_path / "sce_pfs")) {''')
print('Encrypted game folders are decrypted from where they are.')

# ── Free space queries a game makes must not throw ──────────────────────────
# Games ask for a device's free space (sceIoDevctl, sceAppMgrGetDevInfo); the
# throwing fs::space() would end the app if the query is refused. Report a
# generous size instead.
for relative in ['vita3k/modules/SceLibKernel/SceLibKernel.cpp', 'vita3k/modules/SceDriverUser/SceAppMgrUser.cpp']:
    replace(relative, '    fs::space_info space = fs::space(path);',
            '''    std::error_code space_ec;
    fs::space_info space = fs::space(path, space_ec);
    if (space_ec)
        space = fs::space_info{ 16ULL << 30, 8ULL << 30, 8ULL << 30 };''')
    text = read(relative)
    if '#include <system_error>' not in text:
        write(relative, '#include <system_error>\n' + text)
print('Free space queries no longer throw.')

# ── FAT images from memory ──────────────────────────────────────────────────
# libfat16 walks a file's cluster chain from its start for every 64 KB chunk,
# with a seek and a two-byte read per step: thousands of small disk reads per
# file. The partition images are at most a few hundred MB, so they are read
# into memory once and the read and seek hooks work on that.
replace(sce, '''    LOG_INFO("FAT: extracting {} to {}", partition, fs_utils::path_to_utf8(vita_fs_path / partition.substr(0, 3)));
    Fat16::Image img(
        f,
        // Read hook
        [](void *userdata, void *buffer, std::uint32_t size) -> std::uint32_t {
            return static_cast<std::uint32_t>(fread(buffer, 1, size, (FILE *)userdata));
        },
        // Seek hook
        [](void *userdata, std::uint32_t offset, int mode) -> std::uint32_t {
            fseek((FILE *)userdata, offset, (mode == Fat16::IMAGE_SEEK_MODE_BEG ? SEEK_SET : (mode == Fat16::IMAGE_SEEK_MODE_CUR ? SEEK_CUR : SEEK_END)));

            return ftell((FILE *)userdata);
        });''', '''    LOG_INFO("FAT: extracting {} to {}", partition, fs_utils::path_to_utf8(vita_fs_path / partition.substr(0, 3)));
    struct MemoryImage {
        std::vector<std::uint8_t> data;
        std::size_t position = 0;
    } memory;
    fseek(f, 0, SEEK_END);
    const long image_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (image_size > 0) {
        memory.data.resize(static_cast<std::size_t>(image_size));
        if (fread(memory.data.data(), 1, memory.data.size(), f) != memory.data.size()) {
            LOG_ERROR("FAT: could not read {}", partition);
            fclose(f);
            return;
        }
    }
    Fat16::Image img(
        &memory,
        // Read hook
        [](void *userdata, void *buffer, std::uint32_t size) -> std::uint32_t {
            auto *m = static_cast<MemoryImage *>(userdata);
            const std::size_t left = m->position < m->data.size() ? m->data.size() - m->position : 0;
            const std::size_t take = std::min<std::size_t>(size, left);
            if (take)
                std::memcpy(buffer, m->data.data() + m->position, take);
            m->position += take;
            return static_cast<std::uint32_t>(take);
        },
        // Seek hook
        [](void *userdata, std::uint32_t offset, int mode) -> std::uint32_t {
            auto *m = static_cast<MemoryImage *>(userdata);
            const std::size_t base = mode == Fat16::IMAGE_SEEK_MODE_BEG ? 0
                : mode == Fat16::IMAGE_SEEK_MODE_CUR ? m->position : m->data.size();
            m->position = std::min<std::size_t>(base + offset, m->data.size());
            return static_cast<std::uint32_t>(m->position);
        });''')
text = read(sce)
for header in ['#include <algorithm>', '#include <cstring>']:
    if header not in text:
        text = header + '\n' + text
write(sce, text)
print('FAT images are unpacked from memory.')

# ── Log level ──────────────────────────────────────────────────────────────
# At trace level the shader recompiler logs every instruction it translates;
# on the console each shader took over a second, mostly spent logging.
replace('vita3k/libretro/libretro_log.cpp', '    logger->set_level(spdlog::level::trace);',
        '    // Info, not trace: per-instruction recompiler logging slowed every shader.\n    logger->set_level(spdlog::level::info);')
replace('vita3k/libretro/libretro_log.cpp', '    spdlog::set_level(spdlog::level::trace);', '    spdlog::set_level(spdlog::level::info);')
print('Core logging at info level.')
# The emulator also applies its config's log level when it starts a game;
# its default is trace too.
replace('vita3k/config/include/config/config.h', 'code(int, "log-level", 0 /*SPDLOG_LEVEL_TRACE*/, log_level)',
        'code(int, "log-level", 2 /*SPDLOG_LEVEL_INFO*/, log_level)')
# A config.yml written by an earlier build may still say trace: clamp it.
replace('vita3k/interface.cpp', 'logging::set_level(static_cast<spdlog::level::level_enum>(emuenv.cfg.log_level));',
        'logging::set_level(static_cast<spdlog::level::level_enum>(std::max(emuenv.cfg.log_level, 2)));')
replace('vita3k/config/src/config.cpp', 'logging::set_level(static_cast<spdlog::level::level_enum>(cfg.log_level));',
        'logging::set_level(static_cast<spdlog::level::level_enum>(std::max(cfg.log_level, 2)));')
print('Log level clamped to info.')
text = read('vita3k/interface.cpp')
if '#include <algorithm>' not in text:
    write('vita3k/interface.cpp', '#include <algorithm>\n' + text)

# ── Where does app::init spend its time? ────────────────────────────────────
# One console run took about 29 s between "VitaFS path" and "app::init
# succeeded" with nothing logged in between. The three steps that sit there
# now log how long they took.
app_init = 'vita3k/app/src/app_init.cpp'
text = read(app_init)
if '#include <chrono>' not in text:
    write(app_init, '#include <chrono>\n' + text)
replace(app_init, '''    set_current_config(state, state.cfg.run_app_path.has_value() ? *state.cfg.run_app_path : "");

    // Here any path''', '''    const auto init_started = std::chrono::steady_clock::now();
    const auto init_elapsed = [&init_started]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - init_started).count(); };
    set_current_config(state, state.cfg.run_app_path.has_value() ? *state.cfg.run_app_path : "");
    LOG_INFO("app::init: config read in {:.1f} s", init_elapsed());

    // Here any path''')
replace(app_init, '''        LOG_ERROR("Failed to initialize file system for the emulator!");
        return false;
    }
''', '''        LOG_ERROR("Failed to initialize file system for the emulator!");
        return false;
    }
    LOG_INFO("app::init: folders created at {:.1f} s", init_elapsed());
''')
replace(app_init, '''    state.motion.init();
''', '''    state.motion.init();
    LOG_INFO("app::init: motion ready at {:.1f} s", init_elapsed());
''')
print('app::init timing added.')


# ── The frame's way out, measured ───────────────────────────────────────────
# With the three changes above the game's frames were still black on Xbox,
# while the overlay showed. Two steps between the game and the frontend were
# never seen working on Dozen: the game's surface itself, and the blit that
# copies it into the image handed over (the overlay is drawn straight into
# that image). The surface is read back at nine points each time it is
# presented (logged at first and every 600 frames): zeros there mean the
# game's draws never reached it.
lv = 'vita3k/libretro/libretro_vita3k.cpp'
replace(lv, '''    std::array<vk::Framebuffer, 3> overlay_framebuffers;
} s_present;''', '''    std::array<vk::Framebuffer, 3> overlay_framebuffers;
#ifdef VITA3K_UWP
    // nine texels of the presented surface, read back for the log
    vkutil::Buffer probe;
    uint64_t presents = 0;
    vk::Format last_format = vk::Format::eUndefined;
    uint32_t last_w = 0, last_h = 0;
#endif
} s_present;''')
replace(lv, '''    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
    if (surface && surface_format == vk::Format::eR8G8B8A8Srgb) {''', '''    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
#ifdef VITA3K_UWP
    bool probed = false;
    // Only on the frames whose texels get logged (below): nine copies and a
    // layout change through Dozen every frame cost frame time for nothing.
    const uint64_t next_present = s_present.presents + 1;
    const bool probe_now = next_present == 1 || next_present % 600 == 0 || surface_format != s_present.last_format ||
        w != s_present.last_w || h != s_present.last_h;
    if (surface && w > 2 && h > 2 && probe_now) {
        if (!s_present.probe.buffer) {
            static constexpr vma::AllocationCreateInfo readback_alloc = {
                .flags = vma::AllocationCreateFlagBits::eHostAccessRandom | vma::AllocationCreateFlagBits::eMapped,
                .usage = vma::MemoryUsage::eAuto,
                .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            };
            s_present.probe = vkutil::Buffer(9 * 512);
            s_present.probe.init_buffer(vk::BufferUsageFlagBits::eTransferDst, readback_alloc);
        }
        std::array<vk::BufferImageCopy, 9> taps{};
        for (uint32_t i = 0; i < 9; i++) {
            taps[i] = vk::BufferImageCopy{
                .bufferOffset = i * 512, // D3D12 copies need 512-byte placement
                .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                .imageOffset = { static_cast<int32_t>(ox + w * (1 + i % 3) / 4), static_cast<int32_t>(oy + h * (1 + i / 3) / 4), 0 },
                .imageExtent = { 1, 1, 1 },
            };
        }
        cmd.copyImageToBuffer(surface, vk::ImageLayout::eGeneral, s_present.probe.buffer, taps);
        probed = true;
    }
#endif
    if (surface && surface_format == vk::Format::eR8G8B8A8Srgb) {''')
replace(lv, '''    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
    vk_state.device.resetFences(s_present.fence);
''', '''    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
    vk_state.device.resetFences(s_present.fence);
#ifdef VITA3K_UWP
    s_present.presents++;
    const bool changed = surface_format != s_present.last_format || w != s_present.last_w || h != s_present.last_h;
    if (changed || s_present.presents == 1 || s_present.presents % 600 == 0) {
        s_present.last_format = surface_format;
        s_present.last_w = w;
        s_present.last_h = h;
        if (probed && s_present.probe.mapped_data) {
            const auto *t = static_cast<const uint32_t *>(s_present.probe.mapped_data);
            lr_log(RETRO_LOG_INFO, "Present %llu: surface format %d, %ux%u at %u,%u; texels %08x %08x %08x %08x %08x %08x %08x %08x %08x\\n",
                static_cast<unsigned long long>(s_present.presents), static_cast<int>(surface_format), w, h, ox, oy,
                t[0], t[128], t[256], t[384], t[512], t[640], t[768], t[896], t[1024]);
        } else {
            lr_log(RETRO_LOG_INFO, "Present %llu: %s, %ux%u\\n", static_cast<unsigned long long>(s_present.presents),
                surface ? "surface (not probed)" : (pixels ? "frame from memory" : "no frame yet"), w, h);
        }
    }
#endif
''')
print('Presentation: surface probe in the log.')

# ── The render thread does not take the app down ────────────────────────────
# A Vulkan error on the libretro render thread (a lost D3D12 device, for one)
# threw out of the thread, and std::terminate ended the app with nothing in
# the log. Caught now: logged, the render thread stops, and the frontend's
# quick menu can still quit the game.
replace(lv, '''        libretro.render_thread = std::make_unique<std::thread>([]() {
            EmuEnvState &emuenv = *libretro.emuenv;
''', '''        libretro.render_thread = std::make_unique<std::thread>([]() {
            EmuEnvState &emuenv = *libretro.emuenv;
#ifdef VITA3K_UWP
            try {
#endif
''')
replace(lv, '''                    }
                }
            }
        });
    }
    
    lr_trace("context_reset_finalize.complete", "finished");''', '''                    }
                }
            }
#ifdef VITA3K_UWP
            } catch (const vk::DeviceLostError &e) {
                lr_log(RETRO_LOG_ERROR, "Render thread stopped: the GPU device was lost (%s). The game cannot draw any more; quit it from the menu.\\n", e.what());
            } catch (const std::exception &e) {
                lr_log(RETRO_LOG_ERROR, "Render thread stopped by an error: %s\\n", e.what());
            } catch (...) {
                lr_log(RETRO_LOG_ERROR, "Render thread stopped by an unknown error\\n");
            }
#endif
        });
    }
    
    lr_trace("context_reset_finalize.complete", "finished");''')
print('Render thread errors are logged instead of ending the app.')


# ── Updates and DLC on the frontend's request ───────────────────────────────
# The Xbox app has an Updates and DLC folder, like its games folder. Each
# update or DLC there (a folder, .vpk/.zip or .pkg) is installed the way one
# loaded on its own is: into ux0/patch or ux0/addcont of its title.
addon_inst = source / 'vita3k/libretro/libretro_installer.cpp'
addon_text = addon_inst.read_text(encoding='utf-8')
assert 'static GameInstallResult install_or_open(' in addon_text
addon_text += '''
#if defined(VITA3K_UWP)
extern "C" __declspec(dllexport) int vita3k_uwp_install_addon(const char *pref_utf8, const char *path_utf8,
    char *title_id, uint32_t title_size, char *category, uint32_t category_size) {
    if (!pref_utf8 || !path_utf8)
        return 0;
    GameInstallResult result;
    try {
        result = install_or_open(fs_utils::utf8_to_path(path_utf8), fs_utils::utf8_to_path(pref_utf8), 1, false);
    } catch (const std::exception &e) {
        lr_log(RETRO_LOG_ERROR, "Installing %s failed: %s\\n", path_utf8, e.what());
        return 0;
    } catch (...) {
        lr_log(RETRO_LOG_ERROR, "Installing %s failed\\n", path_utf8);
        return 0;
    }
    if (title_id && title_size)
        snprintf(title_id, title_size, "%s", result.title_id.c_str());
    if (category && category_size)
        snprintf(category, category_size, "%s", result.category.c_str());
    return result.success ? 1 : 0;
}
#endif
'''
addon_inst.write_text(addon_text, encoding='utf-8')
print('Updates and DLC can be installed on request from the frontend.')

# ── Performance overlay: the game's own frame rate ──────────────────────────
# The Xbox app's overlay shows how many frames the game itself finishes a
# second (each sceDisplaySetFrameBuf), beside what reaches the screen.
display = 'vita3k/modules/SceDisplay/SceDisplay.cpp'
replace(display, '#include "SceDisplay.h"\n', '''#include "SceDisplay.h"

#ifdef VITA3K_UWP
#include <atomic>
#include <cstdint>
static std::atomic<std::uint64_t> s_uwp_guest_frames{ 0 };
extern "C" __declspec(dllexport) std::uint64_t vita3k_uwp_guest_frame_count() {
    return s_uwp_guest_frames.load(std::memory_order_relaxed);
}
#endif
''')
replace(display, '''    emuenv.display.last_setframe_vblank_count = emuenv.display.vblank_count.load();
    emuenv.frame_count++;
''', '''    emuenv.display.last_setframe_vblank_count = emuenv.display.vblank_count.load();
    emuenv.frame_count++;
#ifdef VITA3K_UWP
    s_uwp_guest_frames.fetch_add(1, std::memory_order_relaxed);
#endif
''')
print('Guest frame count exported for the frontend overlay.')

# ── Say what the GPU layer allows: memory mapping and surface sync ──────────
# Surface sync (the GPU's pictures copied back for the game to read) only runs
# when the Vulkan layer lets the renderer map guest memory, which needs buffer
# device address and uniform buffer standard layout. The log now says which.
replace('vita3k/libretro/libretro_vita3k.cpp', '''    libretro.renderer_ready = true;
    lr_log(RETRO_LOG_INFO, "Vulkan context reset complete\\n");''', '''    libretro.renderer_ready = true;
    if (libretro.emuenv && libretro.emuenv->renderer) {
        const bool mapping = libretro.emuenv->renderer->features.enable_memory_mapping;
        lr_log(RETRO_LOG_INFO, "GPU memory mapping: %s; surface sync: %s\\n", mapping ? "on" : "off (this GPU layer cannot map guest memory)",
            (mapping && !libretro.emuenv->renderer->disable_surface_sync) ? "working" : "not running");
    }
    lr_log(RETRO_LOG_INFO, "Vulkan context reset complete\\n");''')
print('Core logs memory mapping and surface sync state.')

# ── Descriptor pools that fit Dozen's sampler heap ──────────────────────────
# Each new texture descriptor pool holds 64 sets per frame, three frames:
# textures_count * 192 samplers. Dozen caps one pool's sampler heap at 1024,
# so six textures (1152) failed allocateDescriptorSets and stopped the render
# thread (Gravity Rush). The pack shrinks so a pool stays within 1024.
scene = 'vita3k/renderer/src/vulkan/scene.cpp'
text = read(scene)
start = text.index('static vk::DescriptorSet retrieve_descriptor(VKContext &context, bool is_vertex, uint16_t textures_count) {')
end = text.index('static void draw_bind_descriptors', start)
body = text[start:end]
body = body.replace('DESCRIPTOR_PACK_SIZE', 'pack')
body = body.replace('''    // we have no more frame descriptor available, create a bunch of new one for this specific layout
    vk::DescriptorPoolSize pool_size{''', '''    // we have no more frame descriptor available, create a bunch of new one for this specific layout
    // (no more than 1024 samplers in one pool: Dozen's limit)
    const uint32_t pack = (std::max)(1u, (std::min)(DESCRIPTOR_PACK_SIZE, 1024u / (textures_count * MAX_FRAMES_RENDERING)));
    vk::DescriptorPoolSize pool_size{''', 1)
assert 'Dozen\'s limit' in body, 'scene.cpp retrieve_descriptor anchor'
text = text[:start] + body + text[end:]
if '#include <algorithm>' not in text:
    text = text.replace('#include <renderer/vulkan/functions.h>', '#include <algorithm>\n#include <renderer/vulkan/functions.h>', 1)
    assert '#include <algorithm>' in text, 'scene.cpp include anchor'
write(scene, text)
print('Texture descriptor pools stay within Dozen\'s 1024-sampler heap.')

# ── Images without a format ─────────────────────────────────────────────────
# A Vita format with no Vulkan equivalent reached init_image as
# VK_FORMAT_UNDEFINED (Gravity Rush, a 256x128 cast texture). Desktop drivers
# let it slide; Dozen has no size for it, the allocation fails and the render
# thread stops. Such an image is made RGBA8 instead, and logged once.
replace('vita3k/vkutil/src/objects.cpp', '''void Image::init_image(vk::ImageUsageFlags usage, vk::ComponentMapping mapping, const vk::ImageCreateFlags image_create_flags, const void *pNext) {
    vk::ImageCreateInfo image_info{''', '''void Image::init_image(vk::ImageUsageFlags usage, vk::ComponentMapping mapping, const vk::ImageCreateFlags image_create_flags, const void *pNext) {
    if (format == vk::Format::eUndefined) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_WARN("Image {}x{} has no Vulkan format; made RGBA8 instead", width, height);
        }
        format = vk::Format::eR8G8B8A8Unorm;
    }
    vk::ImageCreateInfo image_info{''')
print('Format-less images are created as RGBA8.')

# ── OpenGL objects judged by a stale error ──────────────────────────────────
# GLObjectArray::init reported failure whenever glGetError held an error left
# by any earlier call; the surface cache then dropped a render target and the
# game died (Danganronpa on OpenGL, at its first scene). Old errors are
# cleared first, and the objects count as made when they have names.
replace('vita3k/glutil/include/glutil/object_array.h', '''        this->deleter = deleter;
        generator(static_cast<GLsizei>(names.size()), &names[0]);

        return glGetError() == GL_NO_ERROR;''', '''        this->deleter = deleter;
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
        }
        generator(static_cast<GLsizei>(names.size()), &names[0]);

        if (glGetError() != GL_NO_ERROR)
            return false;
        for (const GLuint name : names)
            if (name == 0)
                return false;
        return true;''')
print('GL object creation ignores errors left by earlier calls.')

# ── 4-bit-per-channel textures on Dozen ─────────────────────────────────────
# The Vita's U4U4U4U4 textures are created as VK_FORMAT_R4G4B4A4_UNORM_PACK16.
# Dozen maps that to DXGI_FORMAT_A4B4G4R4_UNORM only when the GPU has it; the
# Xbox does not, so the image got DXGI_FORMAT_UNKNOWN, had no allocation size
# and stopped the render thread (Gravity Rush, a 256x128 texture). Dozen does
# support B4G4R4A4 for sampling and copies: the same 16 bits with red and blue
# swapped. Such textures are made B4G4R4A4 and the view swaps R and B back,
# so the bytes upload unchanged and the shader sees the right channels.
texture = 'vita3k/renderer/src/vulkan/texture.cpp'
text = read(texture)
start = text.index('void VKTextureCache::configure_texture(const SceGxmTexture &gxm_texture) {')
end = text.index('\n}\n', start)
body = text[start:end]
old_gamma = '''    if (gxm_texture.gamma_mode)
        vk_format = linear_to_srgb(vk_format);
'''
assert body.count(old_gamma) == 1, 'configure_texture gamma anchor'
body = body.replace(old_gamma, old_gamma + '''
    vk::ComponentMapping view_swizzle = swizzle;
    if (vk_format == vk::Format::eR4G4B4A4UnormPack16) {
        static int rgba4_support = -1;
        if (rgba4_support < 0) {
            const vk::FormatProperties props = state.physical_device.getFormatProperties(vk::Format::eR4G4B4A4UnormPack16);
            rgba4_support = (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) ? 1 : 0;
            if (!rgba4_support)
                LOG_INFO("R4G4B4A4 textures are not supported by the GPU; stored as B4G4R4A4 with red and blue swapped back");
        }
        if (!rgba4_support) {
            vk_format = vk::Format::eB4G4R4A4UnormPack16;
            const auto swap_rb = [](vk::ComponentSwizzle c, vk::ComponentSwizzle self) {
                if (c == vk::ComponentSwizzle::eIdentity)
                    c = self;
                if (c == vk::ComponentSwizzle::eR)
                    return vk::ComponentSwizzle::eB;
                if (c == vk::ComponentSwizzle::eB)
                    return vk::ComponentSwizzle::eR;
                return c;
            };
            view_swizzle = vk::ComponentMapping{
                swap_rb(swizzle.r, vk::ComponentSwizzle::eR),
                swap_rb(swizzle.g, vk::ComponentSwizzle::eG),
                swap_rb(swizzle.b, vk::ComponentSwizzle::eB),
                swap_rb(swizzle.a, vk::ComponentSwizzle::eA),
            };
        }
    }
''')
assert body.count('        .components = swizzle,\n') == 1, 'configure_texture view anchor'
body = body.replace('        .components = swizzle,\n', '        .components = view_swizzle,\n')
write(texture, text[:start] + body + text[end:])
print('U4U4U4U4 textures use B4G4R4A4 with an R/B swapped view when R4G4B4A4 is missing.')

# ── OpenGL on Direct3D 12: programmable blending from a copy ───────────────
# With GL_ARB_texture_barrier (which Mesa's D3D12 driver advertises) Vita3K's
# GL renderer binds the render target as a read-write image in every shader
# that reads the frame's colour, and the mask bit adds another read-write
# RGBA8 image to every draw. On the Xbox any image (UAV) in a pixel shader
# ended the GPU device: Danganronpa (mask bit) at its first draws, and in
# build 70 Minecraft and Gravity Rush at their first shader reading the
# frame's colour from a copy bound as an image (GL_OUT_OF_MEMORY, then the
# device gone). Turning texture barrier off kept them alive but those shaders
# then read nothing (Minecraft black, Gravity Rush's interface white). So
# such shaders now read the frame's colour with texelFetch from a texture: a
# copy of the render target made just before the draw (the guarantee texture
# barrier gives: earlier draws are visible), bound to texture unit 32 (the
# game's textures use 0-31). No images at all: shader interlock and the mask
# bit stay off.
gl_renderer = 'vita3k/renderer/src/gl/renderer.cpp'
replace(gl_renderer, '''    return gl_state.init();
}''', '''#ifdef VITA3K_UWP
    gl_state.features.support_shader_interlock = false;
    gl_state.features.use_mask_bit = false;
    {
        GLint combined_units = 0;
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &combined_units);
        if (combined_units <= shader::UWP_COLOR_ATTACHMENT_TEXTURE_UNIT)
            gl_state.features.support_texture_barrier = false;
        LOG_INFO("OpenGL on Direct3D 12: programmable blending {} (texture units {}); shader interlock and the mask bit are off",
            gl_state.features.support_texture_barrier ? "reads a copy of the render target as a texture" : "is off", combined_units);
    }
#endif
    return gl_state.init();
}''')
# Shaders cached by earlier builds were made with other features.
replace(gl_renderer, '''    shader_version = fmt::format("v{}", shader::CURRENT_VERSION);''', '''#ifdef VITA3K_UWP
    shader_version = fmt::format("v{}-d3d12gl3", shader::CURRENT_VERSION);
#else
    shader_version = fmt::format("v{}", shader::CURRENT_VERSION);
#endif''')

gl_draw = 'vita3k/renderer/src/gl/draw.cpp'
replace(gl_draw, '''void draw(GLState &renderer, GLContext &context, const FeatureState &features, SceGxmPrimitiveType type, SceGxmIndexFormat format, void *indices, size_t count, uint32_t instance_count,
    MemState &mem, const Config &config) {''', '''#ifdef VITA3K_UWP
// The render target's contents as they are now, in a texture of the same
// size and format, for a shader that reads the frame's colour as an image:
// Direct3D 12 cannot bind the render target itself as one while drawing to
// it. A few copies are kept, one per size and format.
static GLuint uwp_color_attachment_copy(GLuint attachment) {
    struct Copy {
        GLuint texture = 0;
        GLint format = 0;
        GLint width = 0;
        GLint height = 0;
        bool convert = false; // the copy's sized format differs from the source's unsized one
    };
    static Copy copies[4];
    static int next = 0;
    if (!attachment)
        return 0;
    GLint format = 0, width = 0, height = 0;
    glGetTextureLevelParameteriv(attachment, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    glGetTextureLevelParameteriv(attachment, 0, GL_TEXTURE_WIDTH, &width);
    glGetTextureLevelParameteriv(attachment, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0 || format == 0)
        return attachment;
    Copy *copy = nullptr;
    for (auto &c : copies) {
        if (c.texture && c.format == format && c.width == width && c.height == height) {
            copy = &c;
            break;
        }
    }
    if (!copy) {
        copy = &copies[next];
        next = (next + 1) % 4;
        if (copy->texture)
            glDeleteTextures(1, &copy->texture);
        glCreateTextures(GL_TEXTURE_2D, 1, &copy->texture);
        // Storage takes a sized format: render targets made with an unsized
        // one (GL_RGBA, Persona 4 Golden's) got a copy with no storage at all,
        // and their programmable blending read nothing (parts of the picture
        // black or missing)
        GLenum storage = static_cast<GLenum>(format);
        switch (storage) {
        case GL_RGBA: storage = GL_RGBA8; break;
        case GL_RGB: storage = GL_RGB8; break;
        case GL_RG: storage = GL_RG8; break;
        case GL_RED: storage = GL_R8; break;
        case GL_BGRA: storage = GL_RGBA8; break;
        default: break;
        }
        glTextureStorage2D(copy->texture, 1, storage, width, height);
        copy->convert = storage != static_cast<GLenum>(format);
        copy->format = format;
        copy->width = width;
        copy->height = height;
        static int made = 0;
        if (made < 8) {
            made++;
            LOG_INFO("Programmable blending: render target copy {}x{} format 0x{:X}", width, height, format);
        }
    }
    if (copy->convert) {
        // glCopyImageSubData wants identical formats and refused GL_RGBA to
        // GL_RGBA8 (GL_INVALID_OPERATION, internalFormat mismatch: the copy
        // stayed empty and Persona 4 Golden's background came out black).
        // Reading the target through a framebuffer converts as it copies.
        static GLuint read_fbo = 0;
        if (!read_fbo)
            glGenFramebuffers(1, &read_fbo);
        GLint previous_read = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, attachment, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glCopyTextureSubImage2D(copy->texture, 0, 0, 0, 0, 0, width, height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    } else {
        glCopyImageSubData(attachment, GL_TEXTURE_2D, 0, 0, 0, 0, copy->texture, GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);
    }
    return copy->texture;
}
#endif

void draw(GLState &renderer, GLContext &context, const FeatureState &features, SceGxmPrimitiveType type, SceGxmIndexFormat format, void *indices, size_t count, uint32_t instance_count,
    MemState &mem, const Config &config) {''')
replace(gl_draw, '''    if (fragment_program_gxp.is_frag_color_used() && features.is_programmable_blending_need_to_bind_color_attachment()) {
        if (use_raw_image) {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        } else {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        }
    }''', '''    if (fragment_program_gxp.is_frag_color_used() && features.is_programmable_blending_need_to_bind_color_attachment()) {
#ifdef VITA3K_UWP
        // texelFetch from a copy made now, bound as a texture (see the shader
        // generator): Direct3D 12 here takes no image in a pixel shader
        (void)use_raw_image;
        (void)surface_format;
        glActiveTexture(GL_TEXTURE0 + shader::UWP_COLOR_ATTACHMENT_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D, uwp_color_attachment_copy(context.current_color_attachment));
#else
        if (use_raw_image) {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        } else {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        }
#endif
    }''')
# The copy above stands for the barrier; and a texture sampled while it is
# the render target (a feedback loop Direct3D 12 forbids too) is read from
# Vita3K's ping-pong copy, its path without texture barrier.
replace(gl_draw, '''        } else if (features.should_use_texture_barrier()) {
            // Needs texture barrier
            glTextureBarrier();
        }''', '''        } else if (features.should_use_texture_barrier()) {
#ifndef VITA3K_UWP
            // Needs texture barrier
            glTextureBarrier();
#endif
        }''')
replace(gl_draw, '''    } else if (!context.self_sampling_indices.empty()) {
        if (features.support_texture_barrier) {''', '''    } else if (!context.self_sampling_indices.empty()) {
#ifdef VITA3K_UWP
        if (false) {
#else
        if (features.support_texture_barrier) {
#endif''')
spirv_h = 'vita3k/shader/include/shader/spirv_recompiler.h'
replace(spirv_h, '''static constexpr int COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE = 3;''', '''static constexpr int COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE = 3;
#ifdef VITA3K_UWP
// OpenGL on Direct3D 12: the texture unit the frame's colour is read from
// (a sampler2D, texelFetch) instead of an image; the game's use 0-31.
static constexpr int UWP_COLOR_ATTACHMENT_TEXTURE_UNIT = 32;
#endif''')
spirv_cpp = 'vita3k/shader/src/spirv_recompiler.cpp'
replace(spirv_cpp, '''    bool is_vulkan = false;
    spv::ImageFormat image_storage_format = spv::ImageFormat::ImageFormatUnknown;''', '''    bool is_vulkan = false;
    // OpenGL on Direct3D 12: the frame's colour is a sampler2D, read with
    // texelFetch, not a storage image
    bool sampled_color_attachment = false;
    spv::ImageFormat image_storage_format = spv::ImageFormat::ImageFormatUnknown;''')
replace(spirv_cpp, '''            // Create a global sampler, which is our color attachment
            spv::Id color_attachment = create_builtin_sampler(b, features, translation_state, "f_colorAttachment");
            b.addDecoration(color_attachment, spv::DecorationCoherent);
            b.setPrecision(color_attachment, precision);
            spv::Id color_attachment_raw = spv::NoResult;

            if (translation_state.is_vulkan) {
                b.addDecoration(color_attachment, spv::DecorationBinding, 0);
                b.addDecoration(color_attachment, spv::DecorationDescriptorSet, 1);
            } else {
                b.addDecoration(color_attachment, spv::DecorationBinding, COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE);
            }''', '''#ifdef VITA3K_UWP
            translation_state.sampled_color_attachment = !translation_state.is_vulkan && !features.support_shader_interlock;
#endif
            // Create a global sampler, which is our color attachment
            spv::Id color_attachment = translation_state.sampled_color_attachment
                ? create_param_sampler(b, "f_colorAttachment", spv::Dim2D)
                : create_builtin_sampler(b, features, translation_state, "f_colorAttachment");
            if (!translation_state.sampled_color_attachment)
                b.addDecoration(color_attachment, spv::DecorationCoherent);
            b.setPrecision(color_attachment, precision);
            spv::Id color_attachment_raw = spv::NoResult;

            if (translation_state.is_vulkan) {
                b.addDecoration(color_attachment, spv::DecorationBinding, 0);
                b.addDecoration(color_attachment, spv::DecorationDescriptorSet, 1);
            } else {
#ifdef VITA3K_UWP
                b.addDecoration(color_attachment, spv::DecorationBinding,
                    translation_state.sampled_color_attachment ? UWP_COLOR_ATTACHMENT_TEXTURE_UNIT : COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE);
#else
                b.addDecoration(color_attachment, spv::DecorationBinding, COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE);
#endif
            }''')
replace(spirv_cpp, '''            } else {
                source = b.createOp(spv::OpImageRead, v4, { b.createLoad(color_attachment, spv::NoPrecision), current_coord });
                b.setPrecision(source, precision);''', '''            } else {
                if (translation_state.sampled_color_attachment) {
                    // texelFetch(f_colorAttachment, coord, 0)
                    const spv::Id image_type = b.makeImageType(b.makeFloatType(32), spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
                    const spv::Id image = b.createUnaryOp(spv::OpImage, image_type, b.createLoad(color_attachment, spv::NoPrecision));
                    source = b.createOp(spv::OpImageFetch, v4,
                        std::vector<spv::IdImmediate>{ { true, image }, { true, current_coord },
                            { false, static_cast<unsigned>(spv::ImageOperandsLodMask) }, { true, b.makeIntConstant(0) } });
                } else {
                    source = b.createOp(spv::OpImageRead, v4, { b.createLoad(color_attachment, spv::NoPrecision), current_coord });
                }
                b.setPrecision(source, precision);''')
print('OpenGL on D3D12: programmable blending reads a copy of the render target as a texture; own shader cache version.')

# Say why a framebuffer is incomplete, with its attachments' formats.
replace('vita3k/renderer/src/gl/surface_cache.cpp', '''    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LOG_ERROR("Framebuffer is not completed. Proceed anyway...");''', '''    if (const GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER); fb_status != GL_FRAMEBUFFER_COMPLETE) {
        static int reported = 0;
        if (reported < 8) {
            reported++;
            GLint color_format = 0, color_w = 0, color_h = 0, ds_format = 0, ds_w = 0, ds_h = 0;
            if (color_handle) {
                glBindTexture(GL_TEXTURE_2D, color_handle);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &color_format);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &color_w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &color_h);
            }
            if (ds_handle) {
                glBindTexture(GL_TEXTURE_2D, ds_handle);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ds_format);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &ds_w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ds_h);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            LOG_ERROR("Framebuffer is not complete (status 0x{:X}): color texture {} format 0x{:X} {}x{} (Vita color format 0x{:X}), depth/stencil texture {} format 0x{:X} {}x{}. Proceeding anyway",
                fb_status, color_handle, color_format, color_w, color_h, color ? static_cast<uint32_t>(gxm::get_base_format(color->colorFormat)) : 0u,
                ds_handle, ds_format, ds_w, ds_h);
        }
    }''')
print('Incomplete GL framebuffers are described in the log.')

# ── Vulkan: images in formats the GPU lacks get one it has ──────────────────
# Dozen (Vulkan on the Xbox's Direct3D 12) has no resource format for
# R4G4B4A4 or A2R10G10B10, and an image made in one fails to allocate
# ("no allocation size ... format 0"). configure_texture handled R4G4B4A4
# textures, but Gravity Rush's render-to-texture surfaces are sampled through
# images the surface cache makes in the texture's format, and it stopped
# drawing at the first. Every vkutil image now asks the GPU first: a format it
# lacks for that use becomes the same layout with red and blue swapped (and
# the view swapping them back), or failing that another format of the same
# size, so the image exists; the log names each such format once.
objects_h = 'vita3k/vkutil/include/vkutil/objects.h'
replace(objects_h, '''void init(vma::Allocator vma_allocator);
void deinit();''', '''void init(vma::Allocator vma_allocator);
void deinit();

#ifdef VITA3K_UWP
// format if the GPU can make an image of it for this use; otherwise one it
// can, of the same size. swap_red_blue: the image's data comes from the
// guest's memory in the original layout (a texture upload), so where the
// substitute has red and blue the other way round, mapping swaps them back.
// Images the GPU writes (render targets, copies of them) keep the plain
// mapping: what is written through the substitute reads back the same.
vk::Format supported_image_format(vk::Format format, vk::ImageUsageFlags usage, vk::ImageCreateFlags flags, vk::ComponentMapping &mapping, bool swap_red_blue);
#endif''')
objects_cpp = 'vita3k/vkutil/src/objects.cpp'
replace(objects_cpp, '''void deinit() {
    allocator = nullptr;
}''', '''void deinit() {
    allocator = nullptr;
}

#ifdef VITA3K_UWP
static vk::ComponentMapping vkutil_swap_red_blue(const vk::ComponentMapping &m) {
    const auto swap = [](vk::ComponentSwizzle c, vk::ComponentSwizzle self) {
        if (c == vk::ComponentSwizzle::eIdentity)
            c = self;
        if (c == vk::ComponentSwizzle::eR)
            return vk::ComponentSwizzle::eB;
        if (c == vk::ComponentSwizzle::eB)
            return vk::ComponentSwizzle::eR;
        return c;
    };
    return vk::ComponentMapping{
        swap(m.r, vk::ComponentSwizzle::eR),
        swap(m.g, vk::ComponentSwizzle::eG),
        swap(m.b, vk::ComponentSwizzle::eB),
        swap(m.a, vk::ComponentSwizzle::eA),
    };
}

static bool image_format_usable(vk::PhysicalDevice gpu, vk::Format format, vk::ImageUsageFlags usage, vk::ImageCreateFlags flags) {
    vk::ImageFormatProperties properties;
    return gpu.getImageFormatProperties(format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, usage, flags, &properties) == vk::Result::eSuccess;
}

static vk::Format find_supported_image_format(vk::Format format, vk::ImageUsageFlags usage, vk::ImageCreateFlags flags, bool &swapped);

vk::Format supported_image_format(vk::Format format, vk::ImageUsageFlags usage, vk::ImageCreateFlags flags, vk::ComponentMapping &mapping, bool swap_red_blue) {
    if (!allocator || format == vk::Format::eUndefined)
        return format;
    // Asked for every surface lookup: the answers are kept.
    struct Answer {
        vk::Format format;
        vk::ImageUsageFlags usage;
        vk::ImageCreateFlags flags;
        vk::Format result;
        bool swapped;
    };
    static std::mutex answers_mutex;
    static std::vector<Answer> answers;
    bool swapped = false;
    vk::Format result = vk::Format::eUndefined;
    {
        std::lock_guard<std::mutex> lock(answers_mutex);
        for (const Answer &answer : answers) {
            if (answer.format == format && answer.usage == usage && answer.flags == flags) {
                result = answer.result;
                swapped = answer.swapped;
                break;
            }
        }
    }
    if (result == vk::Format::eUndefined) {
        result = find_supported_image_format(format, usage, flags, swapped);
        std::lock_guard<std::mutex> lock(answers_mutex);
        if (answers.size() < 256)
            answers.push_back({ format, usage, flags, result, swapped });
    }
    if (swapped && swap_red_blue)
        mapping = vkutil_swap_red_blue(mapping);
    return result;
}

static vk::Format find_supported_image_format(vk::Format format, vk::ImageUsageFlags usage, vk::ImageCreateFlags flags, bool &swapped_out) {
    swapped_out = false;
    const vk::PhysicalDevice gpu = allocator.getAllocatorInfo().physicalDevice;
    if (!gpu || image_format_usable(gpu, format, usage, flags))
        return format;

    vk::Format substitute = vk::Format::eUndefined;
    bool swapped = false;
    switch (format) {
    case vk::Format::eR4G4B4A4UnormPack16:
        substitute = vk::Format::eB4G4R4A4UnormPack16;
        swapped = true;
        break;
    case vk::Format::eB4G4R4A4UnormPack16:
        substitute = vk::Format::eR4G4B4A4UnormPack16;
        swapped = true;
        break;
    case vk::Format::eA2R10G10B10UnormPack32:
        substitute = vk::Format::eA2B10G10R10UnormPack32;
        swapped = true;
        break;
    case vk::Format::eA2B10G10R10UnormPack32:
        substitute = vk::Format::eA2R10G10B10UnormPack32;
        swapped = true;
        break;
    default:
        break;
    }
    if (substitute != vk::Format::eUndefined && !image_format_usable(gpu, substitute, usage, flags))
        substitute = vk::Format::eUndefined;

    // Same size, another layout: the colours may be off, but the image exists
    // and what is copied into it fits. Not for depth or compressed formats.
    const char *first_component = vk::componentName(format, 0);
    const bool depth_or_stencil = first_component && (first_component[0] == 'D' || first_component[0] == 'S');
    if (substitute == vk::Format::eUndefined && !depth_or_stencil && !vk::isCompressed(format)) {
        swapped = false;
        std::vector<vk::Format> candidates;
        switch (vk::blockSize(format)) {
        case 1:
            candidates = { vk::Format::eR8Unorm };
            break;
        case 2:
            candidates = { vk::Format::eR5G6B5UnormPack16, vk::Format::eR8G8Unorm, vk::Format::eR16Unorm };
            break;
        case 4:
            candidates = { vk::Format::eR8G8B8A8Unorm, vk::Format::eR32Uint };
            break;
        case 8:
            candidates = { vk::Format::eR16G16B16A16Sfloat, vk::Format::eR32G32Uint };
            break;
        case 16:
            candidates = { vk::Format::eR32G32B32A32Sfloat };
            break;
        default:
            break;
        }
        for (const vk::Format candidate : candidates) {
            if (image_format_usable(gpu, candidate, usage, flags)) {
                substitute = candidate;
                break;
            }
        }
    }

    {
        static std::mutex logged_mutex;
        static std::vector<std::pair<vk::Format, vk::ImageUsageFlags>> logged;
        std::lock_guard<std::mutex> lock(logged_mutex);
        if (logged.size() < 32 && std::find(logged.begin(), logged.end(), std::make_pair(format, usage)) == logged.end()) {
            logged.emplace_back(format, usage);
            if (substitute == vk::Format::eUndefined)
                LOG_ERROR("Vulkan: the GPU cannot make {} images for {} and has no substitute", vk::to_string(format), vk::to_string(usage));
            else
                LOG_WARN("Vulkan: the GPU cannot make {} images for {}; made {} instead{}", vk::to_string(format), vk::to_string(usage),
                    vk::to_string(substitute), swapped ? " (red and blue the other way round)" : " (colours may be off)");
        }
    }
    if (substitute == vk::Format::eUndefined)
        return format;
    swapped_out = swapped;
    return substitute;
}
#endif''')
replace(objects_cpp, '''        format = vk::Format::eR8G8B8A8Unorm;
    }
    vk::ImageCreateInfo image_info{''', '''        format = vk::Format::eR8G8B8A8Unorm;
    }
#ifdef VITA3K_UWP
    format = supported_image_format(format, usage, image_create_flags, mapping, false);
#endif
    vk::ImageCreateInfo image_info{''')
replace(objects_cpp, '#include <utility>\n', '#include <vulkan/vulkan_format_traits.hpp>\n\n#include <algorithm>\n#include <mutex>\n#include <utility>\n#include <vector>\n')

vk_texture = 'vita3k/renderer/src/vulkan/texture.cpp'
replace(vk_texture, '''    current_texture->mip_count = mip_count;
    current_texture->is_cube = is_cube;
    uint32_t memory_needed = get_image_memory_upper_bound(gxm_texture, vk_format, base_format);''', '''#ifdef VITA3K_UWP
    vk_format = vkutil::supported_image_format(vk_format, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        is_cube ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags(), view_swizzle, true);
#endif

    current_texture->mip_count = mip_count;
    current_texture->is_cube = is_cube;
    uint32_t memory_needed = get_image_memory_upper_bound(gxm_texture, vk_format, base_format);''')
# Surfaces are looked up by the format Vita3K wants for them; once made in a
# substitute, that lookup has to ask for the substitute too, or every use
# after the first made a view in the original format on an image of the
# other (Gravity Rush's 10-bit lighting target: an A2R10G10B10 view of an
# A2B10G10R10 image, which Direct3D 12 cannot have).
gxm_to_vk = 'vita3k/renderer/src/vulkan/gxm_to_vulkan.cpp'
replace(gxm_to_vk, '''    if (base_format == SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4 && (!support_native_rgba4 || use_shader_interlock))
        return vk::Format::eR8G8B8A8Unorm;

    return translate_format(base_format);
}''', '''    if (base_format == SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4 && (!support_native_rgba4 || use_shader_interlock))
        return vk::Format::eR8G8B8A8Unorm;

#ifdef VITA3K_UWP
    vk::ComponentMapping unchanged = vkutil::default_comp_mapping;
    return vkutil::supported_image_format(translate_format(base_format),
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eColorAttachment
            | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eInputAttachment,
        {}, unchanged, false);
#else
    return translate_format(base_format);
#endif
}''')
if '#include <vkutil/objects.h>' not in read(gxm_to_vk):
    text = read(gxm_to_vk)
    first = text.index('#include')
    write(gxm_to_vk, text[:first] + '#include <vkutil/objects.h>\n' + text[first:])
print('Vulkan images in formats the GPU lacks are made in one it has.')

# ── OpenGL: which formats a game's textures are made in ─────────────────────
# One line per Vita texture format the first time a game uses it, to tell
# texture problems (Gravity Rush's white interface) apart from others.
gl_texture = 'vita3k/renderer/src/gl/texture.cpp'
replace(gl_texture, '''    GLenum internal_format = translate_internal_format(base_format);
    GLenum format = translate_format(base_format);
    const GLenum type = compressed ? 0 : translate_type(base_format);
''', '''    GLenum internal_format = translate_internal_format(base_format);
    GLenum format = translate_format(base_format);
    const GLenum type = compressed ? 0 : translate_type(base_format);
#ifdef VITA3K_UWP
    {
        static std::vector<uint32_t> seen;
        if (seen.size() < 48 && std::find(seen.begin(), seen.end(), static_cast<uint32_t>(fmt)) == seen.end()) {
            seen.push_back(static_cast<uint32_t>(fmt));
            LOG_INFO("GL texture format 0x{:X} (base 0x{:X}): internal 0x{:X}, format 0x{:X}, type 0x{:X}, swizzle 0x{:X} 0x{:X} 0x{:X} 0x{:X}, {}x{}",
                static_cast<uint32_t>(fmt), static_cast<uint32_t>(base_format), internal_format, format, type,
                swizzle[0], swizzle[1], swizzle[2], swizzle[3], width, height);
        }
    }
#endif
''')
replace(gl_texture, '#include <gxm/functions.h>\n', '#include <gxm/functions.h>\n#include <util/log.h>\n\n#include <algorithm>\n#include <vector>\n')
print('OpenGL texture formats are logged once each.')

# ── System dialogs: a way out, and what they were told ──────────────────────
# A save dialog took no button on the console (Danganronpa: "New Saved Data"
# stayed up, and so did the load one): the game could only be closed. The
# dialog's own input runs on Vita3K's overlay thread; this does not depend on
# it. Holding Select (View) for a second and a half cancels a running dialog
# as Circle would; held another second and a half, the dialog is ended
# outright. Each change of a dialog's state, and each button the overlay
# receives, goes to the log, to find why the buttons did nothing.
lr = 'vita3k/libretro/libretro_vita3k.cpp'
replace(lr, '''    libretro_poll_input();
    if (libretro_update_name_entry(emuenv)) {''', '''    libretro_poll_input();
#ifdef VITA3K_UWP
    {
        static int select_frames = 0;
        static int last_type = -1, last_status = -1, last_substatus = -1;
        auto &dialog = emuenv.common_dialog;
        std::lock_guard<std::recursive_mutex> dialog_lock(dialog.mutex);
        if (static_cast<int>(dialog.type) != last_type || static_cast<int>(dialog.status) != last_status
            || static_cast<int>(dialog.substatus) != last_substatus) {
            last_type = static_cast<int>(dialog.type);
            last_status = static_cast<int>(dialog.status);
            last_substatus = static_cast<int>(dialog.substatus);
            LOG_INFO("System dialog: type {}, status {}, substatus {}, result {}", last_type, last_status, last_substatus,
                static_cast<int>(dialog.result));
        }
        const bool running = dialog.type != NO_DIALOG && dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
        const bool select_held = libretro.input_state_cb
            && libretro.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) != 0;
        if (running && select_held) {
            ++select_frames;
            if (select_frames == 90) {
                LOG_WARN("System dialog (type {}) cancelled: Select held", static_cast<int>(dialog.type));
                dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
                if (dialog.type == SAVEDATA_DIALOG) {
                    // as the save dialog's Circle: the game then ends it
                    dialog.savedata.button_id = SCE_SAVEDATA_DIALOG_BUTTON_ID_INVALID;
                    dialog.savedata.draw_info_window = false;
                    dialog.substatus = SCE_COMMON_DIALOG_STATUS_FINISHED;
                } else {
                    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
                }
                emuenv.ctrl.overlay_input_intercepted.store(false, std::memory_order_relaxed);
            } else if (select_frames == 180) {
                LOG_WARN("System dialog (type {}) ended: Select still held", static_cast<int>(dialog.type));
                dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
                dialog.substatus = SCE_COMMON_DIALOG_STATUS_FINISHED;
                dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
                emuenv.ctrl.overlay_input_intercepted.store(false, std::memory_order_relaxed);
            }
        } else if (!select_held) {
            select_frames = 0;
        }
    }
#endif
    if (libretro_update_name_entry(emuenv)) {''')
replace(lr, '#include <emuenv/state.h>\n', '#include <emuenv/state.h>\n' + ''.join(
    f'#include <{h}>\n' for h in ('ctrl/state.h', 'dialog/state.h') if f'#include <{h}>' not in read(lr)))

dialog_cpp = 'vita3k/overlay/src/common_dialog.cpp'
replace(dialog_cpp, '''void common_dialog_overlay::on_button_pressed(pad_button button_press, bool is_auto_repeat) {
    if (!m_dialog)
        return;''', '''void common_dialog_overlay::on_button_pressed(pad_button button_press, bool is_auto_repeat) {
#ifdef VITA3K_UWP
    if (!is_auto_repeat) {
        static int logged = 0;
        if (logged < 64) {
            logged++;
            LOG_INFO("System dialog button {}: dialog type {}, fading in {}, closing {}, {} buttons, {} save slots",
                static_cast<int>(button_press), m_active_type, m_fade_anim.active, m_dismissing, m_buttons.size(), m_save_slots.size());
        }
    }
#endif
    if (!m_dialog)
        return;''')
print('System dialogs: Select held cancels a stuck one; their states and buttons are logged.')

# ── OpenGL: what each shown frame came from ─────────────────────────────────
# Minecraft ran on OpenGL with a black picture throughout. The first frames,
# then one a minute, say whether the frame was a rendered surface (and its
# texture's size) or memory, so a black one can be traced.
replace(lr, '''        // A frame no surface was rendered into (the game wrote it with the
        // CPU, as some do for their first screen): uploaded from memory into a''', '''#ifdef VITA3K_UWP
        {
            static uint64_t presented = 0;
            presented++;
            if (presented <= 5 || presented % 3600 == 0) {
                GLint tex_w = 0, tex_h = 0, tex_format = 0;
                if (surface_tex) {
                    glGetTextureLevelParameteriv(surface_tex, 0, GL_TEXTURE_WIDTH, &tex_w);
                    glGetTextureLevelParameteriv(surface_tex, 0, GL_TEXTURE_HEIGHT, &tex_h);
                    glGetTextureLevelParameteriv(surface_tex, 0, GL_TEXTURE_INTERNAL_FORMAT, &tex_format);
                }
                LOG_INFO("GL frame {}: display buffer 0x{:X} {}x{} pitch {}; {} (texture {}x{} format 0x{:X}, uv {:.3f} {:.3f} {:.3f} {:.3f})",
                    presented, frame.base.address(), frame.image_size.x, frame.image_size.y, frame.pitch,
                    surface_tex ? "rendered surface" : "no surface, from memory", tex_w, tex_h, tex_format,
                    uvs[0], uvs[1], uvs[2], uvs[3]);
            }
        }
#endif
        // A frame no surface was rendered into (the game wrote it with the
        // CPU, as some do for their first screen): uploaded from memory into a''')
print('OpenGL frames say what they were shown from.')

# ── Vulkan on Direct3D 12: no SCALED vertex formats ─────────────────────────
# Dozen says it can read R8G8B8A8_USCALED vertices (it turns that one into
# UINT and converts in the shader), so Vita3K used SCALED formats for every
# attribute; but for the 1-, 2- and 3-component ones (R16G16_SSCALED, ...)
# Dozen's input layout got DXGI_FORMAT_UNKNOWN and the Xbox refused the whole
# pipeline (E_INVALIDARG): Persona 4 Golden ran behind a black screen on
# Vulkan. Vita3K's path for GPUs without SCALED vertex formats reads them as
# integers and converts in the shader; on Direct3D 12 it is taken always. The
# shader cache's feature mask holds this, so old shaders are rebuilt.
pipeline_cache = 'vita3k/renderer/src/vulkan/pipeline_cache.cpp'
replace(pipeline_cache, '''        support_scaled_vertex_attribute = static_cast<bool>(scaled_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer);
''', '''        support_scaled_vertex_attribute = static_cast<bool>(scaled_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer);
#ifdef VITA3K_UWP
        if (support_scaled_vertex_attribute)
            LOG_INFO("Vulkan on Direct3D 12: SCALED vertex attributes are read as integers and converted in the shader");
        support_scaled_vertex_attribute = false;
#endif
''')
print('Vulkan: SCALED vertex attributes converted in the shader (Dozen refused pipelines with them).')

# ── Vulkan: pipelines compiled in the background, known ones too ────────────
# With background compilation on, Vita3K still compiled a pipeline it knew
# from an earlier session (its shaders cached) on the render thread. On the
# Xbox there is no driver cache across sessions, so that compile took the
# console's compiler 30-750 ms each time, and every scene's first frames
# froze (cutscenes and new effects hitched, sound with them). Those go to the
# worker threads as well.
replace(pipeline_cache, '''    const bool compile_pipeline_async = !maskupdate && !context.shader_hints.masked_framebuffer_fetch && !already_in_cache && consider_for_async && use_async_compilation;''', '''#ifdef VITA3K_UWP
    const bool compile_pipeline_async = !maskupdate && !context.shader_hints.masked_framebuffer_fetch && consider_for_async && use_async_compilation;
#else
    const bool compile_pipeline_async = !maskupdate && !context.shader_hints.masked_framebuffer_fetch && !already_in_cache && consider_for_async && use_async_compilation;
#endif''')
print('Vulkan: known pipelines compile in the background too.')

# ── System dialogs: what the game calls while one is open ───────────────────
# Danganronpa and Gravity Rush stopped at their save screens: the slot was
# chosen (the dialog's sub-status went to finished) and then the game never
# called anything that moved the dialog on, and holding Select to end it did
# not bring the game back either. While a dialog is open, and for 30 s after,
# each guest thread's first call of each function is logged, and each
# thread's latest call is kept; holding Select over a dialog writes those out,
# which says where each game thread is waiting.
module_parent = 'vita3k/modules/module_parent.cpp'
replace(module_parent, '''#include <unordered_set>
''', '''#include <unordered_set>

#ifdef VITA3K_UWP
#include <dialog/state.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>

namespace {
std::mutex uwp_trace_mutex;
std::set<std::pair<SceUID, uint32_t>> uwp_trace_seen;
std::map<SceUID, std::pair<uint32_t, int64_t>> uwp_trace_last;
int uwp_trace_logged = 0;
std::atomic<int64_t> uwp_trace_until{ 0 };

int64_t uwp_trace_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void uwp_dialog_trace(EmuEnvState &emuenv, uint32_t nid, SceUID thread_id) {
    const bool open = emuenv.common_dialog.type != NO_DIALOG;
    const int64_t now = uwp_trace_now_ms();
    if (open)
        uwp_trace_until.store(now + 30000, std::memory_order_relaxed);
    else if (now > uwp_trace_until.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(uwp_trace_mutex);
    uwp_trace_last[thread_id] = { nid, now };
    if (uwp_trace_logged < 400 && uwp_trace_seen.insert({ thread_id, nid }).second) {
        uwp_trace_logged++;
        LOG_INFO("Dialog trace: thread {} called {}{}", thread_id, import_name(nid), open ? "" : " (dialog closed)");
    }
}
} // namespace

// Each guest thread's latest call, with how long ago it was: holding Select
// over a stuck dialog writes this, which tells where the game waits.
void uwp_dialog_trace_dump(EmuEnvState &emuenv) {
    std::map<SceUID, std::pair<uint32_t, int64_t>> last;
    {
        std::lock_guard<std::mutex> lock(uwp_trace_mutex);
        last = uwp_trace_last;
    }
    const int64_t now = uwp_trace_now_ms();
    LOG_WARN("Dialog trace: {} guest thread(s) made calls while the dialog was open", last.size());
    for (const auto &[thread_id, call] : last) {
        std::string name;
        if (const auto thread = emuenv.kernel.get_thread(thread_id))
            name = thread->name;
        LOG_WARN("Dialog trace: thread {} ({}) last called {} {} ms ago", thread_id, name, import_name(call.first), now - call.second);
    }
}
#endif
''')
replace(module_parent, '''void call_import(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, SceUID thread_id) {
    // HLE - call our C++ function''', '''void call_import(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, SceUID thread_id) {
#ifdef VITA3K_UWP
    uwp_dialog_trace(emuenv, nid, thread_id);
#endif
    // HLE - call our C++ function''')
replace(lr, '''            if (select_frames == 90) {
                LOG_WARN("System dialog (type {}) cancelled: Select held", static_cast<int>(dialog.type));''', '''            if (select_frames == 90) {
                LOG_WARN("System dialog (type {}) cancelled: Select held", static_cast<int>(dialog.type));
                uwp_dialog_trace_dump(emuenv);''')
replace(lr, '''RETRO_API void retro_unload_game(void) {''', '''#ifdef VITA3K_UWP
void uwp_dialog_trace_dump(EmuEnvState &emuenv);
#endif

RETRO_API void retro_unload_game(void) {''')
print('System dialogs: the guest calls made while one is open are logged.')

# ── Vulkan: a pipeline that cannot be made skips its draws ──────────────────
# Background compilation was off on the Xbox because a pipeline that failed
# (Dozen returns an error for one the console refuses) threw out of the
# worker thread, leaving it "compiling" for good: the game ran behind a black
# screen. A failed pipeline is now remembered as failed and its draws skipped,
# from the workers and the render thread alike, and never destroyed as if it
# were a real one.
replace(pipeline_cache, '''        vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_gxm, *request->fragment_program_gxm, *request->get_record(), request->hints, mem);
        *request->pipeline = pipeline;''', '''#ifdef VITA3K_UWP
        vk::Pipeline pipeline;
        try {
            pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_gxm, *request->fragment_program_gxm, *request->get_record(), request->hints, mem);
        } catch (const std::exception &e) {
            static std::atomic<int> failures{ 0 };
            if (failures.fetch_add(1) < 16)
                LOG_ERROR("Pipeline could not be made, its draws are skipped: {}", e.what());
            pipeline = nullptr;
        }
        if (!pipeline)
            pipeline = std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);
#else
        vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_gxm, *request->fragment_program_gxm, *request->get_record(), request->hints, mem);
#endif
        *request->pipeline = pipeline;''')
replace(pipeline_cache, '''            if (it->second == pipeline_compiling)
                // pipeline is still compiling
                return nullptr;''', '''            if (it->second == pipeline_compiling)
                // pipeline is still compiling
                return nullptr;
#ifdef VITA3K_UWP
            else if (it->second == std::bit_cast<vk::Pipeline, uint64_t>(~1ULL))
                // it could not be made: its draws are skipped
                return nullptr;
#endif''')
replace(pipeline_cache, '''        // can't wait, compile it right now
        vk::Pipeline result = compile_pipeline(type, render_pass, vertex_program_gxm, fragment_program_gxm, record, context.shader_hints, mem);''', '''        // can't wait, compile it right now
#ifdef VITA3K_UWP
        vk::Pipeline result;
        try {
            result = compile_pipeline(type, render_pass, vertex_program_gxm, fragment_program_gxm, record, context.shader_hints, mem);
        } catch (const std::exception &e) {
            static std::atomic<int> failures{ 0 };
            if (failures.fetch_add(1) < 16)
                LOG_ERROR("Pipeline could not be made, its draws are skipped: {}", e.what());
            result = nullptr;
        }
        if (!result) {
            it->second = std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);
            return nullptr;
        }
#else
        vk::Pipeline result = compile_pipeline(type, render_pass, vertex_program_gxm, fragment_program_gxm, record, context.shader_hints, mem);
#endif''')
replace(pipeline_cache, '''    for (auto &[hash, pipeline] : pipelines)
        state.device.destroy(pipeline);
    pipelines.clear();''', '''    for (auto &[hash, pipeline] : pipelines) {
#ifdef VITA3K_UWP
        // a failed one or one still compiling is no pipeline at all
        if (!pipeline || pipeline == std::bit_cast<vk::Pipeline, uint64_t>(~1ULL) || pipeline == std::bit_cast<vk::Pipeline, uint64_t>(~0ULL))
            continue;
#endif
        state.device.destroy(pipeline);
    }
    pipelines.clear();''')
print('Vulkan: a pipeline that cannot be made skips its draws instead of stopping a compile thread.')

# ── Save dialog: the game's turn after a slot is picked ─────────────────────
# Picking a slot in a save list finishes the dialog's sub-step and leaves it
# waiting for the game to go on (sceSaveDataDialogContinue / Finish). The
# overlay drew it again as a card with no buttons and started reading the pad
# for it, and while an overlay reads the pad the game gets none of it: a game
# that asks its own question at that point (Danganronpa) never saw the answer,
# under a card that hid it, and the save screen could not be left. While the
# dialog waits on the game it is not drawn and takes no input; a progress bar
# is drawn but takes no input either. The game's next step (Continue) brings
# the dialog back.
renderer_cpp = 'vita3k/renderer/src/renderer.cpp'
replace(renderer_cpp, '''        if (common_dialog && common_dialog->type != NO_DIALOG && common_dialog->status == SCE_COMMON_DIALOG_STATUS_RUNNING) {''', '''#ifdef VITA3K_UWP
        const bool savedata_waits_on_game = common_dialog && common_dialog->type == SAVEDATA_DIALOG
            && common_dialog->status == SCE_COMMON_DIALOG_STATUS_RUNNING
            && common_dialog->substatus == SCE_COMMON_DIALOG_STATUS_FINISHED
            && !common_dialog->savedata.has_progress_bar;
        const bool savedata_no_input = common_dialog && common_dialog->type == SAVEDATA_DIALOG
            && common_dialog->savedata.btn_num == 0
            && common_dialog->savedata.mode_to_display != SCE_SAVEDATA_DIALOG_MODE_LIST
            && !common_dialog->savedata.draw_info_window;
        {
            static bool logged_wait = false;
            if (savedata_waits_on_game != logged_wait) {
                logged_wait = savedata_waits_on_game;
                LOG_INFO("Save dialog {}", savedata_waits_on_game ? "waits for the game: hidden, the controller is the game's" : "back on screen");
            }
        }
#else
        const bool savedata_waits_on_game = false, savedata_no_input = false;
#endif
        if (common_dialog && common_dialog->type != NO_DIALOG && common_dialog->status == SCE_COMMON_DIALOG_STATUS_RUNNING && !savedata_waits_on_game) {''')
replace(renderer_cpp, '''            if (dlg->poll_dialog(*common_dialog, sys_date_format, sys_button)
                && common_dialog->type != TROPHY_SETUP_DIALOG) {''', '''            if (dlg->poll_dialog(*common_dialog, sys_date_format, sys_button)
                && common_dialog->type != TROPHY_SETUP_DIALOG && !savedata_no_input) {''')
print('Save dialog: while it waits on the game it is hidden and the controller is the game\'s.')

# ── No firmware download links ──────────────────────────────────────────────
# The core's "firmware missing" log lines carried download links. This port
# neither ships firmware nor points to where it comes from.
installer = 'vita3k/libretro/libretro_installer.cpp'
replace(installer, ' (pd0/) → https://bit.ly/4hlePsX\\n', ' (pd0/)\\n')
replace(installer, ' (sa0/) → https://bit.ly/48ouDaa\\n', ' (sa0/)\\n')
print('Firmware: no download links in the core\'s messages.')

# ── An update installed before its game counts as installed ─────────────────
# An update whose game is not installed yet is put in ux0/patch and applied
# when the game starts, but the install reported failure all the same: the
# app said "could not be installed" and installed the whole PKG again the
# next time round (1.2 GB for Minecraft's).
replace(installer, '''            lr_msg("Update installed. It is applied when the game is started.", 600);
            return result;''', '''            lr_msg("Update installed. It is applied when the game is started.", 600);
            result.success = true;
            return result;''')
replace(installer, '''        lr_msg("Installed. Start the game's own PKG or VPK to play.", 600);
        return result;''', '''        lr_msg("Installed. Start the game's own PKG or VPK to play.", 600);
        result.success = true;
        return result;''')
print('PKG: an update or DLC installed before its game reports success.')

# ── Performance: where the time goes, and a faster CPU ──────────────────────
# Minecraft ran at 7 to 8 frames a second with the emulation loop idle, so the
# time goes into the game's own threads or the renderer's. Each minute the log
# now says how busy each of them was (and how often guest memory traps fired),
# so the next step is aimed rather than guessed.
(source / 'vita3k/util/include/util/uwp_profile.h').write_text('''// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox port: which host threads are busy, reported once a minute
#pragma once

#include <atomic>
#include <cstdint>

namespace uwp_profile {
// Guest memory accesses that trapped (protected pages: texture and buffer tracking)
inline std::atomic<uint64_t> memory_traps{ 0 };
// "Fast" CPU mode: exclusive accesses skip the global monitor (set from the CPU option)
inline std::atomic<bool> fast_atomics{ false };
#if defined(VITA3K_UWP) && defined(BUILD_LIBRETRO)
// Notes the calling host thread under a name; defined in libretro_vita3k.cpp
void register_thread(const char *name);
#else
inline void register_thread(const char *) {}
#endif
} // namespace uwp_profile
''', encoding='utf-8')

mem_cpp = 'vita3k/mem/src/mem.cpp'
replace(mem_cpp, '#include <util/log.h>\n', '#include <util/log.h>\n#include <util/uwp_profile.h>\n')
replace(mem_cpp, '''bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
''', '''bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
    uwp_profile::memory_traps.fetch_add(1, std::memory_order_relaxed);
''')

thread_cpp = 'vita3k/kernel/src/thread.cpp'
replace(thread_cpp, '#include <util/log.h>\n', '#include <util/log.h>\n#include <util/uwp_profile.h>\n')
replace(thread_cpp, '''    } cpu_state_guard;
    set_current_cpu_state(cpu.get());
''', '''    } cpu_state_guard;
    set_current_cpu_state(cpu.get());
    if (top_level)
        uwp_profile::register_thread(("game: " + name).c_str());
''')

replace(pipeline_cache, '''void PipelineCache::compiler_thread(MemState &mem) {
    moodycamel::ConsumerToken consumer_token(pipeline_compile_queue);
''', '''void PipelineCache::compiler_thread(MemState &mem) {
    uwp_profile::register_thread("shader compiler");
    moodycamel::ConsumerToken consumer_token(pipeline_compile_queue);
''')
replace(pipeline_cache, '#include <renderer/vulkan/pipeline_cache.h>\n', '#include <renderer/vulkan/pipeline_cache.h>\n#include <util/uwp_profile.h>\n')

replace(lr, '''        libretro.render_thread = std::make_unique<std::thread>([]() {
            EmuEnvState &emuenv = *libretro.emuenv;''', '''        libretro.render_thread = std::make_unique<std::thread>([]() {
            uwp_profile::register_thread("renderer");
            EmuEnvState &emuenv = *libretro.emuenv;''')
replace(lr, '#include <util/fs.h>\n', '''#include <util/fs.h>
#include <util/uwp_profile.h>
''', count=1)
replace(lr, '''RETRO_API void retro_run(void) {
    if (!libretro.game_loaded || !libretro.emuenv)
        return;
''', '''#ifdef VITA3K_UWP
namespace uwp_profile {
namespace {
struct ProfiledThread {
    std::string name;
    HANDLE handle;
    uint64_t last;
};
std::mutex g_profile_mutex;
std::vector<ProfiledThread> g_profiled;
std::chrono::steady_clock::time_point g_profile_since = std::chrono::steady_clock::now();
uint64_t g_profile_traps = 0;
uint64_t busy_100ns(HANDLE handle) {
    FILETIME created, exited, kernel_time, user_time;
    if (!GetThreadTimes(handle, &created, &exited, &kernel_time, &user_time))
        return 0;
    const auto value = [](const FILETIME &t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return value(kernel_time) + value(user_time);
}
} // namespace

void register_thread(const char *name) {
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
            THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0))
        return;
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    if (g_profiled.size() >= 256) {
        CloseHandle(handle);
        return;
    }
    g_profiled.push_back({ name ? name : "?", handle, busy_100ns(handle) });
}

// How busy each thread was since the last report, as a share of one core
static void report() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - g_profile_since).count();
    if (seconds < 1.0)
        return;
    g_profile_since = now;
    std::vector<std::pair<double, std::string>> busy;
    double total = 0;
    {
        std::lock_guard<std::mutex> lock(g_profile_mutex);
        std::map<std::string, double> by_name;
        for (auto it = g_profiled.begin(); it != g_profiled.end();) {
            const uint64_t t = busy_100ns(it->handle);
            const double share = t > it->last ? (t - it->last) / (seconds * 1e5) : 0.0; // percent of a core
            it->last = t;
            by_name[it->name] += share;
            total += share;
            if (WaitForSingleObject(it->handle, 0) == WAIT_OBJECT_0) {
                CloseHandle(it->handle);
                it = g_profiled.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &[name, share] : by_name)
            busy.emplace_back(share, name);
    }
    std::sort(busy.begin(), busy.end(), std::greater<>());
    std::string line;
    for (const auto &[share, name] : busy) {
        if (share < 2.0 || line.size() > 400)
            break;
        line += fmt::format("{}{} {:.0f}%", line.empty() ? "" : ", ", name, share);
    }
    const uint64_t traps = memory_traps.load(std::memory_order_relaxed);
    LOG_INFO("Thread load (100% = one core busy all the time): {} (all {:.0f}%); guest memory traps {:.0f}/s",
        line.empty() ? "nothing busy" : line, total, (traps - g_profile_traps) / seconds);
    g_profile_traps = traps;
}
} // namespace uwp_profile
#endif

RETRO_API void retro_run(void) {
    if (!libretro.game_loaded || !libretro.emuenv)
        return;
#ifdef VITA3K_UWP
    {
        static bool registered = false;
        if (!registered) {
            registered = true;
            uwp_profile::register_thread("emulation loop");
        }
        static uint64_t profile_ticks = 0;
        if (++profile_ticks % 3600 == 0)
            uwp_profile::report();
    }
#endif
''')
print('Profile: each minute, how busy the game, renderer and compiler threads were.')

# The CPU itself. Atomics (the LDREX/STREX pairs behind every lock and
# reference count) went through a C++ callback each; with fastmem they run as
# host instructions, as in other Dynarmic emulators. NEON no longer switches
# the host's floating-point mode around every instruction, and NaN results
# are not rewritten to the ARM default one by one: the two "unsafe" settings
# other emulators ship on by default, at no cost to games seen so far.
dyn = 'vita3k/cpu/src/dynarmic_cpu.cpp'
replace(dyn, '#include <util/log.h>\n', '#include <util/log.h>\n#include <util/uwp_profile.h>\n')
replace(dyn, '''    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;''', '''    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
#ifdef VITA3K_UWP
    if (cpu_opt) {
        config.unsafe_optimizations = true;
        // The three unsafe floating-point settings (ignore the standard FPCR,
        // inaccurate NaN, reduced-error estimates) are off again: speckled
        // textures in Uncharted and Minecraft, and the same artifacts upstream
        // reports in Uncharted that vanish with CPU optimisation off, pointed
        // at them.
        // No Vita game runs big-endian; skip the check on every memory access.
        config.always_little_endian = true;
        // Hint instructions (PLD preloads, YIELD, SEV, WFE) each ended the JIT
        // block and called back into the emulator, which did nothing for them.
        // Vita libc's memcpy and every spin lock are full of them; as plain
        // no-ops they cost nothing. (WFI becomes a no-op too, so a game idling
        // on it spins instead of sleeping.)
        config.hook_hint_instructions = false;
        // Fast mode (opt-in): LDREX/STREX no longer consult the shared monitor.
        // Quicker locks and reference counts; a game that relies on exact
        // exclusive-access behaviour can hang or misbehave.
        // On for every game from build 94: the biggest CPU gain left for
        // games with several busy threads (Minecraft's server, chunk and
        // render threads all take locks). If a game hangs or misbehaves,
        // the CPU setting "fast" is no longer needed to turn this off: tell me.
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        (void)uwp_profile::fast_atomics;
        if (config.fastmem_pointer) {
            config.fastmem_exclusive_access = true;
            config.recompile_on_exclusive_fastmem_failure = true;
        }
    }
#endif''')
print('CPU: exclusive accesses through fastmem, and the two usual unsafe FP settings.')

# Uncharted: Golden Abyss ships a launcher. Its eboot.bin is a few thousand
# bytes that start the game proper (uncharted.self) with sceAppMgrLoadExec.
# The desktop app answers that call by restarting the title with the new
# executable; this core never reads the request (nothing here takes the
# launch request), so the launcher sat in a loop on its last splash frame on
# both renderers. A launcher is told by its size, and the known target is
# loaded in its place; if that fails the launcher loads as before.
loader = 'vita3k/libretro/libretro_game_loader.cpp'
replace(loader, """    main_module_id = load_module(emuenv, "app0:" + emuenv.self_path);
    if (main_module_id >= 0) {""", """    main_module_id = -1;
#ifdef VITA3K_UWP
    if (emuenv.cfg.self_path.empty()) {
        // Launchers known to hand over to another executable with sceAppMgrLoadExec.
        static constexpr const char *launcher_targets[] = { "uncharted.self" };
        vfs::FileBuffer eboot;
        if (vfs::read_app_file(eboot, emuenv.vita_fs_path, emuenv.io.app_path, EBOOT_PATH) && eboot.size() < 0x10000) {
            for (const char *target : launcher_targets) {
                LOG_INFO("eboot.bin is a launcher ({} bytes); trying {} directly", eboot.size(), target);
                main_module_id = load_module(emuenv, std::string("app0:") + target);
                if (main_module_id >= 0) {
                    emuenv.self_path = target;
                    LOG_INFO("Started {} in place of the launcher", target);
                    break;
                }
                LOG_WARN("{} did not load; using the launcher", target);
            }
        }
    }
    if (main_module_id < 0)
#endif
    main_module_id = load_module(emuenv, "app0:" + emuenv.self_path);
    if (main_module_id >= 0) {""")
print('Launchers: Uncharted starts uncharted.self directly instead of idling in its launcher.')

# Every import call (millions a second in a busy game) read the clock to see
# whether a dialog had been open lately. Now the clock is only read while a
# dialog is open or the 30-second window after one is still running.
replace(module_parent, '''std::atomic<int64_t> uwp_trace_until{ 0 };
''', '''std::atomic<int64_t> uwp_trace_until{ 0 };
std::atomic<bool> uwp_trace_armed{ false };
''')
replace(module_parent, '''    const bool open = emuenv.common_dialog.type != NO_DIALOG;
    const int64_t now = uwp_trace_now_ms();
    if (open)
        uwp_trace_until.store(now + 30000, std::memory_order_relaxed);
    else if (now > uwp_trace_until.load(std::memory_order_relaxed))
        return;''', '''    const bool open = emuenv.common_dialog.type != NO_DIALOG;
    if (!open && !uwp_trace_armed.load(std::memory_order_relaxed))
        return;
    const int64_t now = uwp_trace_now_ms();
    if (open) {
        uwp_trace_until.store(now + 30000, std::memory_order_relaxed);
        uwp_trace_armed.store(true, std::memory_order_relaxed);
    } else if (now > uwp_trace_until.load(std::memory_order_relaxed)) {
        uwp_trace_armed.store(false, std::memory_order_relaxed);
        return;
    }''')
print('Speed: imports no longer read the clock unless a dialog was open lately; ARM reciprocal estimates and endianness checks are cheaper.')

# The CPU option gets a third value, "fast", for the atomics shortcut above.
replace('vita3k/libretro/libretro_vita3k.cpp', '''    read_core_option("vita3k_cpu_opt", "enabled", [](const char *value) {
        libretro.cfg->current_config.cpu_opt = parse_enabled_option(value, true);
    });''', '''    read_core_option("vita3k_cpu_opt", "enabled", [](const char *value) {
#ifdef VITA3K_UWP
        const bool fast = normalize_option_token(value) == "fast";
        uwp_profile::fast_atomics.store(fast, std::memory_order_relaxed);
        libretro.cfg->current_config.cpu_opt = fast || parse_enabled_option(value, true);
        if (fast)
            lr_log(RETRO_LOG_INFO, "CPU: fast mode (exclusive accesses skip the global monitor)\\n");
#else
        libretro.cfg->current_config.cpu_opt = parse_enabled_option(value, true);
#endif
    });''')
print('CPU option: a Fast mode that skips the exclusive monitor.')

# New shaders compile on background threads, and a draw is skipped until its
# pipeline is ready: Minecraft's trees and chunks appeared half-drawn and
# filled in late. The console has eight logical cores and the compiler had
# three; a fourth clears a burst of new shaders sooner.
replace('vita3k/renderer/src/vulkan/pipeline_cache.cpp', '''    else if (nb_logical_threads >= 8)
        nb_worker_threads = 3;''', '''    else if (nb_logical_threads >= 8)
#ifdef VITA3K_UWP
        nb_worker_threads = 4;
#else
        nb_worker_threads = 3;
#endif''')
print('Shaders: four background compiler threads on the console.')

# A new shader wrote several debug files before it ran: the guest program
# (.gxp), its disassembly and the SPIR-V text, each its own file on the
# console's disk, on the thread that is waiting for the shader. Vita3KiOS
# dropped them for the same reason (new scenes stuttered). Only what the
# renderer reads back stays: the GLSL text OpenGL caches under the shader's
# own extension, and the .spv Vulkan caches below.
replace('vita3k/renderer/src/shaders.cpp', '''    fs::create_directories(shaderlog_path);

    auto shader_log_path = get_shaderlog_path("gxp");

    // Dump gxp binary
    fs_utils::dump_data(shader_log_path, &program, program.size);
    const auto write_data_with_ext = [&](const std::string &ext, const std::string &data) {
        fs::path out_path;''', '''#ifndef VITA3K_UWP
    fs::create_directories(shaderlog_path);
#endif

    auto shader_log_path = get_shaderlog_path("gxp");

#ifndef VITA3K_UWP
    // Dump gxp binary
    fs_utils::dump_data(shader_log_path, &program, program.size);
#endif
    const auto write_data_with_ext = [&](const std::string &ext, const std::string &data) {
#ifdef VITA3K_UWP
        // debug dumps (disassembly, SPIR-V text) are skipped; GLSL is the OpenGL cache
        if (target != shader::Target::GLSLOpenGL || ext != shader_type_str)
            return true;
#endif
        fs::path out_path;''')
print('Shaders: debug dumps are not written on the console (new scenes stutter less).')

# ── Vulkan surface sync without memory mapping ──────────────────────────────
# Surface sync (what the GPU drew, copied back where the game reads it) only
# ran when the renderer could map guest memory for the GPU, which needs buffer
# device address: Dozen has none, so on the console it never ran, whatever
# the setting said (the log said "surface sync: not running"). Games that read
# their own pictures (Gravity Rush's bloom, Danganronpa's examine screens, the
# reason the iOS build turns it on for them) got stale memory. Without
# mapping, the picture now goes through a host buffer the CPU can read: the
# copy is waited for at once and written into guest memory. Surfaces are still
# only synced once the game touches their memory (the existing page traps), so
# games that never read them pay nothing.
surface_cache_cpp = 'vita3k/renderer/src/vulkan/surface_cache.cpp'
replace(surface_cache_cpp, '''        buffer = vkutil::Buffer(required_size);
        buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);''', '''        buffer = vkutil::Buffer(required_size);
#ifdef VITA3K_UWP
        // read by the CPU after the GPU wrote it: cached memory, not write-combined
        static constexpr vma::AllocationCreateInfo readback_alloc = {
            .flags = vma::AllocationCreateFlagBits::eHostAccessRandom | vma::AllocationCreateFlagBits::eMapped,
            .usage = vma::MemoryUsage::eAuto,
            .preferredFlags = vk::MemoryPropertyFlagBits::eHostCached
        };
        buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst, readback_alloc);
#else
        buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
#endif''')

replace(surface_cache_cpp, '''bool VKSurfaceCache::check_for_surface(MemState &mem, Address source_address, CallbackRequestFunction &callback, Address target_address) {
    if (!state.features.enable_memory_mapping || state.disable_surface_sync)
        return false;
''', '''bool VKSurfaceCache::check_for_surface(MemState &mem, Address source_address, CallbackRequestFunction &callback, Address target_address) {
    if (state.disable_surface_sync)
        return false;
#ifdef VITA3K_UWP
    if (!state.features.enable_memory_mapping) {
        // No wait thread without mapping: copy the picture, wait for it, write
        // it into guest memory, then let the caller run the transfer itself.
        auto found = color_address_lookup.find(source_address);
        if (found == color_address_lookup.end())
            return false;
        ColorSurfaceCacheInfo &surface = *found->second;
        VKContext &context = *static_cast<VKContext *>(state.context);
        if (surface.last_frame_rendered + MAX_FRAMES_RENDERING <= context.frame_timestamp)
            return false;
        if (!*surface.need_surface_sync) {
            *surface.need_surface_sync = true;
            vk::CommandBuffer prev_cmd = context.render_cmd;
            vk::CommandBuffer surface_cmd = nullptr;
            vk::Fence fence = state.device.createFence({});
            ColorSurfaceCacheInfo *returned_info = nullptr;
            {
                std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
                surface_cmd = vkutil::create_single_time_command(state.device, state.multithread_command_pool);
                context.render_cmd = surface_cmd;
                last_written_surface = &surface;
                returned_info = perform_surface_sync();
                context.render_cmd = prev_cmd;
                surface_cmd.end();
            }
            vk::SubmitInfo submit_info{};
            submit_info.setCommandBuffers(surface_cmd);
            state.locked_queue_submit(state.general_queue, submit_info, fence);
            if (state.device.waitForFences(fence, vk::True, 2'000'000'000ULL) == vk::Result::eSuccess) {
                if (returned_info)
                    perform_post_surface_sync(mem, returned_info);
            } else {
                LOG_ERROR("Surface sync: the GPU did not finish the copy within 2 s");
            }
            state.device.destroyFence(fence);
            std::lock_guard<std::mutex> lock(state.multithread_pool_mutex);
            state.device.freeCommandBuffers(state.multithread_command_pool, surface_cmd);
        }
        return false;
    }
#else
    if (!state.features.enable_memory_mapping)
        return false;
#endif
''')

replace(surface_cache_cpp, '''ColorSurfaceCacheInfo *VKSurfaceCache::perform_surface_sync() {
    // surface sync is supported only if memory mapping is enabled
    if (!state.features.enable_memory_mapping)
        return nullptr;
''', '''ColorSurfaceCacheInfo *VKSurfaceCache::perform_surface_sync() {
#ifndef VITA3K_UWP
    // surface sync is supported only if memory mapping is enabled
    if (!state.features.enable_memory_mapping)
        return nullptr;
#endif
''')

replace(surface_cache_cpp, '''    const bool needs_copy_buffer = format_need_additional_memory(last_written_surface->format)
        || surface_sync_needs_u4u4u4u4_repack(*last_written_surface);''', '''    bool needs_copy_buffer = format_need_additional_memory(last_written_surface->format)
        || surface_sync_needs_u4u4u4u4_repack(*last_written_surface);
#ifdef VITA3K_UWP
    // without mapping there is no GPU view of guest memory: always a host buffer
    if (!state.features.enable_memory_mapping)
        needs_copy_buffer = true;
#endif''')

replace(surface_cache_cpp, '''    uint8_t *pixels = surface->data.cast<uint8_t>().get(mem);
''', '''    uint8_t *pixels = surface->data.cast<uint8_t>().get(mem);

#ifdef VITA3K_UWP
    if (!state.features.enable_memory_mapping && surface->copy_buffer && surface->copy_buffer->mapped_data
        && !surface_sync_needs_u4u4u4u4_repack(*surface) && !format_need_additional_memory(surface->format)) {
        // the picture came through a host buffer: put it where the game reads it
        const size_t bytes = std::min<size_t>(static_cast<size_t>(pixel_stride) * surface->original_height
                * vk::blockSize(surface->texture.format),
            static_cast<size_t>(surface->total_bytes));
        memcpy(pixels, surface->copy_buffer->mapped_data, bytes);
    }
#endif
''')

context_cpp = 'vita3k/renderer/src/vulkan/context.cpp'
replace(context_cpp, '''    if (state.features.enable_memory_mapping && !state.disable_surface_sync && submit)
        surface_info = state.surface_cache.perform_surface_sync();''', '''#ifdef VITA3K_UWP
    if (!state.disable_surface_sync && submit)
#else
    if (state.features.enable_memory_mapping && !state.disable_surface_sync && submit)
#endif
        surface_info = state.surface_cache.perform_surface_sync();''')
replace(context_cpp, '''            state.request_queue.push(request);
        }
    }
    visibility_sync_ranges.clear();''', '''            state.request_queue.push(request);
        }
    }
#ifdef VITA3K_UWP
    else if (surface_info && surface_info->need_post_surface_sync && state.mem && fence) {
        // no wait thread without mapping: wait for this scene, then copy its
        // picture into guest memory before the game reads it
        if (state.device.waitForFences(fence, vk::True, 2'000'000'000ULL) == vk::Result::eSuccess)
            state.surface_cache.perform_post_surface_sync(*state.mem, surface_info);
        else
            LOG_ERROR("Surface sync: the GPU did not finish the scene within 2 s");
    }
#endif
    visibility_sync_ranges.clear();''')

replace('vita3k/renderer/src/transfer.cpp', '''    if (renderer.current_backend == Backend::Vulkan && renderer.features.enable_memory_mapping && !renderer.disable_surface_sync) {''', '''    if (renderer.current_backend == Backend::Vulkan
#ifndef VITA3K_UWP
        && renderer.features.enable_memory_mapping
#endif
        && !renderer.disable_surface_sync) {''', count=2)

replace('vita3k/libretro/libretro_vita3k.cpp', '''            (mapping && !libretro.emuenv->renderer->disable_surface_sync) ? "working" : "not running");''', '''            !libretro.emuenv->renderer->disable_surface_sync ? (mapping ? "working" : "working (read back through a host buffer)") : "off");''')
print('Vulkan: surface sync works without memory mapping (read back through a host buffer).')

# ── New settings: texture cache, hashless texture cache, shader cache and
# compiler threads; FSR gone ────────────────────────────────────────────────
# FSR never worked on the console (the frame it writes is a storage image the
# D3D12 swap path does not take); the menu no longer offers it and a saved FSR
# runs as Bilinear. The compiler thread count was fixed at four; it is now a
# setting (default four).
pipeline_cache_cpp = 'vita3k/renderer/src/vulkan/pipeline_cache.cpp'
replace(pipeline_cache_cpp, '''namespace renderer::vulkan {''', '''#ifdef VITA3K_UWP
// Shader compiler threads, from the "Shader Compiler Threads" setting.
int vita3k_uwp_shader_threads = 4;
#endif

namespace renderer::vulkan {''')
replace(pipeline_cache_cpp, '''    if (use_async_compilation) {
        // we could not initialize the worker threads previously''', '''#ifdef VITA3K_UWP
    nb_worker_threads = vita3k_uwp_shader_threads < 1 ? 1 : (vita3k_uwp_shader_threads > 8 ? 8 : vita3k_uwp_shader_threads);
    LOG_INFO("Shader compiler threads: {}", nb_worker_threads);
#endif
    if (use_async_compilation) {
        // we could not initialize the worker threads previously''')

libretro_cpp = 'vita3k/libretro/libretro_vita3k.cpp'
replace(libretro_cpp, '''static std::string canonical_memory_mapping_option(const char *value) {''', '''#ifdef VITA3K_UWP
extern int vita3k_uwp_shader_threads;
#endif

static std::string canonical_memory_mapping_option(const char *value) {''')
replace(libretro_cpp, '''        libretro.cfg->current_config.screen_filter = value;''', '''#ifdef VITA3K_UWP
        // FSR is not offered on the console any more; a saved FSR runs as Bilinear
        libretro.cfg->current_config.screen_filter = std::string(value) == "FSR" ? std::string("Bilinear") : std::string(value);
#else
        libretro.cfg->current_config.screen_filter = value;
#endif''')
replace(libretro_cpp, '''        libretro.cfg->current_config.disable_surface_sync = !parse_enabled_option(value, true);
    });''', '''        libretro.cfg->current_config.disable_surface_sync = !parse_enabled_option(value, true);
    });
#ifdef VITA3K_UWP
    read_core_option("vita3k_texture_cache", "enabled", [](const char *value) {
        libretro.cfg->current_config.texture_cache = parse_enabled_option(value, true);
    });
    read_core_option("vita3k_hashless_texture_cache", "disabled", [](const char *value) {
        libretro.cfg->hashless_texture_cache = parse_enabled_option(value, false);
    });
    read_core_option("vita3k_shader_cache", "enabled", [](const char *value) {
        libretro.cfg->current_config.shader_cache = parse_enabled_option(value, true);
    });
    read_core_option("vita3k_shader_threads", "4", [](const char *value) {
        const int threads = static_cast<int>(std::strtol(value, nullptr, 10));
        vita3k_uwp_shader_threads = (threads >= 1 && threads <= 8) ? threads : 4;
    });
#endif''')
print('Settings: texture cache, fast texture cache, shader cache, compiler threads; FSR runs as Bilinear.')

# ── Pipeline cache saved off the render thread ──────────────────────────────
# 15 s after the last new shader, the render thread wrote the shader list,
# asked the driver for its whole cache and wrote it to disk: a hitch in the
# middle of play, again after every burst of new effects. The iOS build moved
# it to a thread of its own; here too. The render thread only copies the two
# lists; the driver call and the file run beside the game. The save at exit
# still finishes before the renderer goes away.
pipeline_cache_h = 'vita3k/renderer/include/renderer/vulkan/pipeline_cache.h'
replace(pipeline_cache_h, '''    uint64_t next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();''', '''    uint64_t next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();
#ifdef VITA3K_UWP
    // the periodic save runs here, beside the game (joined before the next
    // save and at cleanup)
    std::jthread uwp_save_thread;
    bool uwp_save_in_background = false;
#endif''')
replace(pipeline_cache_cpp, '''void PipelineCache::save_pipeline_cache() {
    // first save the shader hashes''', '''void PipelineCache::save_pipeline_cache() {
#ifdef VITA3K_UWP
    if (uwp_save_thread.joinable())
        uwp_save_thread.join();
    if (uwp_save_in_background) {
        std::vector<ShadersHash> shaders_copy;
        {
            std::lock_guard<std::mutex> guard(shaders_mutex);
            shaders_copy = state.shaders_cache_hashs;
        }
        std::vector<uint64_t> hashes;
        hashes.reserve(pipelines.size());
        for (auto &[hash, _] : pipelines)
            hashes.push_back(hash);
        uwp_save_thread = std::jthread([this, shaders_copy = std::move(shaders_copy), hashes = std::move(hashes)]() mutable {
            renderer::save_shaders_cache_hashs(state, shaders_copy);
            const std::vector<uint8_t> pipeline_data = state.device.getPipelineCacheData(pipeline_cache);
            if (pipeline_data.empty())
                return;
            const fs::path path = state.shaders_path / fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
            fs::path temp_path = path;
            temp_path += ".tmp";
            std::ofstream file(temp_path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
                return;
            const auto write_integer = [&]<typename T>(T val) {
                file.write(reinterpret_cast<const char *>(&val), sizeof(T));
            };
            write_integer(pipeline_cache_magic);
            write_integer(hashes.size());
            for (const uint64_t hash : hashes)
                write_integer(hash);
            file.write(reinterpret_cast<const char *>(pipeline_data.data()), pipeline_data.size());
            file.close();
            // a half-written cache (the app closed mid-save) must not replace a good one
            std::error_code rename_error;
            fs::rename(temp_path, path, rename_error);
            if (rename_error)
                return;
            LOG_INFO("Pipeline cache saved in the background ({} pipelines, {} KiB)", hashes.size(), pipeline_data.size() / 1024);
        });
        return;
    }
#endif
    // first save the shader hashes''')
replace(pipeline_cache_cpp, '''void PipelineCache::cleanup() {''', '''void PipelineCache::cleanup() {
#ifdef VITA3K_UWP
    if (uwp_save_thread.joinable())
        uwp_save_thread.join();
#endif''')
replace('vita3k/renderer/src/vulkan/renderer.cpp', '''    if (time_s >= pipeline_cache.next_pipeline_cache_save) {
        pipeline_cache.save_pipeline_cache();
''', '''    if (time_s >= pipeline_cache.next_pipeline_cache_save) {
#ifdef VITA3K_UWP
        pipeline_cache.uwp_save_in_background = true;
        pipeline_cache.save_pipeline_cache();
        pipeline_cache.uwp_save_in_background = false;
#else
        pipeline_cache.save_pipeline_cache();
#endif
''')
print('Vulkan: the periodic pipeline cache save runs beside the game.')

# ── Texture cache: 2048 entries (iOS build's value) ─────────────────────────
# A full cache of 1024 textures evicts and re-decodes in texture-heavy scenes.
# The console has the memory for twice as many.
replace('vita3k/renderer/include/renderer/texture_cache.h', '''static constexpr size_t TextureCacheSize = 1024;''', '''#ifdef VITA3K_UWP
static constexpr size_t TextureCacheSize = 2048;
#else
static constexpr size_t TextureCacheSize = 1024;
#endif''')
print('Texture cache holds 2048 textures.')

# ── Pipeline cache: written whole or not at all, read without crashing ──────
# (Both from the iOS build's lessons.) A cache cut short by the app closing
# mid-save made createPipelineCache throw at the next boot, and the game never
# started again until the file was deleted by hand. The exit save now writes
# a temporary file and renames it; a cache the driver refuses is replaced by
# an empty one.
replace(pipeline_cache_cpp, '''    std::ofstream pipeline_cache_file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Saving pipeline cache...");''', '''#ifdef VITA3K_UWP
    fs::path temp_path = path;
    temp_path += ".tmp";
    std::ofstream pipeline_cache_file(temp_path, std::ios::out | std::ios::binary | std::ios::trunc);
#else
    std::ofstream pipeline_cache_file(path, std::ios::out | std::ios::binary | std::ios::trunc);
#endif
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Saving pipeline cache...");''')
replace(pipeline_cache_cpp, '''    pipeline_cache_file.write(reinterpret_cast<const char *>(pipeline_data.data()), pipeline_data.size());
    pipeline_cache_file.close();
    LOG_INFO("Pipeline cache saved");''', '''    pipeline_cache_file.write(reinterpret_cast<const char *>(pipeline_data.data()), pipeline_data.size());
    pipeline_cache_file.close();
#ifdef VITA3K_UWP
    {
        std::error_code rename_error;
        fs::rename(temp_path, path, rename_error);
        if (rename_error) {
            LOG_WARN("Pipeline cache: could not replace the old file ({})", rename_error.message());
            return;
        }
    }
#endif
    LOG_INFO("Pipeline cache saved");''')
replace(pipeline_cache_cpp, '''    state.device.destroyPipelineCache(pipeline_cache);
    pipeline_cache = state.device.createPipelineCache(cache_info);
    LOG_INFO("Pipeline cache read and loaded");''', '''    state.device.destroyPipelineCache(pipeline_cache);
#ifdef VITA3K_UWP
    try {
        pipeline_cache = state.device.createPipelineCache(cache_info);
    } catch (const std::exception &e) {
        LOG_WARN("Pipeline cache refused by the driver ({}); starting an empty one", e.what());
        pipeline_cache = state.device.createPipelineCache(vk::PipelineCacheCreateInfo{});
        return;
    }
#else
    pipeline_cache = state.device.createPipelineCache(cache_info);
#endif
    LOG_INFO("Pipeline cache read and loaded");''')
print('Pipeline cache: atomic saves, a refused cache starts empty.')

# ── Frame fence waits in one-second steps ───────────────────────────────────
# Without memory mapping (the console) the render thread waited for the GPU
# forever at each new frame; a hung GPU left Quit stuck there. It now waits a
# second at a time and gives up when the game is closing.
replace('vita3k/renderer/src/vulkan/context.cpp', '''            auto result = device.waitForFences(frame.rendered_fences, VK_TRUE, std::numeric_limits<uint64_t>::max());
            if (result != vk::Result::eSuccess) {''', '''#ifdef VITA3K_UWP
            auto result = device.waitForFences(frame.rendered_fences, VK_TRUE, 1'000'000'000ULL);
            while (result == vk::Result::eTimeout && !context.state.render_abort.load(std::memory_order_relaxed))
                result = device.waitForFences(frame.rendered_fences, VK_TRUE, 1'000'000'000ULL);
#else
            auto result = device.waitForFences(frame.rendered_fences, VK_TRUE, std::numeric_limits<uint64_t>::max());
#endif
            if (result != vk::Result::eSuccess) {''')
print('Vulkan: frame fence waits give up when the game closes.')

# ── Fios overlays resolved for games that use the kernel call ───────────────
# Games with their own libfios2 (Danganronpa, Gravity Rush, Minecraft) resolve
# paths through _sceFiosKernelOverlayResolveWithRangeSync, which only copied
# the path back. The overlays they registered were never applied, so
# Danganronpa looked for "/Dr1/data/us/flash/fla_*.pak" with no device and
# found nothing (49 times in one log; those are the flash animations of its
# examine and talk screens). It now resolves through the same overlay table
# as the user-mode call; a rooted path no overlay claims is taken as the
# game's own (app0:), where those files are.
fios_kernel = 'vita3k/modules/SceFios2Kernel/SceFios2Kernel.cpp'
replace(fios_kernel, '''#include <module/module.h>''', '''#include <module/module.h>

#ifdef VITA3K_UWP
#include <io/functions.h>
#endif''')
replace(fios_kernel, '''    STUBBED("Using strncpy");
    strncpy(opt->pOutPath.get(emuenv.mem), pInPath, opt->maxPath);

    return 0;''', '''#ifdef VITA3K_UWP
    uint32_t lo = static_cast<uint8_t>(opt->loOrderFilter);
    uint32_t hi = static_cast<uint8_t>(opt->hiOrderFilter);
    if (hi == 0 || hi < lo) {
        lo = 0;
        hi = 0xFF;
    }
    std::string resolved = resolve_path(emuenv.io, pInPath, lo, hi);
    if (resolved == pInPath && resolved.size() > 1 && resolved[0] == '/' && resolved.find(':') == std::string::npos) {
        LOG_INFO_ONCE("Fios: rooted path with no overlay, read from the game's folder ({})", resolved);
        resolved = "app0:" + resolved;
    }
    strncpy(opt->pOutPath.get(emuenv.mem), resolved.c_str(), opt->maxPath);
    return 0;
#else
    STUBBED("Using strncpy");
    strncpy(opt->pOutPath.get(emuenv.mem), pInPath, opt->maxPath);

    return 0;
#endif''')
print('Fios: the kernel overlay resolve applies overlays (Danganronpa examine screens).')
