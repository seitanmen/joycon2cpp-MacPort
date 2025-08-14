#include "JoyConDecoder.h"
#include <cmath>
#include <algorithm>
#include <ViGEm/Client.h>
#include <ViGEm/Common.h>

#include <cstdint>
#include <vector>

/**
 * @brief 2つの8ビット符号なし整数（LSBとMSB）を1つの16ビット符号付き整数に変換
 * @param lsb 下位バイト
 * @param msb 上位バイト
 * @return 変換された16ビット符号付き整数
 */
int16_t to_signed_16(uint8_t lsb, uint8_t msb) {
    return static_cast<int16_t>((msb << 8) | lsb);
}

// 右Joy-Conのボタンマスク定義
constexpr uint32_t BUTTON_A_MASK_RIGHT = 0x000800;     // Aボタン
constexpr uint32_t BUTTON_B_MASK_RIGHT = 0x000200;     // Bボタン
constexpr uint32_t BUTTON_X_MASK_RIGHT = 0x000400;     // Xボタン
constexpr uint32_t BUTTON_Y_MASK_RIGHT = 0x000100;     // Yボタン
constexpr uint32_t BUTTON_PLUS_MASK_RIGHT = 0x000002;  // +ボタン
constexpr uint32_t BUTTON_R_MASK_RIGHT = 0x004000;     // Rボタン
constexpr uint32_t BUTTON_STICK_MASK_RIGHT = 0x000004; // スティック押し込みボタン

// 左Joy-Conのボタンマスク定義
constexpr uint32_t BUTTON_UP_MASK_LEFT = 0x000002;     // 十字キー上
constexpr uint32_t BUTTON_DOWN_MASK_LEFT = 0x000001;   // 十字キー下
constexpr uint32_t BUTTON_LEFT_MASK_LEFT = 0x000008;   // 十字キー左
constexpr uint32_t BUTTON_RIGHT_MASK_LEFT = 0x000004;  // 十字キー右
constexpr uint32_t BUTTON_MINUS_MASK_LEFT = 0x000100;  // -ボタン
constexpr uint32_t BUTTON_L_MASK_LEFT = 0x000040;      // Lボタン
constexpr uint32_t BUTTON_STICK_MASK_LEFT = 0x000800;  // スティック押し込みボタン

/**
 * @brief Joy-Conの生データからアナログスティックの値をデコード
 * @param buffer Joy-Conからの入力レポートのデータバッファ
 * @param isLeft 左のJoy-Conの場合はtrue、右の場合はfalse
 * @param upright Joy-Conを縦持ちしている場合はtrue、横持ちの場合はfalse
 * @return X軸とY軸の値のペア (-32767 から 32767 の範囲)
 */
static std::pair<int16_t, int16_t> decode_joystick(const std::vector<uint8_t>& buffer, bool isLeft, bool upright) {
    // バッファサイズが十分でない場合は、中央値(0, 0)を返す
    if (buffer.size() < 16) {
        return { 0, 0 };
    }

    // Joy-Conの左右に応じて、スティックデータの開始位置を決定
    const uint8_t* data = isLeft ? &buffer[10] : &buffer[13];

    // スティックのXY値は12ビットでエンコードされている
    // 3バイトのデータから12ビットずつ取り出す
    int x_raw = ((data[1] & 0x0F) << 8) | data[0];
    int y_raw = (data[2] << 4) | ((data[1] & 0xF0) >> 4);

    // 0-4095の範囲の値を、-1.0から1.0の浮動小数点数に正規化
    float x = (x_raw - 2048) / 2048.0f;
    float y = (y_raw - 2048) / 2048.0f;

    // 横持ち(Sideways)の場合、軸を回転
    if (!upright) {
        float tx = x, ty = y;
        x = isLeft ? -ty : ty;
        y = isLeft ? tx : -tx;
    }

    // デッドゾーンの設定
    const float deadzone = 0.08f;
    if (std::abs(x) < deadzone && std::abs(y) < deadzone) {
        return { 0, 0 }; // デッドゾーン内なら中央値を返す
    }

    // 値を少し増幅し、-1.0から1.0の範囲にクランプ
    // スティックを最大まで倒したときには確実に最大値を出力する
    x = std::clamp(x * 1.7f, -1.0f, 1.0f);
    y = std::clamp(y * 1.7f, -1.0f, 1.0f);

    // -1.0から1.0の値を、DS4コントローラーが期待する-32767から32767の範囲の16ビット整数に変換
    int16_t outX = static_cast<int16_t>(x * 32767);
    int16_t outY = static_cast<int16_t>(-y * 32767); // Y軸は反転

    return { outX, outY };
}

