#include "write_rules.h"
#include "drive_config.h"

bool isBlacklistedReg(uint16_t a) {
    return a == 0x2031 || a == 0x2061 || a == 0x2010 || a == 0x2009 || a == 0x2001 || a == 0x2002 ||
           (a >= 0x2030 && a <= 0x207A);
}

bool controlWordAllowed(uint16_t w) {
    return w == kCtrlEmergencyStop || w == kCtrlClearFault || w == kCtrlStop || w == kCtrlEnable;
}

bool offlineTimeAllowed(uint16_t ms) { return ms >= kOfflineMinMs && ms <= kOfflineMaxMs; }

bool writeRegAllowedForSingle(uint16_t a) {
    return a == kRegOfflineTime || a == kRegRunMode || a == kRegControlWord;
}
