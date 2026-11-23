#include <unity.h>
#include <cstdlib>
#include "drive_frame_check.h"
#include "drive_registers.h"
#include "guard.h"
#include "test_support.h"
#include "write_frame.h"
#include "write_guard.h"
#include "write_rules.h"

static WriteGuard G90;  // limitRaw = 90（2022h＝1；上限 90 r/min，使用者決定 2026-11-23）

static void setUp_guard() { G90.setLimitRaw(90); }

static std::vector<uint8_t> w06(uint8_t id, uint16_t addr, uint16_t val) {
    return withCrc({id, 0x06, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
                    static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val & 0xFF)});
}
static std::vector<uint8_t> w10(uint8_t id, uint16_t start, uint16_t qty, uint8_t bytes, int16_t l, int16_t r) {
    return withCrc({id, 0x10, static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start & 0xFF),
                    static_cast<uint8_t>(qty >> 8), static_cast<uint8_t>(qty & 0xFF), bytes,
                    static_cast<uint8_t>(static_cast<uint16_t>(l) >> 8), static_cast<uint8_t>(static_cast<uint16_t>(l) & 0xFF),
                    static_cast<uint8_t>(static_cast<uint16_t>(r) >> 8), static_cast<uint8_t>(static_cast<uint16_t>(r) & 0xFF)});
}
static Status chk(const std::vector<uint8_t>& f) { return G90.check(f.data(), f.size()); }
static std::vector<uint8_t> r03(uint8_t id, uint16_t addr, uint16_t n) {
    return withCrc({id, 0x03, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
                    static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n & 0xFF)});
}