/**
 * @brief ジャイロセンサーのデータからマウスカーソルの座標をデコード
 * @param buffer Joy-Conからの入力レポートのデータバッファ
 * @return 画面上のX座標とY座標
 */
std::pair<uint16_t, uint16_t> DecodeMouseCoords(const std::vector<uint8_t>& buffer) {
    // バッファサイズが足りない場合はデフォルトの画面中央座標を返す
    if (buffer.size() < 0x18) return { 960, 471 };

    // バッファからジャイロのXとYの生データを読み込み、16ビット符号付き整数に変換
    int16_t raw_x = to_signed_16(buffer[0x10], buffer[0x11]);
    int16_t raw_y = to_signed_16(buffer[0x12], buffer[0x13]);

    // 生データを-1.0から1.0の範囲に正規化
    float norm_x = std::clamp(raw_x / 32767.0f, -1.0f, 1.0f);
    float norm_y = std::clamp(raw_y / 32767.0f, -1.0f, 1.0f);

    // 正規化された値を画面解像度（1920x943）にマッピング
    uint16_t x = static_cast<uint16_t>((norm_x + 1.0f) * 0.5f * 1920);
    uint16_t y = static_cast<uint16_t>((1.0f - (norm_y + 1.0f) * 0.5f) * 943);

    return { x, y };
}

/**
 * @brief DS4のタッチパッドデータをエンコード
 * @param touch エンコードされたデータを格納するDS4_TOUCH構造体への参照
 * @param trackingId タッチの追跡ID
 * @param x タッチのX座標
 * @param y タッチのY座標
 */
void EncodeDS4Touch(DS4_TOUCH& touch, uint8_t trackingId, uint16_t x, uint16_t y) {
    touch.bIsUpTrackingNum1 = trackingId & 0x7F; // 最初のタッチがアクティブかと、トラッキング番号
    touch.bTouchData1[0] = x & 0xFF;             // X座標の下位8ビット
    touch.bTouchData1[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4); // X座標の上位4ビットとY座標の下位4ビット
    touch.bTouchData1[2] = (y >> 4) & 0xFF;      // Y座標の上位8ビット
}

/**
 * @brief ボタンの状態からトリガーとショルダーボタンの状態をデコード
 * @param state ボタンの状態を示す32ビット整数
 * @param isLeft 左のJoy-Conか
 * @param upright 縦持ちか
 * @param leftTrigger 左トリガーの値（0 or 255）を格納する参照
 * @param rightTrigger 右トリガーの値（0 or 255）を格納する参照
 * @param leftShoulder 左ショルダーボタンが押されているかを格納する参照
 * @param rightShoulder 右ショルダーボタンが押されているかを格納する参照
 */
static void decode_triggers_shoulders(uint32_t state, bool isLeft, bool upright,
    BYTE& leftTrigger, BYTE& rightTrigger,
    bool& leftShoulder, bool& rightShoulder) {

    // ZL/ZRボタンはデジタルトリガーとして扱う
    // (押されていれば255、そうでなければ0
    leftTrigger = (state & 0x000080) ? 255 : 0;  // ZL (左Joy-Con)
    rightTrigger = (state & 0x008000) ? 255 : 0; // ZR (右Joy-Con)

    if (upright) { // 縦持ちの場合
        leftShoulder = (state & 0x000040) != 0;  // Lボタン
        rightShoulder = (state & 0x004000) != 0; // Rボタン
    }
    else { // 横持ちの場合
        // SL/SRボタンをL/Rショルダーとして割り当てる
        leftShoulder = (state & (isLeft ? 0x000020 : 0x002000)) != 0;
        rightShoulder = (state & (isLeft ? 0x000010 : 0x001000)) != 0;
    }
}

