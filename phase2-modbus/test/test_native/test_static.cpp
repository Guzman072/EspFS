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

// ---- JK2501 綠板（S3）只讀環境：編譯期就不能產生寫入（Issue #8） ----
namespace {
// 先去掉 PlatformIO 的 ; 和 # 註解，再取出 [env:name] 區段。
std::string envSection(const std::string& ini, const std::string& name) {
    static const std::regex comment(R"((^|\s)[;#][^\n]*)");
    const std::string code = std::regex_replace(ini, comment, "$1");
    const std::string head = "[env:" + name + "]";
    size_t b = code.find(head);
    if (b == std::string::npos) return "";
    size_t e = code.find("\n[", b + head.size());
    return code.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// Strapping、Y1–Y6、CH340 UART0（43/44）、原生 USB（19/20）。
const std::regex kReservedPinFlag(R"((^|\s)-D\s*[A-Za-z0-9_]*_PIN\s*=\s*(0|3|13|19|20|39|40|41|42|43|44|45|46)\b)");

bool contains(const std::string& t, const char* s) { return t.find(s) != std::string::npos; }

// 程式碼內的寫入功能碼常數（0x06／0x10 作為數值出現）。註解先去掉。
bool mentionsWriteFc(const std::string& text) {
    std::string code = std::regex_replace(text, std::regex(R"(//[^\n]*)"), "");
    return std::regex_search(code, std::regex(R"(0x0?6\b|0x10\b)", std::regex::icase));
}
}  // namespace

static void test_s3_poll_env_is_read_only() {
    const std::string sec = envSection(slurp("platformio.ini"), "s3_poll");
    TEST_ASSERT_FALSE_MESSAGE(sec.empty(), "找不到 [env:s3_poll]");
    TEST_ASSERT_FALSE(contains(sec, "DRIVE_LIVE"));
    TEST_ASSERT_FALSE(contains(sec, "DRIVE_DRYRUN"));
    TEST_ASSERT_FALSE(contains(sec, "FAILSAFE_VERIFIED"));
    TEST_ASSERT_FALSE_MESSAGE(contains(sec, "phase3"), "s3_poll 不得連結 phase3（FC06/FC10 程式所在）");
    TEST_ASSERT_FALSE(contains(sec, "lib_extra_dirs"));
    TEST_ASSERT_TRUE(contains(sec, "+<s3_poll_main.cpp>"));
    TEST_ASSERT_TRUE(contains(sec, "-DRS485_RX_PIN=15"));
    TEST_ASSERT_TRUE(contains(sec, "-DRS485_TX_PIN=16"));
    TEST_ASSERT_TRUE(contains(sec, "-DMODBUS_MAX_ATTEMPTS=1"));
    TEST_ASSERT_FALSE_MESSAGE(contains(sec, "RS485_DE_PIN"), "使用者指定不設 DE/RE 腳");
    TEST_ASSERT_FALSE_MESSAGE(std::regex_search(sec, std::regex(R"(GPIO3[5-7]|PIN=3[5-7]\b)")), "GPIO35–37 被 PSRAM 佔用");
    TEST_ASSERT_FALSE_MESSAGE(std::regex_search(sec, kReservedPinFlag), "s3_poll 的 *_PIN 不得使用 JK2501 保留腳");
}

static void test_s3_ibus_env_has_no_modbus() {
    const std::string sec = envSection(slurp("../phase1-ibus/platformio.ini"), "s3_ibus");
    TEST_ASSERT_FALSE_MESSAGE(sec.empty(), "找不到 [env:s3_ibus]");
    TEST_ASSERT_FALSE(contains(sec, "DRIVE_LIVE"));
    TEST_ASSERT_FALSE(contains(sec, "lib_extra_dirs"));
    TEST_ASSERT_TRUE(contains(sec, "-DIBUS_RX_PIN=18"));
    TEST_ASSERT_FALSE_MESSAGE(std::regex_search(sec, kReservedPinFlag), "s3_ibus 的 *_PIN 不得使用 JK2501 保留腳");
    TEST_ASSERT_FALSE_MESSAGE(std::regex_search(sec, std::regex(R"(PIN=3[5-7]\b)")), "GPIO35–37 被 PSRAM 佔用");
    TEST_ASSERT_FALSE_MESSAGE(contains(sec, "RS485_DE_PIN"), "使用者指定不設 DE/RE 腳");
    const std::string src = slurp("../phase1-ibus/src/main.cpp");
    TEST_ASSERT_FALSE(contains(src, "Serial2"));
    TEST_ASSERT_FALSE(contains(src, "modbus"));
}

// s3_poll 連結的 phase2 函式庫：寫入通道只存在 DRIVE_LIVE 區塊內，且沒有 FC06/FC10 常數。
static void test_phase2_lib_has_no_write_path_without_drive_live() {
    std::vector<std::string> files;
    collect("lib", files);
    collect("src", files);
    for (const auto& f : files) {
        std::string text = slurp(f);
        if (baseName(f) == "transport_serial2.cpp" || baseName(f) == "transport_serial2.h") {
            // 去掉 #ifdef DRIVE_LIVE ... #endif 區塊後，不得再有 writeDrive 的實作
            text = std::regex_replace(text, std::regex(R"(#ifdef DRIVE_LIVE[\s\S]*?#endif)"), "");
        }
        // 寫入通道的宣告或實作（呼叫不算：guard.cpp 呼叫時，s3_poll 只會走到 ITransport 預設的回 0）。
        // transport.h 的預設實作就是拒絕（回 0），另由 test_default_write_drive_refuses 類測試涵蓋。
        static const std::regex def(R"((::\s*writeDrive\s*\()|(\bwriteDrive\s*\(\s*const\b))");
        if (baseName(f) != "transport.h") {
            TEST_ASSERT_FALSE_MESSAGE(std::regex_search(text, def), (f + "：DRIVE_LIVE 以外不得有 writeDrive 實作").c_str());
        }
        TEST_ASSERT_FALSE_MESSAGE(contains(text, "drive_"), (f + "：不得引用 phase3 drive_ 程式").c_str());
        TEST_ASSERT_FALSE_MESSAGE(mentionsWriteFc(text), (f + "：出現 0x06／0x10").c_str());
    }
    const std::string s3 = slurp("src/s3_poll_main.cpp");
    TEST_ASSERT_TRUE(contains(s3, "#error"));
    TEST_ASSERT_FALSE(contains(s3, "Serial2."));
}

static void test_write_fc_detector_works() {
    TEST_ASSERT_TRUE(mentionsWriteFc("uint8_t fc = 0x06;"));
    TEST_ASSERT_TRUE(mentionsWriteFc("x == 0x10)"));
    TEST_ASSERT_FALSE(mentionsWriteFc("// FC 0x06 comment only"));
    TEST_ASSERT_FALSE(mentionsWriteFc("0x20A0 0x103"));
}

void run_static_tests() {
    RUN_TEST(test_detector_catches_forbidden_calls);
    RUN_TEST(test_scanner_finds_the_library_sources);
    RUN_TEST(test_serial2_write_only_in_real_transport_file);
    RUN_TEST(test_src_never_mentions_serial2);
    RUN_TEST(test_real_transport_has_third_layer_check);
    RUN_TEST(test_no_write_request_builders_exist);
    RUN_TEST(test_write_fc_detector_works);
    RUN_TEST(test_s3_poll_env_is_read_only);
    RUN_TEST(test_s3_ibus_env_has_no_modbus);
    RUN_TEST(test_phase2_lib_has_no_write_path_without_drive_live);
}
