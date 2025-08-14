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

#include "JoyConDecoder.h"

#include <ViGEm/Client.h>  // ViGEm (Virtual Gamepad Emulation Framework) クライアント
#include <ViGEm/Common.h>  // ViGEm の共通定義

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

// 両手持ちJoy-Conプレイヤー用
struct DualJoyConPlayer {
    ConnectedJoyCon leftJoyCon;     // 左Joy-Con
    ConnectedJoyCon rightJoyCon;    // 右Joy-Con
    PVIGEM_TARGET ds4Controller;    // 仮想DS4コントローラー
    std::atomic<bool> running;      // 更新スレッドの実行フラグ
    std::thread updateThread;       // 更新用スレッド
};

// Proコントローラープレイヤー用
struct ProControllerPlayer {
    ConnectedJoyCon controller;     // 接続情報
    PVIGEM_TARGET ds4Controller;    // 仮想DS4コントローラー
};

// Proコントローラーのレポート生成関数（JoyConDecoder.cppで実装）
DS4_REPORT_EX GenerateProControllerReport(const std::vector<uint8_t>& buffer);

/**
 * @brief メイン関数
 */
int main()
{
    // WinRT (COM) を使用するためにアパートメントを初期化
    init_apartment();

    // プレイヤー設定の受付
    int numPlayers;
    std::wcout << L"How many players? ";
    std::wcin >> numPlayers;
    std::wcin.ignore(); // 改行文字をバッファからクリア

    std::vector<PlayerConfig> playerConfigs;

    for (int i = 0; i < numPlayers; ++i) {
        PlayerConfig config{};
        std::wstring line;

        while (true) {
            std::wcout << L"Player " << (i + 1) << L":\n";
            std::wcout << L"  What controller type? (1=Single JoyCon, 2=Dual JoyCon, 3=Pro Controller, 4=NSO GC Controller): ";
            std::getline(std::wcin, line);
            if (line == L"1" || line == L"2" || line == L"3" || line == L"4") {
                config.controllerType = static_cast<ControllerType>(std::stoi(std::string(line.begin(), line.end())));
                break;
            }
            std::wcout << L"Invalid input. Please enter 1, 2, 3, or 4.\n";
        }

        if (config.controllerType == SingleJoyCon) {
            while (true) {
                std::wcout << L"  Which side? (L=Left, R=Right): ";
                std::getline(std::wcin, line);
                if (line == L"L" || line == L"R" || line == L"l" || line == L"r") {
                    config.joyconSide = (line == L"L" || line == L"l") ? JoyConSide::Left : JoyConSide::Right;
                    break;
                }
                std::wcout << L"Invalid input. Please enter L or R.\n";
            }
            while (true) {
                std::wcout << L"  What orientation? (U=Upright, S=Sideways): ";
                std::getline(std::wcin, line);
                if (line == L"U" || line == L"S" || line == L"u" || line == L"s") {
                    config.joyconOrientation = (line == L"S" || line == L"s") ? JoyConOrientation::Sideways : JoyConOrientation::Upright;
                    break;
                }
                std::wcout << L"Invalid input. Please enter U or S.\n";
            }
        }
        else if (config.controllerType == DualJoyCon) {
            // 両手持ちは常に縦持ち
            config.joyconSide = JoyConSide::Left; // ダミー値
            config.joyconOrientation = JoyConOrientation::Upright;
        }

        playerConfigs.push_back(config);
    }

    // ViGEmを初期化
    InitializeViGEm();

    // プレイヤーオブジェクトの管理用ベクター
    std::vector<SingleJoyConPlayer> singlePlayers;
    std::vector<std::unique_ptr<DualJoyConPlayer>> dualPlayers;
    std::vector<ProControllerPlayer> proPlayers;

    // 各プレイヤーのセットアップ
    for (int i = 0; i < numPlayers; ++i) {
        auto& config = playerConfigs[i];
        std::wcout << L"Player " << (i + 1) << L" setup...\n";

        if (config.controllerType == SingleJoyCon) {
            std::wstring sideStr = (config.joyconSide == JoyConSide::Left) ? L"Left" : L"Right";
            std::wcout << L"Please sync your single Joy-Con (" << sideStr << L") now.\n";

            ConnectedJoyCon cj = WaitForJoyCon(L"Waiting for single Joy-Con...");

            // 仮想DS4コントローラーを作成
            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret)) 
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            // プレイヤー情報をベクターに追加
            singlePlayers.push_back({ cj, ds4_controller, config.joyconSide, config.joyconOrientation });
            auto& player = singlePlayers.back();

            // Joy-Conからの入力があったときのイベントハンドラを設定
            player.joycon.inputChar.ValueChanged([joyconSide = player.side, joyconOrientation = player.orientation, &player](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    // 生データを読み取り
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                    reader.ReadBytes(buffer);

                    // レポートを生成
                    DS4_REPORT_EX report = GenerateDS4Report(buffer, joyconSide, joyconOrientation);

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

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (config.controllerType == DualJoyCon) {
            // 両手持ちJoy-Conのセットアップ
            std::wcout << L"Please sync your RIGHT Joy-Con now.\n";
            ConnectedJoyCon rightJoyCon = WaitForJoyCon(L"Waiting for RIGHT Joy-Con...");
            if (rightJoyCon.writeChar) SendCustomCommands(rightJoyCon.writeChar);

            std::wcout << L"Please sync your LEFT Joy-Con now.\n";
            ConnectedJoyCon leftJoyCon = WaitForJoyCon(L"Waiting for LEFT Joy-Con...");
            if (leftJoyCon.writeChar) SendCustomCommands(leftJoyCon.writeChar);

            // 仮想DS4コントローラーを作成
            PVIGEM_TARGET ds4Controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4Controller);
            if (!VIGEM_SUCCESS(ret)) 
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            // プレイヤー情報を生成
            auto dualPlayer = std::make_unique<DualJoyConPlayer>();
            dualPlayer->leftJoyCon = leftJoyCon;
            dualPlayer->rightJoyCon = rightJoyCon;
            dualPlayer->ds4Controller = ds4Controller;
            dualPlayer->running.store(true);

            // 左右のJoy-Conの最新データを保持するためのアトミックな共有ポインタ
            std::atomic<std::shared_ptr<std::vector<uint8_t>>> leftBufferAtomic{ std::make_shared<std::vector<uint8_t>>() };
            std::atomic<std::shared_ptr<std::vector<uint8_t>>> rightBufferAtomic{ std::make_shared<std::vector<uint8_t>>() };

            // 左Joy-Conのイベントハンドラ
            dualPlayer->leftJoyCon.inputChar.ValueChanged([&leftBufferAtomic](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    auto buf = std::make_shared<std::vector<uint8_t>>(reader.UnconsumedBufferLength());
                    reader.ReadBytes(*buf);
                    // 最新のデータをアトミックに格納
                    leftBufferAtomic.store(buf, std::memory_order_release);
                });

            // 左Joy-Conの通知を有効化
            auto statusLeft = dualPlayer->leftJoyCon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
            if (statusLeft == GattCommunicationStatus::Success) std::wcout << L"LEFT Joy-Con notifications enabled.\n";
            else std::wcout << L"Failed to enable LEFT Joy-Con notifications.\n";

            // 右Joy-Conのイベントハンドラ
            dualPlayer->rightJoyCon.inputChar.ValueChanged([&rightBufferAtomic](GattCharacteristic const&, GattValueChangedEventArgs const& args)
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    auto buf = std::make_shared<std::vector<uint8_t>>(reader.UnconsumedBufferLength());
                    reader.ReadBytes(*buf);
                    // 最新のデータをアトミックに格納
                    rightBufferAtomic.store(buf, std::memory_order_release);
                });

            // 右Joy-Conの通知を有効化
            auto statusRight = dualPlayer->rightJoyCon.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
            if (statusRight == GattCommunicationStatus::Success) std::wcout << L"RIGHT Joy-Con notifications enabled.\n";
            else std::wcout << L"Failed to enable RIGHT Joy-Con notifications.\n";

            // 状態更新用スレッド
            // 左右のデータを同期してレポートを生成・送信
            dualPlayer->updateThread = std::thread([dualPlayerPtr = dualPlayer.get(), &leftBufferAtomic, &rightBufferAtomic]()
                {
                    while (dualPlayerPtr->running.load(std::memory_order_acquire))
                    {
                        // 最新の左右のバッファを取得
                        auto leftBuf = leftBufferAtomic.load(std::memory_order_acquire);
                        auto rightBuf = rightBufferAtomic.load(std::memory_order_acquire);

                        // データがまだ空なら少し待ってリトライ
                        if (leftBuf->empty() || rightBuf->empty())
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }

                        // 結合レポートを生成
                        DS4_REPORT_EX report = GenerateDualJoyConDS4Report(*leftBuf, *rightBuf);

                        // 仮想コントローラーを更新
                        auto ret = vigem_target_ds4_update_ex(vigem_client, dualPlayerPtr->ds4Controller, report);
                        if (!VIGEM_SUCCESS(ret))
                        {
                            std::wcerr << L"Failed to update DS4 report: 0x" << std::hex << ret << L"\n";
                        }

                        // 約60Hzで更新するために16ms待機
                        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz
                    }
                });

            dualPlayers.push_back(std::move(dualPlayer));

            std::wcout << L"Dual Joy-Cons connected and configured. Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);
        }
        else if (config.controllerType == ProController) {
            // Proコントローラーのセットアップ
            std::wcout << L"Please sync your Pro Controller now.\n";

            ConnectedJoyCon proController = WaitForJoyCon(L"Waiting for Pro Controller...");

            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret)) 
            {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            // イベントハンドラ
            proController.inputChar.ValueChanged([ds4_controller](GattCharacteristic const&, GattValueChangedEventArgs const& args) mutable
                {
                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                    std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                    reader.ReadBytes(buffer);

                    // Proコン用のレポートを生成
                    DS4_REPORT_EX report = GenerateProControllerReport(buffer);

                    auto ret = vigem_target_ds4_update_ex(vigem_client, ds4_controller, report);
                    if (!VIGEM_SUCCESS(ret)) {
                        std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
                    }
                });

            // 通知を有効化
            auto status = proController.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (proController.writeChar)
                SendCustomCommands(proController.writeChar);

            if (status == GattCommunicationStatus::Success)
                std::wcout << L"Pro Controller notifications enabled.\n";
            else
                std::wcout << L"Failed to enable Pro Controller notifications.\n";

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);

            proPlayers.push_back({ proController, ds4_controller });
        }
        else if (config.controllerType == NSOGCController) {
            // NSOゲームキューブコントローラーのセットアップ
            std::wcout << L"Please sync your NSO GameCube Controller now.\n";

            ConnectedJoyCon gcController = WaitForJoyCon(L"Waiting for NSO GC Controller...");

            PVIGEM_TARGET ds4_controller = vigem_target_ds4_alloc();
            auto ret = vigem_target_add(vigem_client, ds4_controller);
            if (!VIGEM_SUCCESS(ret)) {
                std::wcerr << L"Failed to add DS4 controller target: 0x" << std::hex << ret << L"\n";
                exit(1);
            }

            // イベントハンドラ
            gcController.inputChar.ValueChanged([ds4_controller](GattCharacteristic const&, GattValueChangedEventArgs const& args) mutable {
                auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                std::vector<uint8_t> buffer(reader.UnconsumedBufferLength());
                reader.ReadBytes(buffer);

                // NSO GCコン用のレポートを生成
                DS4_REPORT_EX report = GenerateNSOGCReport(buffer);

                auto ret = vigem_target_ds4_update_ex(vigem_client, ds4_controller, report);
                if (!VIGEM_SUCCESS(ret)) {
                    std::wcerr << L"Failed to update DS4 EX report: 0x" << std::hex << ret << L"\n";
                }
                });

            // 通知を有効化
            auto status = gcController.inputChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

            if (gcController.writeChar)
                SendCustomCommands(gcController.writeChar); // Optional, only if NSO GC expects init commands

            if (status == GattCommunicationStatus::Success)
                std::wcout << L"NSO GC Controller notifications enabled.\n";
            else
                std::wcout << L"Failed to enable NSO GC Controller notifications.\n";

            std::wcout << L"Press Enter to continue...\n";
            std::wstring dummy;
            std::getline(std::wcin, dummy);

            // ProControllerPlayer構造体を再利用
            proPlayers.push_back({ gcController, ds4_controller });
        }
    }

    std::wcout << L"All players connected. Press Enter to exit...\n";
    std::wstring dummy;
    std::getline(std::wcin, dummy);

    // --- クリーンアップ処理 ---

    // DualJoyConプレイヤーのスレッドを停止し、リソースを解放
    for (auto& dp : dualPlayers)
    {
        dp->running.store(false); // スレッドに停止を通知
        if (dp->updateThread.joinable())
            dp->updateThread.join(); // スレッドの終了を待つ

        vigem_target_remove(vigem_client, dp->ds4Controller);
        vigem_target_free(dp->ds4Controller);
    }

    // SingleJoyConプレイヤーのリソースを解放
    for (auto& sp : singlePlayers)
    {
        vigem_target_remove(vigem_client, sp.ds4Controller);
        vigem_target_free(sp.ds4Controller);
    }

    // Pro/GCコントローラープレイヤーのリソースを解放
    for (auto& pp : proPlayers)
    {
        vigem_target_remove(vigem_client, pp.ds4Controller);
        vigem_target_free(pp.ds4Controller);
    }

    // ViGEmクライアントをクリーンアップ
    if (vigem_client)
    {
        vigem_disconnect(vigem_client);
        vigem_free(vigem_client);
        vigem_client = nullptr;
    }

    return 0;
}