/**
 * @brief 単体のJoy-Conの入力データからDS4コントローラーのレポートを生成
 * @param buffer Joy-Conからの入力レポートのデータバッファ
 * @param side どちらのJoy-Conか (左 or 右)
 * @param orientation Joy-Conの向き (縦持ち or 横持ち)
 * @return 生成されたDS4レポート
 */
DS4_REPORT_EX GenerateDS4Report(const std::vector<uint8_t>& buffer, JoyConSide side, JoyConOrientation orientation) {
    DS4_REPORT_EX report{}; // レポート構造体をゼロで初期化
    // DS4レポートの必須フィールドを初期化するマクロ
    DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&report.Report));

    // バッファサイズが不十分な場合は、初期化されたレポートをそのまま返す
    if (buffer.size() < 0x3C) return report;

    bool isLeft = (side == JoyConSide::Left);
    bool upright = (orientation == JoyConOrientation::Upright);

    // ボタンデータのオフセットはJoy-Conの左右で異なる
    int btnOffset = isLeft ? 4 : 3;
    // 3バイトのボタンデータを1つの32ビット整数にまとめる
    uint32_t state = (buffer[btnOffset] << 16) | (buffer[btnOffset + 1] << 8) | buffer[btnOffset + 2];

    // アナログスティックの値をデコード
    auto [stickX, stickY] = decode_joystick(buffer, isLeft, upright);

    if (isLeft) {
        // 左Joy-Conのボタン処理
        bool up = (state & BUTTON_UP_MASK_LEFT) != 0;
        bool down = (state & BUTTON_DOWN_MASK_LEFT) != 0;
        bool left = (state & BUTTON_LEFT_MASK_LEFT) != 0;
        bool right = (state & BUTTON_RIGHT_MASK_LEFT) != 0;

        // 十字キーの状態をDS4のDPAD形式に変換
        uint8_t dpad = DS4_BUTTON_DPAD_NONE;
        if (up && left) dpad = DS4_BUTTON_DPAD_NORTHWEST;
        else if (up && right) dpad = DS4_BUTTON_DPAD_NORTHEAST;
        else if (down && left) dpad = DS4_BUTTON_DPAD_SOUTHWEST;
        else if (down && right) dpad = DS4_BUTTON_DPAD_SOUTHEAST;
        else if (up) dpad = DS4_BUTTON_DPAD_NORTH;
        else if (down) dpad = DS4_BUTTON_DPAD_SOUTH;
        else if (left) dpad = DS4_BUTTON_DPAD_WEST;
        else if (right) dpad = DS4_BUTTON_DPAD_EAST;

        DS4_SET_DPAD(reinterpret_cast<PDS4_REPORT>(&report.Report), static_cast<DS4_DPAD_DIRECTIONS>(dpad));

        // 他のボタンをDS4の対応するボタンにマッピング
        if (state & BUTTON_MINUS_MASK_LEFT)   report.Report.wButtons |= DS4_BUTTON_SHARE;
        if (state & BUTTON_L_MASK_LEFT)       report.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
        if (state & BUTTON_STICK_MASK_LEFT)   report.Report.wButtons |= DS4_BUTTON_THUMB_LEFT;
    }
    else {
        // 右Joy-Conのボタン処理
        DS4_SET_DPAD(reinterpret_cast<PDS4_REPORT>(&report.Report), DS4_BUTTON_DPAD_NONE); // 右Joy-ConにはDPADはない

        // ボタンをDS4の対応するボタンにマッピング
        if (state & BUTTON_A_MASK_RIGHT)      report.Report.wButtons |= DS4_BUTTON_CIRCLE;
        if (state & BUTTON_B_MASK_RIGHT)      report.Report.wButtons |= DS4_BUTTON_TRIANGLE;
        if (state & BUTTON_X_MASK_RIGHT)      report.Report.wButtons |= DS4_BUTTON_CROSS;
        if (state & BUTTON_Y_MASK_RIGHT)      report.Report.wButtons |= DS4_BUTTON_SQUARE;
        if (state & BUTTON_PLUS_MASK_RIGHT)   report.Report.wButtons |= DS4_BUTTON_OPTIONS;
        if (state & BUTTON_R_MASK_RIGHT)      report.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
        if (state & BUTTON_STICK_MASK_RIGHT)  report.Report.wButtons |= DS4_BUTTON_THUMB_RIGHT;
    }

    // ジャイロデータをマウス座標に変換し、DS4のタッチパッドデータとしてエンコード
    auto [touchX, touchY] = DecodeMouseCoords(buffer);
    report.Report.bTouchPacketsN = 1; // タッチパケット数
    report.Report.sCurrentTouch.bPacketCounter++; // パケットカウンターをインクリメント
    EncodeDS4Touch(report.Report.sCurrentTouch, 1, touchX, touchY);

    // トリガーとショルダーボタンの状態をデコード
    BYTE leftTrigger = 0, rightTrigger = 0;
    bool leftShoulder = false, rightShoulder = false;
    decode_triggers_shoulders(state, isLeft, upright, leftTrigger, rightTrigger, leftShoulder, rightShoulder);

    // デコードした値をレポートに設定
    if (leftShoulder)  report.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (rightShoulder) report.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (leftTrigger)   report.Report.wButtons |= DS4_BUTTON_TRIGGER_LEFT;
    if (rightTrigger)  report.Report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

    // スティックの値を0-255の範囲に変換して設定
    report.Report.bThumbLX = static_cast<BYTE>((stickX / 32767.0f) * 127 + 128);
    report.Report.bThumbLY = static_cast<BYTE>((stickY / 32767.0f) * 127 + 128);

    // 加速度センサーの値をレポートに設定
    report.Report.wAccelX = to_signed_16(buffer[0x30], buffer[0x31]);
    report.Report.wAccelY = to_signed_16(buffer[0x32], buffer[0x33]);
    report.Report.wAccelZ = to_signed_16(buffer[0x34], buffer[0x35]);

    // ジャイロセンサーの値をレポートに設定
    report.Report.wGyroX = to_signed_16(buffer[0x36], buffer[0x37]);
    report.Report.wGyroY = to_signed_16(buffer[0x38], buffer[0x39]);
    report.Report.wGyroZ = to_signed_16(buffer[0x3A], buffer[0x3B]);

    return report;
}

