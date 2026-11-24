// Checks src/startup_stubs/storage_root.h: the storage root handed to the executable always ends in
// one separator, and a value that does not fit is refused instead of cut off.
//   --mutate separator   the rule without the appended separator; must fail (negative control)
#include "startup_stubs/storage_root.h"
#include "startup_stubs/storage_root_utf8.h"

#include <cstdio>
#include <string>

namespace {
int failures;
bool mutate;

bool root(const char* value, char* out)
{
    if (!mutate) return x2w_startup::storage_root(value, out);
    // The behaviour before: the value as it is.
    if (!value || !*value || std::strlen(value) >= x2w_startup::storage_root_capacity) return false;
    std::strcpy(out, value);
    return true;
}

void expect(const char* name, const char* value, const char* wanted)
{
    char out[x2w_startup::storage_root_capacity + 8];
    std::memset(out, '#', sizeof(out));
    out[sizeof(out) - 1] = '\0';
    const bool ok = root(value, out);
    const bool match = wanted ? ok && std::strcmp(out, wanted) == 0 : !ok && out[0] == '#';
    // Nothing may be written past the capacity.
    const bool bounded = out[x2w_startup::storage_root_capacity] == '#';
    if (!match || !bounded) {
        std::printf("FAIL %s: %s \"%.80s\", expected %s%s\n", name, ok ? "gave" : "refused, buffer", ok ? out : "untouched or not",
                    wanted ? wanted : "a refusal with the buffer untouched", bounded ? "" : " (wrote past the capacity)");
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    auto wide = [](const wchar_t* value, const char* expected) {
        char out[x2w_startup::storage_root_capacity + 1];
        std::memset(out, '#', sizeof(out));
        const bool ok = x2w_startup::storage_root_utf8(value, out);
        if ((expected ? (!ok || std::strcmp(out, expected)) : (ok || out[0] != '#')) ||
            out[x2w_startup::storage_root_capacity] != '#') {
            std::printf("FAIL UTF-16 to UTF-8 root conversion or capacity guard\n");
            ++failures;
        }
    };
    wide(L"C:\\saves", "C:\\saves\\");
    wide(L"C:\\caf\u00e9\\\u6e38\u620f\\\U0001f3ae", "C:\\caf\xc3\xa9\\\xe6\xb8\xb8\xe6\x88\x8f\\\xf0\x9f\x8e\xae\\");
    wide(nullptr, nullptr);
    wide(L"", nullptr);
    wide(L"\xd800", nullptr);
    std::string utf8_limit;
    for (int i = 0; i < 129; ++i) utf8_limit += "\xc3\xa9";
    wide(std::wstring(129, L'\u00e9').c_str(), (utf8_limit + "\\").c_str());
    wide(std::wstring(130, L'\u00e9').c_str(), nullptr);
    mutate = argc == 3 && !std::strcmp(argv[1], "--mutate") && !std::strcmp(argv[2], "separator");
    if (argc != 1 && !mutate) return 2;
    // The executable appends "settings\settings.xml" to the root as it is.
    expect("drive root", "S:\\", "S:\\");
    expect("folder without a separator", "D:\\saves", "D:\\saves\\");
    expect("folder with a separator", "D:\\saves\\", "D:\\saves\\");
    expect("folder with a forward slash", "D:/saves/", "D:/saves/");
    expect("relative folder", ".", ".\\");
    expect("bare drive", "S:", "S:\\");
    expect("empty value", "", nullptr);
    expect("no value", nullptr, nullptr);
    // 64 characters was the old buffer: such a value now fits.
    const std::string sixty_four = "D:\\" + std::string(60, 'a') + "\\";
    expect("64 characters", sixty_four.c_str(), sixty_four.c_str());
    // The longest values that fit, with and without their own separator, and the first that do not.
    const std::size_t capacity = x2w_startup::storage_root_capacity;
    const std::string longest_plain(capacity - 2, 'b'), longest_separated = std::string(capacity - 2, 'b') + "\\";
    expect("longest value without a separator", longest_plain.c_str(), longest_separated.c_str());
    expect("longest value with a separator", longest_separated.c_str(), longest_separated.c_str());
    expect("one character too long without a separator", std::string(capacity - 1, 'b').c_str(), nullptr);
    expect("one character too long with a separator", (std::string(capacity - 1, 'b') + "\\").c_str(), nullptr);
    expect("far too long", std::string(4 * capacity, 'b').c_str(), nullptr);
    if (failures) {
        std::printf("startup_storage_root: %d FAILED\n", failures);
        return 1;
    }
    std::printf("startup_storage_root: PASS (9 roots, 5 refusals, 7 UTF-8 cases)\n");
    return 0;
}
