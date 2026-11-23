// 靜態檢查：以文字搜尋原始碼，確認沒有任何地方能繞過 guard 直接寫 UART2。
// 掃描範圍：phase2-modbus/lib 與 phase2-modbus/src（測試目錄不掃，因為本檔本身要描述被禁字串）。
// `pio test` 的工作目錄是專案根目錄（phase2-modbus/）。
#include <unity.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
// 唯一允許出現 Serial2 寫入呼叫的檔名（真實 transport，尚未建立）。
const char* const kAllowedFile = "transport_serial2.cpp";

bool hasSerial2Write(const std::string& text) {
    static const std::regex re(R"(Serial2\s*\.\s*(write|print|println|printf|availableForWrite)\b)");
    return std::regex_search(text, re);
}

bool isSource(const std::string& n) {
    auto ends = [&](const char* e) {
        std::string s(e);
        return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
    };
    return ends(".h") || ends(".hpp") || ends(".cpp") || ends(".c") || ends(".ino");
}

void collect(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        DIR* sub = opendir(path.c_str());
        if (sub) {
            closedir(sub);
            collect(path, out);
        } else if (isSource(name)) {
            out.push_back(path);
        }
    }
    closedir(d);
}

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string baseName(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
}  // namespace

// 先證明檢查器本身有效，避免「掃不到東西所以全部通過」。
static void test_detector_catches_forbidden_calls() {
    TEST_ASSERT_TRUE(hasSerial2Write("Serial2.write(buf, 8);"));
    TEST_ASSERT_TRUE(hasSerial2Write("Serial2 . print(x)"));
    TEST_ASSERT_TRUE(hasSerial2Write("Serial2.println(\"x\")"));
    TEST_ASSERT_TRUE(hasSerial2Write("Serial2\n  .write(1)"));
    TEST_ASSERT_FALSE(hasSerial2Write("Serial2.begin(115200)"));
    TEST_ASSERT_FALSE(hasSerial2Write("Serial.println(\"hi\")"));
}

static void test_scanner_finds_the_library_sources() {
    std::vector<std::string> files;
    collect("lib", files);
    collect("src", files);
    TEST_ASSERT_TRUE_MESSAGE(files.size() >= 10, "掃描不到原始碼，工作目錄可能不對");
}

static void test_serial2_write_only_in_real_transport_file() {
    std::vector<std::string> files;
    collect("lib", files);
    collect("src", files);
    for (const auto& f : files) {
        if (baseName(f) == kAllowedFile) continue;
        TEST_ASSERT_FALSE_MESSAGE(hasSerial2Write(slurp(f)), f.c_str());
    }
}

// 真實 transport 檔案存在，且 src/（main、loopback）完全不出現 Serial2 物件：只能透過 transport 介面。
static void test_src_never_mentions_serial2() {
    std::vector<std::string> files;
    collect("src", files);
    TEST_ASSERT_TRUE_MESSAGE(files.size() >= 2, "src/ 應有 poll_main 與 loopback_main");
    for (const auto& f : files) {
        static const std::regex bare(R"(\bSerial2\b)");  // Serial2Transport 這類類別名稱不算
        TEST_ASSERT_FALSE_MESSAGE(std::regex_search(slurp(f), bare), f.c_str());
    }
}
static void test_real_transport_has_third_layer_check() {
    std::string text = slurp("lib/modbus_ro/transport_serial2.cpp");
    TEST_ASSERT_TRUE(hasSerial2Write(text));                       // 它確實是唯一的寫入點
    TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("isReadFunctionFrame"));
    TEST_ASSERT_TRUE_MESSAGE(text.find("isReadFunctionFrame") < text.find("Serial2.write"),
                             "功能碼檢查必須在 Serial2.write 之前");
}

// 組裝層：不得存在任何寫入請求的組裝函式或接受任意功能碼的通用函式。
static void test_no_write_request_builders_exist() {
    std::vector<std::string> files;
    collect("lib", files);
    collect("src", files);
    static const std::regex forbidden(
        R"(\b(build|make|assemble|create)\w*(Write|WriteSingle|WriteMultiple|Preset|Coil|Control|Frame|Request)\w*\s*\()",
        std::regex::icase);
    for (const auto& f : files) {
        std::string text = slurp(f);
        std::smatch m;
        // buildReadRegisters 是唯一允許的組裝函式；其餘符合模式者即判失敗
        std::string cleaned = std::regex_replace(text, std::regex(R"(buildReadRegisters\s*\()"), "OK_(");
        bool bad = std::regex_search(cleaned, m, forbidden);
        TEST_ASSERT_FALSE_MESSAGE(bad, (f + ": " + m.str()).c_str());
    }
}

void run_static_tests() {
    RUN_TEST(test_detector_catches_forbidden_calls);
    RUN_TEST(test_scanner_finds_the_library_sources);
    RUN_TEST(test_serial2_write_only_in_real_transport_file);
    RUN_TEST(test_src_never_mentions_serial2);
    RUN_TEST(test_real_transport_has_third_layer_check);
    RUN_TEST(test_no_write_request_builders_exist);
}