/**
 * @brief 左右両方のJoy-Conの入力データから1つのDS4コントローラーレポートを生成
 * @param leftBuffer 左Joy-Conからの入力レポートのデータバッファ
 * @param rightBuffer 右Joy-Conからの入力レポートのデータバッファ
 * @return 生成された結合後のDS4レポート
 */
DS4_REPORT_EX GenerateDualJoyConDS4Report(const std::vector<uint8_t>& leftBuffer, const std::vector<uint8_t>& rightBuffer)
{
    DS4_REPORT_EX report{};
    DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&report.Report));

    // どちらのバッファもサイズが不十分な場合は、初期化されたレポートを返す
    if (leftBuffer.size() < 0x3C && rightBuffer.size() < 0x3C) {
        return report;
    }

    // 左Joy-Conのレポートを生成
    DS4_REPORT_EX leftReport{};
    if (leftBuffer.size() >= 0x3C) {
        leftReport = GenerateDS4Report(leftBuffer, JoyConSide::Left, JoyConOrientation::Upright);
    }

    // 右Joy-Conのレポートを生成
    DS4_REPORT_EX rightReport{};
    if (rightBuffer.size() >= 0x3C) {
        rightReport = GenerateDS4Report(rightBuffer, JoyConSide::Right, JoyConOrientation::Upright);
    }

    // ボタンの状態を結合します。
    // 左のDPADはそのまま使い、他のボタンは両方のレポートのOをとる
    USHORT leftDpad = leftReport.Report.wButtons & 0xF; // DPAD部分のみ抽出
    USHORT leftButtonsNoDpad = leftReport.Report.wButtons & ~0xF; // DPAD以外
    USHORT rightButtonsNoDpad = rightReport.Report.wButtons & ~0xF; // DPAD以外

    USHORT combinedButtons = leftButtonsNoDpad | rightButtonsNoDpad;
    report.Report.wButtons = combinedButtons | leftDpad;

    // 特殊ボタン（PSボタンなど）の状態を結合
    report.Report.bSpecial = leftReport.Report.bSpecial | rightReport.Report.bSpecial;

    // タッチパッドは2点タッチとして扱う
    // 左Joy-Conのジャイロを1点目、右Joy-Conのジャイロを2点目とする
    auto [x1, y1] = DecodeMouseCoords(leftBuffer);
    auto [x2, y2] = DecodeMouseCoords(rightBuffer);

    report.Report.bTouchPacketsN = 1;
    report.Report.sCurrentTouch.bPacketCounter++;
    EncodeDS4Touch(report.Report.sCurrentTouch, 1, x1, y1); // 1点目
    report.Report.sCurrentTouch.bIsUpTrackingNum2 = 2; // 2点目のID
    report.Report.sCurrentTouch.bTouchData2[0] = x2 & 0xFF;
    report.Report.sCurrentTouch.bTouchData2[1] = ((x2 >> 8) & 0x0F) | ((y2 & 0x0F) << 4);
    report.Report.sCurrentTouch.bTouchData2[2] = (y2 >> 4) & 0xFF;

    // トリガーとショルダーボタンの状態を両方のJoy-Conから取得して結合
    uint32_t leftState = (leftBuffer[4] << 16) | (leftBuffer[5] << 8) | leftBuffer[6];
    uint32_t rightState = (rightBuffer[3] << 16) | (rightBuffer[4] << 8) | rightBuffer[5];

    BYTE lt = 0, rt = 0;
    bool ls = false, rs = false;

    // 左Joy-Conのトリガー/ショルダー
    decode_triggers_shoulders(leftState, true, true, lt, rt, ls, rs);
    report.Report.bTriggerL = lt;
    if (ls) report.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (lt) report.Report.wButtons |= DS4_BUTTON_TRIGGER_LEFT;

    // 右Joy-Conのトリガー/ショルダー
    decode_triggers_shoulders(rightState, false, true, lt, rt, ls, rs);
    report.Report.bTriggerR = rt;
    if (rs) report.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (rt) report.Report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

    // 左スティックは左Joy-Conから、右スティックは右Joy-Conから取得
    report.Report.bThumbLX = leftReport.Report.bThumbLX;
    report.Report.bThumbLY = leftReport.Report.bThumbLY;
    report.Report.bThumbRX = rightReport.Report.bThumbLX;
    report.Report.bThumbRY = rightReport.Report.bThumbLY;

    // モーションセンサーの値を結合するヘルパーラムダ
    auto combine_16 = [](int16_t a, int16_t b) -> int16_t {
        if (a == 0) return b;
        if (b == 0) return a;
        return static_cast<int16_t>((a / 2) + (b / 2));
    };

    // 加速度センサーの値を結合
    report.Report.wAccelX = combine_16(leftReport.Report.wAccelX, rightReport.Report.wAccelX);
    report.Report.wAccelY = combine_16(leftReport.Report.wAccelY, rightReport.Report.wAccelY);
    report.Report.wAccelZ = combine_16(leftReport.Report.wAccelZ, rightReport.Report.wAccelZ);

    // ジャイロセンサーの値を結合
    report.Report.wGyroX = combine_16(leftReport.Report.wGyroX, rightReport.Report.wGyroX);
    report.Report.wGyroY = combine_16(leftReport.Report.wGyroY, rightReport.Report.wGyroY);
    report.Report.wGyroZ = combine_16(leftReport.Report.wGyroZ, rightReport.Report.wGyroZ);

    return report;
}

