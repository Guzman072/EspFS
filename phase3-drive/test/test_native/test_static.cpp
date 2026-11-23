// Phase 3 靜態檢查：以文字搜尋原始碼與設定檔，確認寫入通道只存在於該存在的地方。
// `pio test` 的工作目錄是專案根目錄（phase3-drive/）；同時掃描 Phase 2 的 lib 與 src。
#include <unity.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include "transport.h"

namespace {
const char* const kDirs[] = {"lib", "src", "../phase2-modbus/lib", "../phase2-modbus/src"};

bool isSource(const std::string& n) {
    auto ends = [&](const char* e) {
        std::string s(e);
        return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
    };
    return ends(".h") || ends(".hpp") || ends(".cpp") || ends(".c");
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
std::vector<std::string> allSources() {
    std::vector<std::string> files;
    for (const char* d : kDirs) collect(d, files);
    return files;
}
std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string base(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
bool in(const std::string& name, std::initializer_list<const char*> allowed) {
    for (const char* a : allowed) if (name == a) return true;
    return false;
}
// 取出 platformio.ini 中 [env:name] 區段的文字
std::string envSection(const std::string& ini, const std::string& name) {
    const std::string tag = "[env:" + name + "]";
    size_t a = ini.find(tag);
    if (a == std::string::npos) return "";
    size_t b = ini.find("\n[", a + tag.size());
    return ini.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
// 去掉以 ; 或 # 開頭的註解行，只比對真正的設定
std::string noComments(const std::string& s) {
    std::stringstream in_(s), out;
    std::string line;
    while (std::getline(in_, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && (line[i] == ';' || line[i] == '#')) continue;
        out << line << "\n";
    }
    return out.str();
}
struct Dummy : ITransport {
    size_t write(const uint8_t*, size_t n) override { return n; }
    void flushTx() override {}
    void clearRx() override {}
    size_t available() override { return 0; }
    int readByte() override { return -1; }
    uint32_t nowMs() override { return 0; }
    void delayMs(uint32_t) override {}
};
}  // namespace

static void test_scanner_sees_both_projects() {
    auto files = allSources();
    bool p3 = false, p2 = false;
    for (const auto& f : files) {
        if (f.rfind("lib/drive_ctl", 0) == 0) p3 = true;
        if (f.rfind("../phase2-modbus/lib", 0) == 0) p2 = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(p3 && p2, "掃描不到原始碼，工作目錄可能不對");
}
static void test_serial2_write_only_in_real_transport_file() {
    static const std::regex re(R"(Serial2\s*\.\s*(write|print|println|printf|availableForWrite)\b)");
    for (const auto& f : allSources()) {
        if (base(f) == "transport_serial2.cpp") continue;
        TEST_ASSERT_FALSE_MESSAGE(std::regex_search(slurp(f), re), f.c_str());
    }
}
static void test_src_never_mentions_serial2_object() {
    static const std::regex bare(R"(\bSerial2\b)");
    std::vector<std::string> files;
    collect("src", files);
    collect("../phase2-modbus/src", files);
    TEST_ASSERT_TRUE(files.size() >= 3);
    for (const auto& f : files) TEST_ASSERT_FALSE_MESSAGE(std::regex_search(slurp(f), bare), f.c_str());
}
static void test_write_frames_are_assembled_only_in_write_frame_cpp() {
    // 寫入功能碼 0x06／0x10 只能被寫進請求位元組的第 2 個位置，而且只在 write_frame.cpp
    static const std::regex assign(R"(\[1\]\s*=\s*0x(06|10)\b)");
    bool found = false;
    for (const auto& f : allSources()) {
        const bool hit = std::regex_search(slurp(f), assign);
        if (base(f) == "write_frame.cpp") {
            found = hit;
        } else {
            TEST_ASSERT_FALSE_MESSAGE(hit, f.c_str());
        }
    }
    TEST_ASSERT_TRUE(found);
}
static void test_drive_live_only_in_transport_and_config() {
    for (const auto& f : allSources()) {
        const bool has = slurp(f).find("DRIVE_LIVE") != std::string::npos;
        if (in(base(f), {"transport_serial2.h", "transport_serial2.cpp", "config.h"})) continue;
        TEST_ASSERT_FALSE_MESSAGE(has, f.c_str());
    }
}
static void test_dryrun_flag_never_reaches_the_real_transport() {
    for (const auto& f : allSources()) {
        const bool has = slurp(f).find("DRIVE_DRYRUN") != std::string::npos;
        if (in(base(f), {"drive_link.cpp", "drive_fsm.cpp", "config.h"})) continue;
        TEST_ASSERT_FALSE_MESSAGE(has, f.c_str());
    }
}
static void test_write_path_in_transport_exists_only_under_drive_live() {
    for (const char* name : {"../phase2-modbus/lib/modbus_ro/transport_serial2.cpp", "../phase2-modbus/lib/modbus_ro/transport_serial2.h"}) {
        const std::string t = slurp(name);
        const size_t live = t.find("#ifdef DRIVE_LIVE");
        const size_t def = t.find("writeDrive");
        TEST_ASSERT_TRUE(live != std::string::npos && def != std::string::npos);
        TEST_ASSERT_TRUE_MESSAGE(live < def, name);
        // 第一個 Serial2.write（唯讀路徑）在 DRIVE_LIVE 區塊之前，第二個才在區塊內
        if (std::string(name).find(".cpp") != std::string::npos) {
            const size_t w1 = t.find("Serial2.write");
            const size_t w2 = t.find("Serial2.write", w1 + 1);
            TEST_ASSERT_TRUE(w1 < live && live < w2);
            TEST_ASSERT_NOT_EQUAL(std::string::npos, t.find("isAllowedDriveFrameShape"));
            TEST_ASSERT_TRUE(t.find("isAllowedDriveFrameShape") < w2);
        }
    }
}
static void test_base_transport_refuses_drive_writes_by_default() {
    Dummy d;
    const uint8_t f[8] = {1, 6, 0x20, 0x0E, 0, 7, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(0, d.writeDrive(f, 8));
}
static void test_platformio_envs() {
    const std::string p3 = noComments(slurp("platformio.ini"));
    const std::string p2 = noComments(slurp("../phase2-modbus/platformio.ini"));
    TEST_ASSERT_TRUE(p3.size() > 100);
    const std::string live = envSection(p3, "live"), dry = envSection(p3, "dryrun");
    const std::string nat = envSection(p3, "native"), natDry = envSection(p3, "native_dryrun");
    TEST_ASSERT_TRUE(!live.empty() && !dry.empty() && !nat.empty() && !natDry.empty());
    TEST_ASSERT_TRUE(live.find("-DDRIVE_LIVE") != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(live.find("DRIVE_DRYRUN") == std::string::npos, "live 不得存在 dryrun 旗標");
    TEST_ASSERT_TRUE(dry.find("-DDRIVE_DRYRUN") != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(dry.find("DRIVE_LIVE") == std::string::npos, "dryrun 不得含寫入傳輸（DRIVE_LIVE）");
    TEST_ASSERT_TRUE(nat.find("DRIVE_LIVE") == std::string::npos && nat.find("DRIVE_DRYRUN") == std::string::npos);
    TEST_ASSERT_TRUE(natDry.find("-DDRIVE_DRYRUN") != std::string::npos && natDry.find("DRIVE_LIVE") == std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(p2.find("DRIVE_") == std::string::npos, "Phase 2 專案不得有 DRIVE_ 旗標");
    // 通道旗標由使用者指定（2026-11-23）：dryrun、live 必須是同一組（油門 CH2＝1、SWA CH7＝6）。
    // FAILSAFE_VERIFIED=1 只能出現在 live（使用者於 2026-11-23 聲明 T2 (a)(b) 已在 dryrun 通過後指定）。
    // 之後若要改動這些旗標，須由使用者明確同意，並同步更新此測試。
    for (const std::string* env : {&live, &dry}) {
        TEST_ASSERT_TRUE_MESSAGE(env->find("-DTHROTTLE_CH=1") != std::string::npos, "THROTTLE_CH 必須是使用者指定的 1");
        TEST_ASSERT_TRUE_MESSAGE(env->find("-DARM_CH=6") != std::string::npos, "ARM_CH 必須是使用者指定的 6");
    }
    TEST_ASSERT_TRUE_MESSAGE(live.find("-DFAILSAFE_VERIFIED=1") != std::string::npos, "live 需要 FAILSAFE_VERIFIED=1");
    for (const std::string* env : {&dry, &nat, &natDry})
        TEST_ASSERT_TRUE_MESSAGE(env->find("FAILSAFE_VERIFIED") == std::string::npos, "FAILSAFE_VERIFIED 只能出現在 live");
    TEST_ASSERT_TRUE(p3.find("upload") == std::string::npos);
}
static void test_config_h_blocks_compilation_until_user_specifies() {
    const std::string c = slurp("src/config.h");
    static const std::regex t(R"(#ifndef THROTTLE_CH\s*\n\s*#error)");
    static const std::regex a(R"(#ifndef ARM_CH\s*\n\s*#error)");
    TEST_ASSERT_TRUE(std::regex_search(c, t));
    TEST_ASSERT_TRUE(std::regex_search(c, a));
    TEST_ASSERT_TRUE(c.find("defined(DRIVE_LIVE) && !defined(FAILSAFE_VERIFIED)") != std::string::npos);
    TEST_ASSERT_TRUE(c.find("defined(DRIVE_LIVE) && defined(DRIVE_DRYRUN)") != std::string::npos);
    static const std::regex def(R"(#\s*define\s+(THROTTLE_CH|ARM_CH|FAILSAFE_VERIFIED)\b)");
    for (const auto& f : allSources()) TEST_ASSERT_FALSE_MESSAGE(std::regex_search(slurp(f), def), f.c_str());
    TEST_ASSERT_TRUE(slurp("src/drive_main.cpp").find("#include \"config.h\"") != std::string::npos);
}
static void test_dryrun_build_has_no_write_transport_sources() {
    // dryrun 的進入點只含 drive_main.cpp；寫入傳輸 writeDrive 需要 DRIVE_LIVE
    const std::string p3 = noComments(slurp("platformio.ini"));
    const std::string dry = envSection(p3, "dryrun");
    TEST_ASSERT_TRUE(dry.find("build_src_filter = -<*> +<drive_main.cpp>") != std::string::npos);
}

void run_static_tests() {
    RUN_TEST(test_scanner_sees_both_projects);
    RUN_TEST(test_serial2_write_only_in_real_transport_file);
    RUN_TEST(test_src_never_mentions_serial2_object);
    RUN_TEST(test_write_frames_are_assembled_only_in_write_frame_cpp);
    RUN_TEST(test_drive_live_only_in_transport_and_config);
    RUN_TEST(test_dryrun_flag_never_reaches_the_real_transport);
    RUN_TEST(test_write_path_in_transport_exists_only_under_drive_live);
    RUN_TEST(test_base_transport_refuses_drive_writes_by_default);
    RUN_TEST(test_platformio_envs);
    RUN_TEST(test_config_h_blocks_compilation_until_user_specifies);
    RUN_TEST(test_dryrun_build_has_no_write_transport_sources);
}