// ---------- 寫入白名單：每一條允許 ----------
static void test_allow_200D_only_3() {
    setUp_guard();
    TEST_ASSERT_EQUAL(Status::OK, chk(w06(1, 0x200D, 3)));
}
static void test_allow_200E_the_four_words() {
    setUp_guard();
    for (uint16_t w : {0x05, 0x06, 0x07, 0x08}) TEST_ASSERT_EQUAL(Status::OK, chk(w06(1, 0x200E, w)));
}
static void test_allow_2000_200_to_1000() {
    setUp_guard();
    for (uint16_t v : {200, 201, 500, 999, 1000}) TEST_ASSERT_EQUAL(Status::OK, chk(w06(1, 0x2000, v)));
}
static void test_allow_2088_two_regs_within_limit() {
    setUp_guard();
    TEST_ASSERT_EQUAL(Status::OK, chk(w10(1, 0x2088, 2, 4, 0, 0)));
    TEST_ASSERT_EQUAL(Status::OK, chk(w10(1, 0x2088, 2, 4, 90, 90)));
    TEST_ASSERT_EQUAL(Status::OK, chk(w10(1, 0x2088, 2, 4, -90, 90)));
    TEST_ASSERT_EQUAL(Status::OK, chk(w10(1, 0x2088, 2, 4, 90, -90)));
}
// ---------- 寫入白名單：每一條拒絕 ----------
static void test_reject_200D_other_values() {
    setUp_guard();
    for (uint16_t v : {0, 1, 2, 4, 5, 0x10, 0xFFFF}) TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w06(1, 0x200D, v)));
}
static void test_reject_200E_other_values() {
    setUp_guard();
    for (uint16_t v : {0, 1, 2, 3, 4, 9, 0x10, 0x11, 0x12, 0xFFFF}) TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w06(1, 0x200E, v)));
}
static void test_reject_2000_out_of_range() {
    setUp_guard();
    for (uint16_t v : {0, 1, 199, 1001, 5000, 0xFFFF}) TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w06(1, 0x2000, v)));
}
static void test_reject_2088_wrong_count_or_start() {
    setUp_guard();
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(w10(1, 0x2088, 1, 4, 0, 0)));   // 數量 1
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(w10(1, 0x2088, 3, 4, 0, 0)));   // 數量 3
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(w10(1, 0x2088, 0, 4, 0, 0)));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(w10(1, 0x2088, 2, 2, 0, 0)));   // 位元組數不是 4
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(w10(1, 0x2088, 2, 6, 0, 0)));
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, 0x2089, 2, 4, 0, 0)));   // 起始 ≠ 2088h
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, 0x2087, 2, 4, 0, 0)));
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, 0x208A, 2, 4, 0, 0)));   // 位置模式目標
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, 0x2090, 2, 4, 0, 0)));   // 轉矩目標
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, 0x200E, 2, 4, 0, 0)));
}
static void test_reject_2088_value_over_limit() {
    setUp_guard();
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, 91, 0)));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, 0, 91)));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, -91, 0)));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, 0, -91)));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, 3000, 3000)));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, chk(w10(1, 0x2088, 2, 4, -32768, 0)));
}
static void test_limit_follows_resolution_and_defaults_to_zero() {
    WriteGuard g;  // 未設定：只允許 0
    auto zero = w10(1, 0x2088, 2, 4, 0, 0);
    auto one = w10(1, 0x2088, 2, 4, 1, 0);
    TEST_ASSERT_EQUAL(Status::OK, g.check(zero.data(), zero.size()));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.check(one.data(), one.size()));
    g.setLimitRaw(limitRawFor(10));  // 2022h＝A → 900
    auto f900 = w10(1, 0x2088, 2, 4, 900, -900);
    auto f901 = w10(1, 0x2088, 2, 4, 901, 0);
    TEST_ASSERT_EQUAL(Status::OK, g.check(f900.data(), f900.size()));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, g.check(f901.data(), f901.size()));
}
static void test_reject_slave_id_not_1() {
    setUp_guard();
    for (uint8_t id : {0, 2, 4, 127, 128, 255}) {
        TEST_ASSERT_EQUAL(Status::REJECTED_ID, chk(w06(id, 0x200E, 7)));
        TEST_ASSERT_EQUAL(Status::REJECTED_ID, chk(w10(id, 0x2088, 2, 4, 0, 0)));
        TEST_ASSERT_EQUAL(Status::REJECTED_ID, chk(r03(id, 0x20A0, 1)));
    }
}
// ---------- 黑名單 ----------
static void test_blacklist_explicit_registers_fc06_and_fc10() {
    setUp_guard();
    for (uint16_t a : {0x2031, 0x2061, 0x2010, 0x2009, 0x2001, 0x2002}) {
        TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w06(1, a, 1)));
        TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w06(1, a, 0)));
        TEST_ASSERT_EQUAL(Status::REJECTED_REG, chk(w10(1, a, 2, 4, 0, 0)));
        TEST_ASSERT_TRUE(isBlacklistedReg(a));
    }
}
static void test_blacklist_2030_to_207A_every_address() {
    setUp_guard();
    for (uint16_t a = 0x2030; a <= 0x207A; ++a) {
        TEST_ASSERT_TRUE(isBlacklistedReg(a));
        TEST_ASSERT_EQUAL_MESSAGE(Status::REJECTED_REG, chk(w06(1, a, 3)), "FC06 黑名單");
        TEST_ASSERT_EQUAL_MESSAGE(Status::REJECTED_REG, chk(w10(1, a, 2, 4, 0, 0)), "FC10 黑名單");
    }
    TEST_ASSERT_FALSE(isBlacklistedReg(0x202F));
    TEST_ASSERT_FALSE(isBlacklistedReg(0x207B));
}
static void test_every_unlisted_single_register_is_rejected() {
    setUp_guard();
    int accepted = 0;
    for (uint32_t a = 0; a <= 0xFFFF; a += 1) {
        auto f = w06(1, static_cast<uint16_t>(a), 3);
        if (G90.check(f.data(), f.size()) == Status::OK) ++accepted;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, accepted, "FC06 值 3 只有 200Dh 會通過");  // 2000h 的 3 超出 200–1000、200Eh 的 3 不在集合
}
// ---------- 功能碼、長度、CRC ----------
static void test_all_256_function_codes() {
    setUp_guard();
    int passedFc = 0;
    for (int fc = 0; fc <= 0xFF; ++fc) {
        auto f = withCrc({1, static_cast<uint8_t>(fc), 0x20, 0xA0, 0x00, 0x01});
        const Status s = G90.check(f.data(), f.size());
        if (fc == 0x03) {
            TEST_ASSERT_EQUAL(Status::OK, s);
            ++passedFc;
        } else {
            TEST_ASSERT_TRUE_MESSAGE(s != Status::OK, "非 0x03 的 6 位元組幀不可能通過");
            if (fc != 0x06 && fc != 0x10) TEST_ASSERT_EQUAL(Status::REJECTED_FC, s);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, passedFc);
}
static void test_guarded_write_sends_nothing_for_rejected_frames() {
    setUp_guard();
    DriveMock t;
    auto bad = w06(1, 0x2031, 5);
    TEST_ASSERT_EQUAL(Status::REJECTED_REG, guardedWrite(t, G90, bad.data(), bad.size()));
    auto bad2 = w10(2, 0x2088, 2, 4, 0, 0);
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, guardedWrite(t, G90, bad2.data(), bad2.size()));
    TEST_ASSERT_EQUAL_UINT32(0, t.totalWritten);
    TEST_ASSERT_EQUAL_UINT32(0, t.driveWrites.size());
    auto good = w06(1, 0x200E, 7);
    TEST_ASSERT_EQUAL(Status::OK, guardedWrite(t, G90, good.data(), good.size()));
    TEST_ASSERT_EQUAL_UINT32(1, t.driveWrites.size());  // FC 0x06 走 writeDrive
    auto rd = r03(1, 0x20A0, 1);
    TEST_ASSERT_EQUAL(Status::OK, guardedWrite(t, G90, rd.data(), rd.size()));
    TEST_ASSERT_EQUAL_UINT32(1, t.driveWrites.size());  // FC 0x03 走 write()，不經 writeDrive
}
static void test_length_and_crc_rules() {
    setUp_guard();
    auto f = w06(1, 0x200E, 7);
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, G90.check(f.data(), 7));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, G90.check(f.data(), 1));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, G90.check(nullptr, 0));
    f[7] ^= 0xFF;
    TEST_ASSERT_EQUAL(Status::REJECTED_CRC, chk(f));
    auto m = w10(1, 0x2088, 2, 4, 0, 0);
    m.pop_back();
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(m));
    auto m2 = w10(1, 0x2088, 2, 4, 0, 0);
    m2[m2.size() - 1] ^= 0xFF;
    TEST_ASSERT_EQUAL(Status::REJECTED_CRC, chk(m2));
    auto s = w06(1, 0x200E, 7);
    s.push_back(0);
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, chk(s));
}
// ---------- 讀取白名單（Phase 3）與 Phase 2 沒放寬 ----------
static void test_phase3_read_whitelist() {
    for (uint16_t a : {0x2000, 0x2001, 0x2002, 0x2007, 0x2008, 0x200F, 0x2011, 0x2022, 0x2023, 0x2088, 0x2089, 0x20A0, 0x20AE})
        TEST_ASSERT_TRUE(driveReadAllowed(a, 1));
    TEST_ASSERT_TRUE(driveReadAllowed(0x2007, 2));
    TEST_ASSERT_TRUE(driveReadAllowed(0x2088, 2));
    TEST_ASSERT_TRUE(driveReadAllowed(0x20A0, 15));
    TEST_ASSERT_FALSE(driveReadAllowed(0x2011, 2));  // 2012h 不在白名單
    TEST_ASSERT_FALSE(driveReadAllowed(0x2008, 2));  // 2009h 不在
    TEST_ASSERT_FALSE(driveReadAllowed(0x2007, 3));
    TEST_ASSERT_FALSE(driveReadAllowed(0x20AF, 1));
    TEST_ASSERT_FALSE(driveReadAllowed(0x20B0, 1));
    TEST_ASSERT_FALSE(driveReadAllowed(0x2010, 1));
    TEST_ASSERT_FALSE(driveReadAllowed(0x2031, 1));
    TEST_ASSERT_FALSE(driveReadAllowed(0x2003, 1));
    TEST_ASSERT_FALSE(driveReadAllowed(0x20A0, 16));
    TEST_ASSERT_FALSE(driveReadAllowed(0x2000, 0));
    TEST_ASSERT_FALSE(driveReadAllowed(0xFFFF, 2));
}
static void test_phase2_guard_still_rejects_the_new_addresses() {
    for (uint16_t a : {0x2007, 0x2008, 0x200F, 0x2011, 0x2022, 0x2023, 0x2088, 0x2089}) {
        auto f = r03(1, a, 1);
        TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, checkRequest(f.data(), f.size()));
        TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, readOnlyGuard().check(f.data(), f.size()));
        TEST_ASSERT_FALSE(rangeAllowed(a, 1));
        TEST_ASSERT_EQUAL(Status::OK, G90.check(f.data(), f.size()));  // Phase 3 的 guard 允許
    }
    for (uint8_t fc : {uint8_t(0x06), uint8_t(0x10)}) {
        auto f = withCrc({1, fc, 0x20, 0x0E, 0x00, 0x07});
        TEST_ASSERT_EQUAL(Status::REJECTED_FC, readOnlyGuard().check(f.data(), f.size()));
    }
}
static void test_build_drive_read_validation() {
    Request r;
    TEST_ASSERT_EQUAL(Status::OK, buildDriveRead(1, 0x2022, 1, r));
    TEST_ASSERT_EQUAL_HEX8(0x03, r.bytes[1]);
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildDriveRead(4, 0x2022, 1, r));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, buildDriveRead(1, 0x2022, 0, r));
    TEST_ASSERT_EQUAL(Status::REJECTED_LEN, buildDriveRead(1, 0x2022, 17, r));
    TEST_ASSERT_EQUAL(Status::REJECTED_ADDR, buildDriveRead(1, 0x2010, 1, r));
}
static void test_decode_extra_registers() {
    uint16_t v[1] = {10};
    DecodedValue out[4];
    TEST_ASSERT_EQUAL_UINT32(1, driveDecodeBlock(0x2022, v, 1, out, 4));
    TEST_ASSERT_EQUAL_STRING("speed_resolution", out[0].name);
    TEST_ASSERT_EQUAL_INT32(10, out[0].scaled);
    uint16_t two[2] = {5, 1000};
    TEST_ASSERT_EQUAL_UINT32(2, driveDecodeBlock(0x2007, two, 2, out, 4));   // 2007h、2008h
    TEST_ASSERT_EQUAL_STRING("power_on_lock_mode", out[0].name);
    TEST_ASSERT_EQUAL_STRING("max_rpm", out[1].name);
    uint16_t mixed[3] = {1000, 1, 1};  // 2000h、2001h 走 Phase 2 解碼
    TEST_ASSERT_EQUAL_UINT32(3, driveDecodeBlock(0x2000, mixed, 3, out, 4));
    TEST_ASSERT_EQUAL_STRING("comm_offline_ms", out[0].name);
    uint16_t cap[2] = {1, 1};
    DecodedValue one[1];
    TEST_ASSERT_EQUAL_UINT32(1, driveDecodeBlock(0x2088, cap, 2, one, 1));
}
// ---------- builder ----------
static void test_builders_match_manual_frames() {
    WriteFrame f{};
    TEST_ASSERT_EQUAL(Status::OK, buildSetSpeedMode(1, f));  // V4 RS485 p.5：01 06 20 0D 00 03 53 C8
    const uint8_t mode[8] = {0x01, 0x06, 0x20, 0x0D, 0x00, 0x03, 0x53, 0xC8};
    TEST_ASSERT_EQUAL_UINT32(8, f.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mode, f.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildControlWord(1, 0x08, f));
    const uint8_t en[8] = {0x01, 0x06, 0x20, 0x0E, 0x00, 0x08, 0xE2, 0x0F};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(en, f.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildControlWord(1, 0x07, f));
    const uint8_t stop[8] = {0x01, 0x06, 0x20, 0x0E, 0x00, 0x07, 0xA2, 0x0B};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(stop, f.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildControlWord(1, 0x05, f));
    const uint8_t estop[8] = {0x01, 0x06, 0x20, 0x0E, 0x00, 0x05, 0x23, 0xCA};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(estop, f.bytes, 8);
    TEST_ASSERT_EQUAL(Status::OK, buildSetSpeeds(1, 100, 100, 100, f));
    const uint8_t sp[13] = {0x01, 0x10, 0x20, 0x88, 0x00, 0x02, 0x04, 0x00, 0x64, 0x00, 0x64, 0x23, 0x9C};
    TEST_ASSERT_EQUAL_UINT32(13, f.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sp, f.bytes, 13);
    TEST_ASSERT_EQUAL(Status::OK, buildSetSpeeds(1, -10, 100, 100, f));
    const uint8_t mix[13] = {0x01, 0x10, 0x20, 0x88, 0x00, 0x02, 0x04, 0xFF, 0xF6, 0x00, 0x64, 0xB2, 0x65};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mix, f.bytes, 13);
    TEST_ASSERT_EQUAL(Status::OK, buildSetOfflineTime(1, 500, f));
    TEST_ASSERT_EQUAL_HEX8(0x20, f.bytes[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.bytes[3]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f.bytes[4]);
    TEST_ASSERT_EQUAL_HEX8(0xF4, f.bytes[5]);
}
static void test_builders_reject_bad_parameters_and_leave_output_alone() {
    WriteFrame f{};
    f.len = 99;
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildSetSpeedMode(2, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildControlWord(0, 8, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildControlWord(1, 0x10, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildControlWord(1, 0, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildSetOfflineTime(1, 199, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildSetOfflineTime(1, 1001, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildSetOfflineTime(3, 500, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildSetSpeeds(1, 91, 0, 90, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildSetSpeeds(1, 0, -91, 90, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_VALUE, buildSetSpeeds(1, 0, 0, -1, f));
    TEST_ASSERT_EQUAL(Status::REJECTED_ID, buildSetSpeeds(5, 0, 0, 90, f));
    TEST_ASSERT_EQUAL_UINT32(99, f.len);
}
static void test_every_builder_output_passes_the_guard() {
    setUp_guard();
    WriteFrame f{};
    buildSetSpeedMode(1, f);
    TEST_ASSERT_EQUAL(Status::OK, G90.check(f.bytes, f.len));
    for (uint16_t w : {0x05, 0x06, 0x07, 0x08}) {
        buildControlWord(1, w, f);
        TEST_ASSERT_EQUAL(Status::OK, G90.check(f.bytes, f.len));
    }
    buildSetOfflineTime(1, 200, f);
    TEST_ASSERT_EQUAL(Status::OK, G90.check(f.bytes, f.len));
    buildSetSpeeds(1, -90, 90, 90, f);
    TEST_ASSERT_EQUAL(Status::OK, G90.check(f.bytes, f.len));
}
// ---------- 第三層「形狀」檢查 ----------
static void test_third_layer_shape_check() {
    auto ok06 = w06(1, 0x200E, 7);
    TEST_ASSERT_TRUE(isAllowedDriveFrameShape(ok06.data(), ok06.size()));
    for (uint16_t a : {0x2000, 0x200D, 0x200E}) {
        auto f = w06(1, a, 1);
        TEST_ASSERT_TRUE(isAllowedDriveFrameShape(f.data(), f.size()));
    }
    for (uint16_t a : {0x2001, 0x2002, 0x2010, 0x2031, 0x2061, 0x2009, 0x2088, 0x200F}) {
        auto f = w06(1, a, 1);
        TEST_ASSERT_FALSE(isAllowedDriveFrameShape(f.data(), f.size()));
    }
    auto ok10 = w10(1, 0x2088, 2, 4, 0, 0);
    TEST_ASSERT_TRUE(isAllowedDriveFrameShape(ok10.data(), ok10.size()));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(w10(1, 0x2087, 2, 4, 0, 0).data(), 13));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(w10(1, 0x2088, 1, 4, 0, 0).data(), 13));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(w10(1, 0x2088, 2, 2, 0, 0).data(), 13));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(ok10.data(), 12));
    TEST_ASSERT_TRUE(isAllowedDriveFrameShape(r03(1, 0x20A0, 1).data(), 8));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(nullptr, 8));
    TEST_ASSERT_FALSE(isAllowedDriveFrameShape(ok06.data(), 1));
    for (int fc = 0; fc <= 0xFF; ++fc) {
        if (fc == 0x03 || fc == 0x06 || fc == 0x10) continue;
        auto f = withCrc({1, static_cast<uint8_t>(fc), 0x20, 0x0E, 0x00, 0x07});
        TEST_ASSERT_FALSE(isAllowedDriveFrameShape(f.data(), f.size()));
    }
}

void run_write_guard_tests() {
    RUN_TEST(test_allow_200D_only_3);
    RUN_TEST(test_allow_200E_the_four_words);
    RUN_TEST(test_allow_2000_200_to_1000);
    RUN_TEST(test_allow_2088_two_regs_within_limit);
    RUN_TEST(test_reject_200D_other_values);
    RUN_TEST(test_reject_200E_other_values);
    RUN_TEST(test_reject_2000_out_of_range);
    RUN_TEST(test_reject_2088_wrong_count_or_start);
    RUN_TEST(test_reject_2088_value_over_limit);
    RUN_TEST(test_limit_follows_resolution_and_defaults_to_zero);
    RUN_TEST(test_reject_slave_id_not_1);
    RUN_TEST(test_blacklist_explicit_registers_fc06_and_fc10);
    RUN_TEST(test_blacklist_2030_to_207A_every_address);
    RUN_TEST(test_every_unlisted_single_register_is_rejected);
    RUN_TEST(test_all_256_function_codes);
    RUN_TEST(test_guarded_write_sends_nothing_for_rejected_frames);
    RUN_TEST(test_length_and_crc_rules);
    RUN_TEST(test_phase3_read_whitelist);
    RUN_TEST(test_phase2_guard_still_rejects_the_new_addresses);
    RUN_TEST(test_build_drive_read_validation);
    RUN_TEST(test_decode_extra_registers);
    RUN_TEST(test_builders_match_manual_frames);
    RUN_TEST(test_builders_reject_bad_parameters_and_leave_output_alone);
    RUN_TEST(test_every_builder_output_passes_the_guard);
    RUN_TEST(test_third_layer_shape_check);
}