/**
 * @brief Proコントローラーの生データからアナログスティックの値をデコード
 * @param data スティックデータへのポインタ
 * @return X軸とY軸の値をペアで返します (-32767 から 32767 の範囲)
 */
static std::pair<int16_t, int16_t> decode_pro_joystick(const uint8_t* data)
{
    // データが無効な場合は中央値を返す
    if (!data) {
        return { 0, 0 };
    }

    // Joy-Conと同様に12ビットのデータをデコード
    int x_raw = ((data[1] & 0x0F) << 8) | data[0];
    int y_raw = (data[2] << 4) | ((data[1] & 0xF0) >> 4);

    // -1.0から1.0に正規化
    float x = (x_raw - 2048) / 2048.0f;
    float y = (y_raw - 2048) / 2048.0f;

    // デッドゾーン処理
    constexpr float deadzone = 0.08f;
    if (std::abs(x) < deadzone && std::abs(y) < deadzone) {
        return { 0, 0 };
    }

    // 値の増幅とクランプ
    x = std::clamp(x * 1.7f, -1.0f, 1.0f);
    y = std::clamp(y * 1.7f, -1.0f, 1.0f);

    // 16ビット整数に変換
    int16_t outX = static_cast<int16_t>(x * 32767);
    int16_t outY = static_cast<int16_t>(y * 32767);

    return { outX, outY };
}

