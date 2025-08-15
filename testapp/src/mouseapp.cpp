#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#pragma comment(lib, "setupapi.lib") // SetupAPIライブラリをリンク（デバイス情報取得などに使用）
#include <Windows.h>

#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <iomanip>
#include <array>
#include <optional>
#include <chrono>
#include <fstream>
#include <string>

#include "JoyConDecoder.h"

#include <ViGEm/Client.h>  // ViGEm (Virtual Gamepad Emulation Framework) クライアント
#include <ViGEm/Common.h>  // ViGEm の共通定義

// マウス感度のグローバル変数
double mouse_sensitivity = 1.0;

/**
 * @brief mouse_sensitivity.txt からマウス感度を読み込む
 */
void LoadMouseSensitivity() {
    std::ifstream ifs("mouse_sensitivity.txt");
    if (ifs.is_open()) {
        std::string line;
        if (std::getline(ifs, line)) {
            try {
                mouse_sensitivity = std::stod(line);
                std::wcout << L"Mouse sensitivity set to: " << mouse_sensitivity << std::endl;
            }
            catch (const std::invalid_argument&) {
                std::wcerr << L"Invalid format in mouse_sensitivity.txt. Using default value 1.0." << std::endl;
            }
            catch (const std::out_of_range&) {
                std::wcerr << L"Value in mouse_sensitivity.txt is out of range. Using default value 1.0." << std::endl;
            }
        }
    }
    else {
        std::wcout << L"mouse_sensitivity.txt not found. Using default sensitivity 1.0." << std::endl;
    }
}

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation;

// 定数定義

// Joy-ConのBluetooth Manufacturer ID (Nintendo)
constexpr uint16_t JOYCON_MANUFACTURER_ID = 1363;
// Joy-ConのAdvertisementパケットに含まれる製造元データのプレフィックス
const std::vector<uint8_t> JOYCON_MANUFACTURER_PREFIX = { 0x01, 0x00, 0x03, 0x7E };
// Joy-ConのGATTサービスで入力レポートを受け取るためのキャラクタリスティックUUID
const wchar_t* INPUT_REPORT_UUID = L"ab7de9be-89fe-49ad-828f-118f09df7fd2";
// Joy-Conにコマンドを送信するためのキャラクタリスティックUUID
const wchar_t* WRITE_COMMAND_UUID = L"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";

// ViGEmクライアントのグローバルポインタ
PVIGEM_CLIENT vigem_client = nullptr;

/**
 * @brief ViGEmクライアントを初期化し、バスに接続
 */
void InitializeViGEm()
{
    // 既に初期化済みの場合は何もしない
    if (vigem_client != nullptr)
        return;

    // ViGEmクライアントをアロケート（メモリ確保）
    vigem_client = vigem_alloc();
    if (vigem_client == nullptr)
    {
        std::wcerr << L"Failed to allocate ViGEm client.\n";
        exit(1); // エラーで終了
    }

    // ViGEmバスに接続
    auto ret = vigem_connect(vigem_client);
    if (!VIGEM_SUCCESS(ret)) // 接続失敗
    {
        std::wcerr << L"Failed to connect to ViGEm bus: 0x" << std::hex << ret << L"\n";
        exit(1); // エラーで終了
    }

    std::wcout << L"ViGEm client initialized and connected.\n";
}

/**
 * @brief 受信した生データを16進数でコンソールに出力
 * @param buffer 受信したデータのバッファ
 */
void PrintRawNotification(const std::vector<uint8_t>& buffer)
{
    std::cout << "[Raw Notification] ";
    for (auto b : buffer) {
        printf("%02X ", b);
    }
    std::cout << std::endl;
}

/**
 * @brief DS4レポートの状態をコンソールに表示
 * @param report 表示するDS4レポート
 * @-
 */
