// Exercise the actual patched util/fs.h against the core's pinned fmt headers.
#include <vector>
#include <util/fs.h>
#include <fmt/std.h>
#include <cassert>
int main() {
    const fs::path path = fs::path("ux0") / "app" / "UNDERTALE";
    assert(fmt::format("{:g}", path) == "ux0/app/UNDERTALE");
    assert(!fmt::format("{}", path).empty());
    assert(fmt::format("{:g}", fs::path{}).empty());
    assert(fmt::format("{:g}", fs::path("a b")) == "a b");
}