// Proコントローラー/NSO GCコントローラーのボタンマスク定義
constexpr uint64_t BUTTON_A_MASK = 0x000800000000;
constexpr uint64_t BUTTON_B_MASK = 0x000400000000;
constexpr uint64_t BUTTON_X_MASK = 0x000200000000;
constexpr uint64_t BUTTON_Y_MASK = 0x000100000000;
constexpr uint64_t BUTTON_R_SHOULDER = 0x004000000000;
constexpr uint64_t BUTTON_L_SHOULDER = 0x000000400000;
constexpr uint64_t BUTTON_DPAD_UP = 0x000000020000;
constexpr uint64_t BUTTON_DPAD_RIGHT = 0x000000040000;
constexpr uint64_t BUTTON_DPAD_DOWN = 0x000000010000;
constexpr uint64_t BUTTON_DPAD_LEFT = 0x000000080000;
constexpr uint64_t BUTTON_GUIDE = 0x000010000000; // Homeボタン
constexpr uint64_t BUTTON_BACK = 0x000001000000;  // - ボタン (キャプチャボタン)
constexpr uint64_t BUTTON_START = 0x000002000000; // + ボタン
constexpr uint64_t BUTTON_R_THUMB = 0x000004000000; // 右スティック押し込み
constexpr uint64_t BUTTON_L_THUMB = 0x000008000000; // 左スティック押し込み

constexpr uint64_t TRIGGER_LT_MASK = 0x000000800000; // ZLトリガー
constexpr uint64_t TRIGGER_RT_MASK = 0x008000000000; // ZRトリガー

/**
 * @brief Proコントローラーの入力データからDS4コントローラーのレポートを生成
 * @param buffer Proコントローラーからの入力レポートのデータバッファ
 * @return 生成されたDS4レポート
 */