void PrintDS4ReportState(const DS4_REPORT_EX& report) {
    // カーソルを行頭に戻すことで、出力を同じ行に上書きする
    std::wcout << L"\r";

    // ボタンの状態
    std::wcout << L"Buttons: ";

    if (report.Report.wButtons & DS4_BUTTON_SQUARE)         std::wcout << L"Y ";
    if (report.Report.wButtons & DS4_BUTTON_CROSS)          std::wcout << L"B ";
    if (report.Report.wButtons & DS4_BUTTON_CIRCLE)         std::wcout << L"A ";
    if (report.Report.wButtons & DS4_BUTTON_TRIANGLE)       std::wcout << L"X ";
    if (report.Report.wButtons & DS4_BUTTON_SHOULDER_LEFT)  std::wcout << L"L1 ";
    if (report.Report.wButtons & DS4_BUTTON_SHOULDER_RIGHT) std::wcout << L"R1 ";
    if (report.Report.wButtons & DS4_BUTTON_TRIGGER_LEFT)   std::wcout << L"L2 ";
    if (report.Report.wButtons & DS4_BUTTON_TRIGGER_RIGHT)  std::wcout << L"R2 ";
    if (report.Report.wButtons & DS4_BUTTON_SHARE)          std::wcout << L"SHARE ";
    if (report.Report.wButtons & DS4_BUTTON_OPTIONS)        std::wcout << L"OPTIONS ";
    if (report.Report.wButtons & DS4_BUTTON_THUMB_LEFT)     std::wcout << L"L3 ";
    if (report.Report.wButtons & DS4_BUTTON_THUMB_RIGHT)    std::wcout << L"R3 ";
    if (report.Report.bSpecial & DS4_SPECIAL_BUTTON_PS)     std::wcout << L"PS ";

    // 十字キーの状態
    switch (report.Report.wButtons & 0xF) {
        case DS4_BUTTON_DPAD_NORTHWEST: std::wcout << L"NW "; break;
        case DS4_BUTTON_DPAD_NORTHEAST: std::wcout << L"NE "; break;
        case DS4_BUTTON_DPAD_SOUTHWEST: std::wcout << L"SW "; break;
        case DS4_BUTTON_DPAD_SOUTHEAST: std::wcout << L"SH "; break;
        case DS4_BUTTON_DPAD_NORTH:     std::wcout << L"N "; break;
        case DS4_BUTTON_DPAD_SOUTH:     std::wcout << L"S "; break;
        case DS4_BUTTON_DPAD_WEST:      std::wcout << L"W "; break;
        case DS4_BUTTON_DPAD_EAST:      std::wcout << L"E "; break;
        default:                        std::wcout << L"  "; break;
    }

    std::wcout << report.Report.wButtons << L"  ";

    // アナログスティックの値 (0-255)
    std::wcout << L"| LX: " << std::setw(3) << static_cast<int>(report.Report.bThumbLX)
               << L" LY: " << std::setw(3) << static_cast<int>(report.Report.bThumbLY)
               << L" | RX: " << std::setw(3) << static_cast<int>(report.Report.bThumbRX)
               << L" RY: " << std::setw(3) << static_cast<int>(report.Report.bThumbRY);

    // トリガーの値 (0-255)
    std::wcout << L" | L2: " << std::setw(3) << static_cast<int>(report.Report.bTriggerL)
               << L" R2: " << std::setw(3) << static_cast<int>(report.Report.bTriggerR);

    // タッチパッド（マウス）の座標
    if (report.Report.bTouchPacketsN > 0) {
        // 1点目のタッチ情報
        uint16_t x1 = report.Report.sCurrentTouch.bTouchData1[0] | ((report.Report.sCurrentTouch.bTouchData1[1] & 0x0F) << 8);
        uint16_t y1 = ((report.Report.sCurrentTouch.bTouchData1[1] & 0xF0) >> 4) | (report.Report.sCurrentTouch.bTouchData1[2] << 4);
        // bIsUpTrackingNum1の最上位ビットが0のとき、タッチされている
        if ((report.Report.sCurrentTouch.bIsUpTrackingNum1 & 0x80) == 0) {
            std::wcout << L" | Touch1: (" << std::setw(4) << x1 << L", " << std::setw(4) << y1 << L")";
        }

        // 2点目のタッチ情報 (Dual Joy-Con用)
        uint16_t x2 = report.Report.sCurrentTouch.bTouchData2[0] | ((report.Report.sCurrentTouch.bTouchData2[1] & 0x0F) << 8);
        uint16_t y2 = ((report.Report.sCurrentTouch.bTouchData2[1] & 0xF0) >> 4) | (report.Report.sCurrentTouch.bTouchData2[2] << 4);
        // bIsUpTrackingNum2の最上位ビットが0のとき、タッチされている
        if ((report.Report.sCurrentTouch.bIsUpTrackingNum2 & 0x80) == 0) {
            std::wcout << L" | Touch2: (" << std::setw(4) << x2 << L", " << std::setw(4) << y2 << L")";
        }
    }

    std::wcout << L"          ";
}


