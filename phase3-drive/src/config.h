// Phase 3 進入點的設定。通道對應與失效保護驗證由使用者指定，預設未定義，未定義時編譯期 #error 擋住。
//
// 使用者之後以一句話指定，例如：
//   pio run -e dryrun   需要 -DTHROTTLE_CH=<油門通道 0 起算> -DARM_CH=<SWA 通道 0 起算>
//   pio run -e live     另需 -DFAILSAFE_VERIFIED=1（T2 通過後才可指定，見 DESIGN.md §7）
// 這些旗標由使用者寫進 platformio.ini 的對應環境；Claude 不自行假設通道（Phase 1 的 CH1～CH10 對應尚未驗證）。
#pragma once
#include "drive_config.h"
#include "remote_input.h"

#ifndef THROTTLE_CH
#error "THROTTLE_CH 未定義：請由使用者指定油門通道（0 起算），例如 -DTHROTTLE_CH=2。Claude 不自行假設通道。"
#endif
#ifndef ARM_CH
#error "ARM_CH 未定義：請由使用者指定解鎖開關 SWA 的通道（0 起算），例如 -DARM_CH=4。Claude 不自行假設通道。"
#endif

#if defined(DRIVE_LIVE) && defined(DRIVE_DRYRUN)
#error "DRIVE_LIVE 與 DRIVE_DRYRUN 不可同時定義"
#endif
#if !defined(DRIVE_LIVE) && !defined(DRIVE_DRYRUN)
#error "必須選 dryrun（DRIVE_DRYRUN）或 live（DRIVE_LIVE）環境"
#endif

#if defined(DRIVE_LIVE) && !defined(FAILSAFE_VERIFIED)
#error "live 需要 -DFAILSAFE_VERIFIED=1：T2（遙控器關機後 i-BUS 進入 STALE）通過後才可由使用者指定。"
#endif

#ifdef STEER_CH
static_assert(STEER_CH >= 0 && STEER_CH < kRemoteChannels, "STEER_CH 必須在 i-BUS 通道範圍內");
static_assert(STEER_CH != THROTTLE_CH && STEER_CH != ARM_CH, "STEER_CH 不可與油門或 SWA 共用通道");
constexpr int kSteeringChannel = STEER_CH;
#else
constexpr int kSteeringChannel = -1;
#endif

// 綠板 JK2501（s3_drive_dry／s3_drive_live）啟用的額外安全項 (a)(b)(c)，見 DESIGN.md §12。esp32dev 環境不定義。
#ifdef DRIVE_STRICT_SAFETY
constexpr bool kStrictSafety = true;
#else
constexpr bool kStrictSafety = false;
#endif

#ifdef DRIVE_LIVE
constexpr bool kFailsafeVerified = (FAILSAFE_VERIFIED + 0) != 0;
#else
constexpr bool kFailsafeVerified = true;  // dryrun 不需要
#endif