DS4_REPORT_EX GenerateProControllerReport(const std::vector<uint8_t>& buffer)
{
    DS4_REPORT_EX report{};
    DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&report.Report));

    if (buffer.size() < 0x3C) {
        return report;
    }

    // 6バイトのボタンデータを1つの64ビット整数にまとめる
    uint64_t state = 0;
    for (int i = 3; i <= 8; ++i) {
        state = (state << 8) | buffer[i];
    }

    // 各ボタンのマスクを使って状態を確認し、DS4のボタンにマッピング
    if (state & BUTTON_A_MASK)        report.Report.wButtons |= DS4_BUTTON_CIRCLE;
    if (state & BUTTON_B_MASK)        report.Report.wButtons |= DS4_BUTTON_TRIANGLE;
    if (state & BUTTON_X_MASK)        report.Report.wButtons |= DS4_BUTTON_CROSS;
    if (state & BUTTON_Y_MASK)        report.Report.wButtons |= DS4_BUTTON_SQUARE;
    if (state & BUTTON_L_SHOULDER)    report.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (state & BUTTON_R_SHOULDER)    report.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (state & BUTTON_L_THUMB)       report.Report.wButtons |= DS4_BUTTON_THUMB_LEFT;
    if (state & BUTTON_R_THUMB)       report.Report.wButtons |= DS4_BUTTON_THUMB_RIGHT;
    if (state & BUTTON_BACK)          report.Report.wButtons |= DS4_BUTTON_SHARE;
    if (state & BUTTON_START)         report.Report.wButtons |= DS4_BUTTON_OPTIONS;
    if (state & BUTTON_GUIDE)         report.Report.bSpecial |= DS4_SPECIAL_BUTTON_PS;

    // DPADの状態をデコード
    bool up = (state & BUTTON_DPAD_UP) != 0;
    bool down = (state & BUTTON_DPAD_DOWN) != 0;
    bool left = (state & BUTTON_DPAD_LEFT) != 0;
    bool right = (state & BUTTON_DPAD_RIGHT) != 0;

    uint8_t dpad = DS4_BUTTON_DPAD_NONE;
    if (up && left) dpad = DS4_BUTTON_DPAD_NORTHWEST;
    else if (up && right) dpad = DS4_BUTTON_DPAD_NORTHEAST;
    else if (down && left) dpad = DS4_BUTTON_DPAD_SOUTHWEST;
    else if (down && right) dpad = DS4_BUTTON_DPAD_SOUTHEAST;
    else if (up) dpad = DS4_BUTTON_DPAD_NORTH;
    else if (down) dpad = DS4_BUTTON_DPAD_SOUTH;
    else if (left) dpad = DS4_BUTTON_DPAD_WEST;
    else if (right) dpad = DS4_BUTTON_DPAD_EAST;

    DS4_SET_DPAD(reinterpret_cast<PDS4_REPORT>(&report.Report), static_cast<DS4_DPAD_DIRECTIONS>(dpad));

    // トリガーの状態を設定 (Proコンのトリガーはデジタル)
    report.Report.bTriggerL = (state & TRIGGER_LT_MASK) ? 255 : 0;
    report.Report.bTriggerR = (state & TRIGGER_RT_MASK) ? 255 : 0;

    // 左右のスティック値をデコード
    auto [lx, ly] = decode_pro_joystick(&buffer[10]);
    ly = -ly; // Y軸を反転
    auto [rx, ry] = decode_pro_joystick(&buffer[13]);
    ry = -ry; // Y軸を反転

    // スティック値を0-255の範囲に変換して設定
    report.Report.bThumbLX = static_cast<uint8_t>((lx / 32767.0f) * 127 + 128);
    report.Report.bThumbLY = static_cast<uint8_t>((ly / 32767.0f) * 127 + 128);
    report.Report.bThumbRX = static_cast<uint8_t>((rx / 32767.0f) * 127 + 128);
    report.Report.bThumbRY = static_cast<uint8_t>((ry / 32767.0f) * 127 + 128);

    // 加速度センサーとジャイロセンサーの値を設定
    report.Report.wAccelX = to_signed_16(buffer[0x30], buffer[0x31]);
    report.Report.wAccelY = to_signed_16(buffer[0x32], buffer[0x33]);
    report.Report.wAccelZ = to_signed_16(buffer[0x34], buffer[0x35]);
    report.Report.wGyroX = to_signed_16(buffer[0x36], buffer[0x37]);
    report.Report.wGyroY = to_signed_16(buffer[0x38], buffer[0x39]);
    report.Report.wGyroZ = to_signed_16(buffer[0x3A], buffer[0x3B]);

    return report;
}

/**
 * @brief NSO(Nintendo Switch Online)用ゲームキューブコントローラーの入力データからDS4レポートを生成
 * @param buffer NSO GCコントローラーからの入力レポートのデータバッファ
 * @return 生成されたDS4レポート
 */
