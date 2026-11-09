// SPDX-License-Identifier: GPL-2.0-or-later
#include <vector>
#include <util/fs.h>
#include <chrono>
#include <cassert>
int main() {
    const fs::path root=fs::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    auto leaf=root/"new"/"";
    assert(fs::without_trailing_separator(leaf).parent_path()==root);
    assert(fs::without_trailing_separator(root.root_path())==root.root_path());
    assert((root/".").filename()==fs::path("."));
    assert((root/"..").filename()==fs::path(".."));
    std::error_code error;
    assert(!fs::remove(root/"absent",error) && !error);
    assert(fs::create_directory(leaf));
    assert(fs::remove(fs::without_trailing_separator(leaf),error) && !error);
    fs::remove_all(root);
}
