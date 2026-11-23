// 輸出一行文字的抽象介面。真實版本寫到 USB Serial（Serial，不是 Serial2），測試版本收集成陣列。
#pragma once

class ILineSink {
public:
    virtual ~ILineSink() = default;
    virtual void line(const char* text) = 0;  // 不含換行
};