static bool prevLeftButtonState = false;
static bool prevRightButtonState = false;
static bool prevMiddleButtonState = false;
static std::optional<std::pair<uint16_t, uint16_t>> prevMouseState = std::nullopt;

void OperateMouse(const DS4_REPORT_EX& report, const JoyConSide& joyconSide)
{
    static auto last_call_time = std::chrono::system_clock::now();
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_call_time).count();
    std::wcout << L"\r[DEBUG] OperateMouse called after " << std::setw(4) << duration << L" ms. ";
    last_call_time = now;

    std::vector<INPUT> inputs;

    // マウスの左ボタンの状態を確認 (Joy-ConのZL)
    uint16_t leftButtonMask = joyconSide == JoyConSide::Left ? DS4_BUTTON_TRIGGER_LEFT : DS4_BUTTON_TRIGGER_RIGHT;
    bool currentLeftButtonState = (report.Report.wButtons & leftButtonMask);
    if (currentLeftButtonState != prevLeftButtonState)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = currentLeftButtonState ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        inputs.push_back(input);
        prevLeftButtonState = currentLeftButtonState;
    }

    // マウスの右ボタンの状態を確認 (Joy-ConのL)
    uint16_t rightButtonMask = joyconSide == JoyConSide::Left ? DS4_BUTTON_SHOULDER_LEFT: DS4_BUTTON_SHOULDER_RIGHT;
    bool currentRightButtonState = (report.Report.wButtons & rightButtonMask);
    if (currentRightButtonState != prevRightButtonState)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = currentRightButtonState ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        inputs.push_back(input);
        prevRightButtonState = currentRightButtonState;
    }

    // マウスの中ボタンの状態を確認 (Joy-Conのアナログスティックボタン)
    uint16_t middleButtonMask = joyconSide == JoyConSide::Left ? DS4_BUTTON_THUMB_LEFT: DS4_BUTTON_THUMB_RIGHT;
    bool currentMiddleButtonState = (report.Report.wButtons & middleButtonMask);
    if (currentMiddleButtonState != prevMiddleButtonState)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = currentMiddleButtonState ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        inputs.push_back(input);
        prevMiddleButtonState = currentMiddleButtonState;
    }

    // マウスカーソルの操作
    uint16_t x = report.Report.sCurrentTouch.bTouchData1[0] | ((report.Report.sCurrentTouch.bTouchData1[1] & 0x0F) << 8);
    uint16_t y = ((report.Report.sCurrentTouch.bTouchData1[1] & 0xF0) >> 4) | (report.Report.sCurrentTouch.bTouchData1[2] << 4);
    if(prevMouseState)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>((x - prevMouseState.value().first) * mouse_sensitivity);
        input.mi.dy = static_cast<LONG>((prevMouseState.value().second - y) * mouse_sensitivity);
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        inputs.push_back(input);
        prevMouseState = {x, y};
    }
    else
        prevMouseState = {x, y};

    // マウススクロールの操作
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.mouseData = 128 - static_cast<int>(report.Report.bThumbLY);
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputs.push_back(input);
    }


    if (!inputs.empty())
    {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

/**
 * @brief Joy-Conに初期化用のカスタムコマンドを送信
 * @param characteristic コマンド書き込み用のGATTキャラクタリスティック
 * @note Joy-Conが安定してレポートを送信するために必要
 */
void SendCustomCommands(GattCharacteristic const& characteristic)
{
    // 送信するコマンドのリスト
    std::vector<std::vector<uint8_t>> commands = {
        { 0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },
        { 0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 }
    };

    for (const auto& cmd : commands)
    {
        auto writer = DataWriter();
        writer.WriteBytes(cmd);
        IBuffer buffer = writer.DetachBuffer();

        // コマンドを非同期で書き込み、完了を待つ
        auto status = characteristic.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse).get();

        if (status == GattCommunicationStatus::Success)
        {
            std::wcout << L"Command sent successfully.\n";
        }
        else
        {
            std::wcout << L"Failed to send command.\n";
        }

        // コマンド間に短い待機時間を入れる
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

/**
 * @struct ConnectedJoyCon
 * @brief 接続されたJoy-Conのデバイス情報とGATTキャラクタリスティックを保持する構造体
 */
struct ConnectedJoyCon {
    BluetoothLEDevice device = nullptr;       // Bluetoothデバイスオブジェクト
    GattCharacteristic inputChar = nullptr; // 入力レポート用キャラクタリスティック
    GattCharacteristic writeChar = nullptr; // コマンド書き込み用キャラクタリスティック
};

/**
 * @brief Joy-ConからのBluetooth LEアドバタイズを待ち受け、接続を試みる
 * @param prompt ユーザーに表示するプロンプトメッセージ
 * @return 接続に成功したJoy-Conの情報 (ConnectedJoyCon)
 */
ConnectedJoyCon WaitForJoyCon(const std::wstring& prompt)
{
    std::wcout << prompt << L"\n";

    ConnectedJoyCon cj{}; // 戻り値用の構造体

    BluetoothLEDevice device = nullptr;
    bool connected = false;

    // Bluetooth LEアドバタイズメントウォッチャーを作成
    BluetoothLEAdvertisementWatcher watcher;

    // 同期用のミューテックスと条件変数
    std::mutex mtx;
    std::condition_variable cv;

    // アドバタイズメント受信時のイベントハンドラ（ラムダ式）
    watcher.Received([&](auto const&, auto const& args)
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (connected) return; // 既に接続済みの場合は無視

            // アドバタイズメントから製造元データを取得
            auto mfg = args.Advertisement().ManufacturerData();
            for (uint32_t i = 0; i < mfg.Size(); i++)
            {
                auto section = mfg.GetAt(i);
                // 会社IDが任天堂(1363)でなければスキップ
                if (section.CompanyId() != JOYCON_MANUFACTURER_ID) continue;

                // 製造元データがJoy-Conのプレフィックスと一致するか確認
                auto reader = DataReader::FromBuffer(section.Data());
                std::vector<uint8_t> data(reader.UnconsumedBufferLength());
                reader.ReadBytes(data);
                if (data.size() >= JOYCON_MANUFACTURER_PREFIX.size() &&
                    std::equal(JOYCON_MANUFACTURER_PREFIX.begin(), JOYCON_MANUFACTURER_PREFIX.end(), data.begin()))
                {
                    // 一致したら、そのデバイスに接続を試みる
                    device = BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress()).get();
                    if (!device) return; // 接続失敗

                    connected = true; // 接続成功フラグを立てる
                    watcher.Stop();   // スキャンを停止
                    cv.notify_one();  // 待機中のメインスレッドに通知
                    return;
                }
            }
        });

    watcher.ScanningMode(BluetoothLEScanningMode::Active); // アクティブスキャンモード
    watcher.Start(); // スキャン開始

    std::wcout << L"Scanning for Joy-Con... (Waiting up to 30 seconds)\n";

    {
        std::unique_lock<std::mutex> lock(mtx);
        // 30秒のタイムアウト付きで、接続が成功するのを待つ
        if (!cv.wait_for(lock, std::chrono::seconds(30), [&]() { return connected; }))
        {
            watcher.Stop();
            std::wcerr << L"Timeout: Joy-Con not found.\n";
            exit(1);
        }
    }

    cj.device = device;

    // GATTサービスを取得
    auto servicesResult = device.GetGattServicesAsync().get();
    if (servicesResult.Status() != GattCommunicationStatus::Success)
    {
        std::wcerr << L"Failed to get GATT services.\n";
        exit(1);
    }

    // 全てのサービスをループして、目的のキャラクタリスティックを探す
    for (auto service : servicesResult.Services())
    {
        auto charsResult = service.GetCharacteristicsAsync().get();
        if (charsResult.Status() != GattCommunicationStatus::Success) continue;
        for (auto characteristic : charsResult.Characteristics())
        {
            if (characteristic.Uuid() == guid(INPUT_REPORT_UUID))
                cj.inputChar = characteristic; // 入力用
            else if (characteristic.Uuid() == guid(WRITE_COMMAND_UUID))
                cj.writeChar = characteristic; // 書き込み用
        }
    }

    return cj;
}

