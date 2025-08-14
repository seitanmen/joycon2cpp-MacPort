#pragma once

#include <vector>
#include <utility>
#include <cstdint>
#include <Windows.h>
#include <ViGEm/Client.h>

/**
 * @enum JoyConSide
 * @brief Joy-Conが左か右かを示す列挙型
 */
enum class JoyConSide {
    Left,  // 左のJoy-Con
    Right  // 右のJoy-Con
};

/**
 * @enum JoyConOrientation
 * @brief Joy-Conの持ち方を示す列挙型
 */
enum class JoyConOrientation {
    Upright,  // 縦持ち
    Sideways  // 横持ち
};

/**
 * @struct StickData
 * @brief アナログスティックのデータを保持する構造体
 */
struct StickData {
    int16_t x;  // X軸の値 (-32767 to 32767)
    int16_t y;  // Y軸の値 (-32767 to 32767)
    BYTE rx; // 0-255に変換されたX軸の値
    BYTE ry; // 0-255に変換されたY軸の値
};

/**
 * @struct MotionData
 * @brief モーションセンサー（ジャイロ・加速度）のデータを保持する構造体
 */
struct MotionData {
    SHORT gyroX, gyroY, gyroZ;   // ジャイロセンサーの各軸の値
    SHORT accelX, accelY, accelZ; // 加速度センサーの各軸の値
};


/**
 * @brief 単体のJoy-Conの入力データからDS4コントローラーのレポートを生成
 * @param buffer Joy-Conからの生データ
 * @param side Joy-Conが左か右か
 * @param orientation Joy-Conの持ち方
 * @return 生成されたDS4レポート。
 */
DS4_REPORT_EX GenerateDS4Report(
    const std::vector<uint8_t>& buffer,
    JoyConSide side,
    JoyConOrientation orientation
);

/**
 * @brief 左右両方のJoy-Conの入力データから1つのDS4コントローラーレポートを生成
 * @param leftBuffer 左Joy-Conからの生データ
 * @param rightBuffer 右Joy-Conからの生データ
 * @return 結合されたDS4レポート
 */
DS4_REPORT_EX GenerateDualJoyConDS4Report(
    const std::vector<uint8_t>& leftBuffer,
    const std::vector<uint8_t>& rightBuffer
);

/**
 * @brief Proコントローラーの入力データからDS4コントローラーのレポートを生成
 * @param buffer Proコントローラーからの生データ
 *return 生成されたDS4レポート
 */
DS4_REPORT_EX GenerateProControllerReport(
    const std::vector<uint8_t>& buffer
);

/**
 * @brief NSOゲームキューブコントローラーの入力データからDS4コントローラーのレポートを生成
 * @param buffer NSO GCコントローラーからの生データ
 * @return 生成されたDS4レポート
 */
DS4_REPORT_EX GenerateNSOGCReport(
    const std::vector<uint8_t>& buffer
);


/**
 * @brief 生データからボタンの状態を抽出
 * @param buffer Joy-Conからの生データ
 * @return ボタンの状態を表す32ビット整数
 */
uint32_t ExtractButtonState(
    const std::vector<uint8_t>& buffer
);

/**
 * @brief 生データからアナログスティックの値をデコード
 * @param buffer Joy-Conからの生データ
 * @param side Joy-Conが左か右か
 * @param orientation Joy-Conの持ち方
 * @return デコードされたスティックデータ
 */
StickData DecodeJoystick(
    const std::vector<uint8_t>& buffer,
    JoyConSide side,
    JoyConOrientation orientation
);

/**
 * @brief 生データからモーションセンサーの値をデコード
 * @param buffer Joy-Conからの生データ
 * @return デコードされたモーションデータ
 */
MotionData DecodeMotion(
    const std::vector<uint8_t>& buffer
);


