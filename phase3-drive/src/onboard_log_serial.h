#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <unistd.h>
#include "onboard_log.h"

class OnboardSerialLog {
public:
    void begin() {
        char* psram = static_cast<char*>(ps_malloc(kRingBytes));
        if (!psram) {
            Serial.println("# ONBOARD_LOG PSRAM unavailable");
            return;
        }
        ring_ = OnboardLog(psram, kRingBytes);
        // 只在全空的分割區建立檔案系統。掛載失敗時保留已有的資料。
        if (!LittleFS.begin() && !(partitionIsBlank() && LittleFS.begin(true))) {
            Serial.println("# ONBOARD_LOG LittleFS unavailable");
            return;
        }
        Preferences preferences;
        if (preferences.begin("onboard-log")) {
            bootSequence_ = preferences.getUInt("boot", 0) + 1;
            ready_ = preferences.putUInt("boot", bootSequence_) == sizeof(bootSequence_);
        }
        if (!ready_) Serial.println("# ONBOARD_LOG boot sequence unavailable");
    }

    void line(const char* text) {
        ring_.capture(text);
        if (dump_) {
            // 狀態轉換要即時顯示。dump 每次送完整一行，因此中斷也不會拆開 CSV。
            if (std::strncmp(text, "S,", 2) != 0) return;
            dump_.close();  // 唯讀 handle
            Serial.println("# ONBOARD_LOG dump interrupted");
        }
        Serial.println(text);
    }

    void service(const DriveFsm& fsm) {
        const DriveState state = fsm.state();
        const char* reason = faultReasonName(fsm.faultReason());
        const bool safe = OnboardLog::storageAllowed(state, fsm.stopSampling());
        // 與 FSM 在同一個 loop。所有檔案操作都在完成本次 step 後，且只在安全狀態執行。
        ring_.update(state, fsm.stopSampling(), [&](const char* a, size_t aSize, const char* b, size_t bSize) {
            return ready_ && append(state, reason, a, aSize, b, bSize);
        });
        heartbeat(state, reason);
        pollCommand(safe);
        if (safe && dump_) sendDumpLine();
    }

private:
    static constexpr size_t kRingBytes = 128 * 1024;
    static constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;
    static constexpr const char* kLogPath = "/onboard.log";
    static constexpr const char* kMountedLogPath = "/littlefs/onboard.log";

    OnboardLog ring_;
    fs::File dump_;
    bool ready_ = false;
    uint32_t bootSequence_ = 0;
    uint32_t lastHeartbeatMs_ = 0;
    char command_[6] = {};
    size_t commandLength_ = 0;

    static bool partitionIsBlank() {
        const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
        if (!part) return false;
        uint8_t bytes[512];
        for (size_t offset = 0; offset < part->size; offset += sizeof bytes) {
            const size_t count = std::min(sizeof bytes, static_cast<size_t>(part->size) - offset);
            if (esp_partition_read(part, offset, bytes, count) != ESP_OK) return false;
            for (size_t i = 0; i < count; ++i) if (bytes[i] != 0xFF) return false;
        }
        return true;
    }

    bool append(DriveState state, const char* reason, const char* a, size_t aSize, const char* b, size_t bSize) {
        char header[96];
        const int length = std::snprintf(header, sizeof header, "\n# BOOT seq=%lu\n# STATE %s reason=%s\n",
                                         static_cast<unsigned long>(bootSequence_), driveStateName(state), reason);
        auto* file = std::fopen(kMountedLogPath, "a");
        if (!file) {
            Serial.println("# ONBOARD_LOG open failed");
            return false;
        }
        const long fileSize = std::fseek(file, 0, SEEK_END) == 0 ? std::ftell(file) : -1;
        if (fileSize >= 0 && static_cast<size_t>(fileSize) + length + aSize + bSize > kMaxFileBytes) {
            // 保持單檔及固定上限，從最新一批記錄重新開始。
            std::fclose(file);
            file = std::fopen(kMountedLogPath, "w");
            if (!file) {
                Serial.println("# ONBOARD_LOG rollover failed");
                return false;
            }
            Serial.println("# ONBOARD_LOG rollover");
        }
        const bool written = fileSize >= 0 && write(file, header, length) && write(file, a, aSize) && write(file, b, bSize);
        const bool synced = written && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
        const bool ok = std::fclose(file) == 0 && synced;
        if (!ok) Serial.println("# ONBOARD_LOG write failed");
        return ok;
    }

    static bool write(std::FILE* file, const char* data, size_t length) {
        return length == 0 || std::fwrite(data, 1, length, file) == length;
    }

    void heartbeat(DriveState state, const char* reason) {
        const uint32_t now = millis();
        if (now - lastHeartbeatMs_ < 5000) return;
        char text[64];
        const int length = std::snprintf(text, sizeof text, "# STATE %s reason=%s", driveStateName(state), reason);
        if (Serial.availableForWrite() < length + 2) return;
        Serial.println(text);
        lastHeartbeatMs_ = now;
    }

    void pollCommand(bool safe) {
        for (int i = 0; i < 16 && Serial.available(); ++i) {
            const int ch = Serial.read();
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (commandLength_ > 0 && commandLength_ < sizeof command_) {
                    command_[commandLength_] = '\0';
                    handleCommand(safe);
                }
                commandLength_ = 0;
            } else if (commandLength_ < sizeof command_) {
                command_[commandLength_++] = static_cast<char>(ch);
            }
        }
    }

    void handleCommand(bool safe) {
        const bool dumpCommand = std::strcmp(command_, "dump") == 0;
        const bool eraseCommand = std::strcmp(command_, "erase") == 0;
        if (!dumpCommand && !eraseCommand) return;
        if (!safe || dump_) {
            if (Serial.availableForWrite() >= 32) Serial.println("# ONBOARD_LOG busy");
            return;
        }
        if (!ready_) {
            Serial.println("# ONBOARD_LOG unavailable");
        } else if (dumpCommand) {
            dump_ = LittleFS.open(kLogPath, FILE_READ);
            Serial.println(dump_ ? "# ONBOARD_LOG dump begin" : "# ONBOARD_LOG no log");
        } else if (!LittleFS.exists(kLogPath) || LittleFS.remove(kLogPath)) {
            ring_.clear();
            Serial.println("# ONBOARD_LOG erased");
        } else {
            Serial.println("# ONBOARD_LOG erase failed");
        }
    }

    void sendDumpLine() {
        // 一次只讀一行，等 UART 有空間才送。正常 log 行最長 159 bytes。
        char line[192];
        if (Serial.availableForWrite() < static_cast<int>(sizeof line + 2)) return;
        size_t length = 0;
        while (length < sizeof line - 1) {
            const int ch = dump_.read();
            if (ch < 0) {
                if (dump_.available()) {
                    dump_.close();
                    Serial.println("# ONBOARD_LOG dump read failed");
                    return;
                }
                break;
            }
            if (ch == '\n') break;
            line[length++] = static_cast<char>(ch);
        }
        if (length > 0 || dump_.available()) {
            line[length] = '\0';
            Serial.println(line);
        }
        if (!dump_.available()) {
            dump_.close();
            Serial.println("# ONBOARD_LOG dump end");
        }
    }
};