/**
 * @enum ControllerType
 * @brief ユーザーが選択するコントローラーの種類
 */
enum ControllerType {
    SingleJoyCon = 1,    // Joy-Con単体
    DualJoyCon = 2,      // Joy-Con両手持ち
    ProController = 3,   // Proコントローラー
    NSOGCController = 4  // NSOゲームキューブコントローラー
};

/**
 * @struct PlayerConfig
 * @brief プレイヤーごとの設定を保持する構造体。
 */
struct PlayerConfig {
    ControllerType controllerType;
    JoyConSide joyconSide;
    JoyConOrientation joyconOrientation;
};


// 単体Joy-Conプレイヤー用
struct SingleJoyConPlayer {
    ConnectedJoyCon joycon;         // 接続情報
    PVIGEM_TARGET ds4Controller;    // 仮想DS4コントローラー
    JoyConSide side;                // 左右どちらか
    JoyConOrientation orientation;  // 持ち方
};


/**
 * @brief メイン関数
 */
int main()
{
    // マウス感度をファイルから読み込み
    LoadMouseSensitivity();

    // WinRT (COM) を使用するためにアパートメントを初期化
    init_apartment();
    // ViGEmを初期化
    InitializeViGEm();

    // L/Rの選択
    std::wstring line;
    JoyConSide joyconSide;
    while (true) {
        std::wcout << L"  Which side? (L=Left, R=Right): ";
        std::getline(std::wcin, line);
        if (line == L"L" || line == L"R" || line == L"l" || line == L"r") {
            joyconSide = (line == L"L" || line == L"l") ? JoyConSide::Left : JoyConSide::Right;
            break;
        }
        std::wcout << L"Invalid input. Please enter L or R.\n";
    }
    std::wstring sideStr = (joyconSide == JoyConSide::Left) ? L"Left" : L"Right";
    std::wcout << L"Please sync your single " + sideStr + L" Joy-Con.\n";
    ConnectedJoyCon cj = WaitForJoyCon(L"Waiting for " + sideStr + L" Joy-Con...");

    // 仮想DS4コントローラーを作成
    PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
    auto ret = vigem_target_add(vigem_client, ds4_controller);
    if(!VIGEM_SUCCESS(ret))
    {
        std::wcerr << L"Failed to add DS4 contoller target: 0x" << std::hex << ret << L"\n";
        exit(1);
    }

    SingleJoyConPlayer player{cj, ds4_controller, joyconSide, JoyConOrientation::Upright};

    // Joy-Conからの入力があったときのイベントハンドラを設定
    player.joycon.inputChar.ValueChanged([joyconSide = player.side, joyconOrientation = player.orientation, &player](GattCharacteristic const&, GattValueChangedEventArgs const& args)
        {
            // 生データを読み取り
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
            reader.ReadBytes(buffer);

            // レポートを生成
            DS4_REPORT_EX report = GenerateDS4Report(buffer, joyconSide, joyconOrientation);

            // 状態をコンソール出力
            // PrintDS4ReportState(report);

            // マウス操作
            OperateMouse(report, joyconSide);

            // 仮想コントローラーの状態を更新
            auto ret = vigem_target_ds4_update_ex(vigem_client, player.ds4Controller, report);
            if (!VIGEM_SUCCESS(ret)) {
                std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
            }
        });

    // 通知を有効化
    auto status = player.joycon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
        GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

    // 初期化コマンドを送信
    if (player.joycon.writeChar)
        SendCustomCommands(player.joycon.writeChar);

    if (status == GattCommunicationStatus::Success)
        std::wcout << L"Notifications enabled.\n";
    else
        std::wcout << L"Failed to enable notifications.\n";

    std::wcout << L"LEFT Joy-Con connected. Press Enter to exit...\n";
    std::wstring dummy;
    std::getline(std::wcin, dummy);

    // リソース開放
    vigem_target_remove(vigem_client, player.ds4Controller);
    vigem_target_free(player.ds4Controller);


    return 0;
}