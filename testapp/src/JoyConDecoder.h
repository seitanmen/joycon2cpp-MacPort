#pragma once
#include <vector>
#include <utility>
#include <cstdint>
#include <Windows.h>
#include <ViGEm/Client.h>

enum class JoyConSide { Left, Right };
enum class JoyConOrientation { Upright, Sideways };

struct StickData {
    int16_t x;
    int16_t y;
    BYTE rx;
    BYTE ry;
};

struct MotionData {
    SHORT gyroX, gyroY, gyroZ;
    SHORT accelX, accelY, accelZ;
};

// Pass side and orientation explicitly now:
DS4_REPORT_EX GenerateDS4Report(const std::vector<uint8_t>& buffer, JoyConSide side, JoyConOrientation orientation);
DS4_REPORT_EX GenerateDualJoyConDS4Report(const std::vector<uint8_t>& leftBuffer, const std::vector<uint8_t>& rightBuffer);
DS4_REPORT_EX GenerateProControllerReport(const std::vector<uint8_t>& buffer);
DS4_REPORT_EX GenerateNSOGCReport(const std::vector<uint8_t>& buffer);

uint32_t ExtractButtonState(const std::vector<uint8_t>& buffer);
StickData DecodeJoystick(const std::vector<uint8_t>& buffer, JoyConSide side, JoyConOrientation orientation);
MotionData DecodeMotion(const std::vector<uint8_t>& buffer);