DS4_REPORT_EX GenerateNSOGCReport(const std::vector<uint8_t>& buffer)
{
    DS4_REPORT_EX report{};
    DS4_REPORT_INIT(reinterpret_cast<PDS4_REPORT>(&report.Report));

    if (buffer.size() < 0x3C) {
        return report;
    }

    // ボタンデータを64ビット整数にまとめる
    uint64_t state = 0;
    for (int i = 3; i <= 8; ++i) {
        state = (state << 8) | buffer[i];
    }

    // ボタンマッピング
    if (state & BUTTON_A_MASK)        report.Report.wButtons |= DS4_BUTTON_CIRCLE;
    if (state & BUTTON_B_MASK)        report.Report.wButtons |= DS4_BUTTON_TRIANGLE;
    if (state & BUTTON_X_MASK)        report.Report.wButtons |= DS4_BUTTON_CROSS;
    if (state & BUTTON_Y_MASK)        report.Report.wButtons |= DS4_BUTTON_SQUARE;
    if (state & BUTTON_L_SHOULDER)    report.Report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (state & BUTTON_R_SHOULDER)    report.Report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
    if (state & BUTTON_L_THUMB)       report.Report.wButtons |= DS4_BUTTON_THUMB_LEFT;
    if (state & BUTTON_R_THUMB)       report.Report.wButtons |= DS4_BUTTON_THUMB_RIGHT;
    if (state & BUTTON_BACK)          report.Report.wButtons |= DS4_BUTTON_SHARE;
    if (state & BUTTON_START)         report.Report.wButtons |= DS4_BUTTON_OPTIONS;
    if (state & BUTTON_GUIDE)         report.Report.bSpecial |= DS4_SPECIAL_BUTTON_PS;

    // DPADデコード
    bool up = (state & BUTTON_DPAD_UP) != 0;
    bool down = (state & BUTTON_DPAD_DOWN) != 0;
    bool left = (state & BUTTON_DPAD_LEFT) != 0;
    bool right = (state & BUTTON_DPAD_RIGHT) != 0;

    uint8_t dpad = DS4_BUTTON_DPAD_NONE;
    if (up && left) dpad = DS4_BUTTON_DPAD_NORTHWEST;
    else if (up && right) dpad = DS4_BUTTON_DPAD_NORTHEAST;
    else if (down && left) dpad = DS4_BUTTON_DPAD_SOUTHWEST;
    else if (down && right) dpad = DS4_BUTTON_DPAD_SOUTHEAST;
    else if (up) dpad = DS4_BUTTON_DPAD_NORTH;
    else if (down) dpad = DS4_BUTTON_DPAD_SOUTH;
    else if (left) dpad = DS4_BUTTON_DPAD_WEST;
    else if (right) dpad = DS4_BUTTON_DPAD_EAST;

    DS4_SET_DPAD(reinterpret_cast<PDS4_REPORT>(&report.Report), static_cast<DS4_DPAD_DIRECTIONS>(dpad));

    // トリガー設定
    report.Report.bTriggerL = (state & TRIGGER_LT_MASK) ? 255 : 0;
    report.Report.bTriggerR = (state & TRIGGER_RT_MASK) ? 255 : 0;

    // スティック値のデコードと設定
    auto [lx, ly] = decode_pro_joystick(&buffer[10]);
    ly = -ly;
    auto [rx, ry] = decode_pro_joystick(&buffer[13]);
    ry = -ry;

    report.Report.bThumbLX = static_cast<uint8_t>((lx / 32767.0f) * 127 + 128);
    report.Report.bThumbLY = static_cast<uint8_t>((ly / 32767.0f) * 127 + 128);
    report.Report.bThumbRX = static_cast<uint8_t>((rx / 32767.0f) * 127 + 128);
    report.Report.bThumbRY = static_cast<uint8_t>((ry / 32767.0f) * 127 + 128);

    // モーションセンサーの値の設定
    report.Report.wAccelX = to_signed_16(buffer[0x30], buffer[0x31]);
    report.Report.wAccelY = to_signed_16(buffer[0x32], buffer[0x33]);
    report.Report.wAccelZ = to_signed_16(buffer[0x34], buffer[0x35]);
    report.Report.wGyroX = to_signed_16(buffer[0x36], buffer[0x37]);
    report.Report.wGyroY = to_signed_16(buffer[0x38], buffer[0x39]);
    report.Report.wGyroZ = to_signed_16(buffer[0x3A], buffer[0x3B]);

    return report;
}