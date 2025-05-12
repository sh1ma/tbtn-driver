#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>

static const struct acpi_device_id tbtn_device_ids[] = {
    {"MAT002A", 0}, // 0x2A003434
    {"MAT002B", 0}, // 0x2B003434
    {"", 0},
};

MODULE_DEVICE_TABLE(acpi, tbtn_device_ids);

struct tbtn_dev
{
    acpi_handle handle;
    struct input_dev *input_dev;
};

static const struct key_entry tbtn_keymap[] = {
    // 「押下」に対応するHINFの値(の下位7bit)とキーコードをマッピング
    // A1ボタン: HINF 0x39 (key: 57) を KEY_PROG1 にマッピング
    {KE_KEY, 57, {KEY_PROG1}},
    // A2ボタン: HINF 0x43 (key: 67) を KEY_PROG2 にマッピング
    {KE_KEY, 67, {KEY_PROG2}},
    {KE_END, 0}};

// ACPI通知ハンドラ
static void tbtn_notify_handler(struct acpi_device *device, u32 event)
{
    struct tbtn_dev *tbtn = acpi_driver_data(device);
    unsigned long long hinf_result;
    acpi_status status;
    unsigned int key_value;      // HINFから取得したキーの値
    unsigned int key_event_type; // 押された(0x80)か離された(0x00)か

    if (!tbtn || !tbtn->input_dev)
    {
        pr_warn("tbtn: Received notify for uninitialized device\\n");
        return;
    }

    switch (event)
    {
    case 0x80: // TBTNからの通知イベント
        pr_info("tbtn: Notify 0x80 received\\n");

        // HINFメソッドを呼び出してキー情報を取得
        // "HINF" はdsdt.dslで定義されていたメソッド名
        status = acpi_evaluate_integer(tbtn->handle, "HINF", NULL, &hinf_result);
        if (ACPI_FAILURE(status))
        {
            pr_err("tbtn: Failed to evaluate HINF: %s\\n", acpi_format_exception(status));
            return;
        }

        pr_info("tbtn: HINF returned 0x%llx\\n", hinf_result);

        // HINFの返り値からキーの値とイベントタイプを抽出する
        // (dsdt.dslのHINFの実装やpanasonic-laptop.cのacpi_pcc_generate_keyinputを参考に)
        // 例: key_value = hinf_result & GENMASK(6,0); // 下位7ビットがキーの値
        //     key_event_type = hinf_result & BIT(7); // 8ビット目が押下/解放 (0x80 or 0x00)
        // この部分は実際のHINFの返り値の形式に合わせて調整が必要

        // TODO: 上記のコメントに従って key_value と key_event_type を正しく抽出する
        // 以下は仮実装 (実際のHINFの仕様に合わせて修正必須)
        key_value = (unsigned int)(hinf_result & 0x7F);
        key_event_type = (unsigned int)(hinf_result & 0x80);

        // HINFの値に基づいて、報告するキー値とイベントタイプを決定
        // key_event_type: 1=押下, 0=解放
        // report_key_value: sparse_keymapに登録したキー値を指定
        unsigned int report_key_value = 0;
        int event_type_to_report = -1; // -1 は未定を示す

        switch (key_value)
        {
        case 57:                      // A1ボタンの押下 (HINF 0x39) と仮定
            report_key_value = 57;    // キーマップに登録した値
            event_type_to_report = 1; // 押下
            break;
        case 56:                      // A1ボタンの解放 (HINF 0x38) と仮定
            report_key_value = 57;    // 対応する押下時のキーマップ値を指定
            event_type_to_report = 0; // 解放
            break;
        case 67:                      // A2ボタンの押下 (HINF 0x43) と仮定
            report_key_value = 67;    // キーマップに登録した値
            event_type_to_report = 1; // 押下
            break;
        case 66:                      // A2ボタンの解放 (HINF 0x42) と仮定
            report_key_value = 67;    // 対応する押下時のキーマップ値を指定
            event_type_to_report = 0; // 解放
            break;
        default:
            pr_warn("tbtn: Received unhandled HINF key_value: 0x%x\n", key_value);
            return; // 未知の値の場合は何もしない
        }

        // sparse_keymapを使ってキーイベントを報告
        if (event_type_to_report != -1)
        { // 有効なイベントタイプが設定された場合のみ報告
            if (!sparse_keymap_report_event(tbtn->input_dev, report_key_value, event_type_to_report, true))
            {
                // この警告は、キーマップに report_key_value が見つからない場合に主に出る
                pr_warn("tbtn: Failed to report HINF event via sparse_keymap: HINF_raw=0x%llx, report_key=%u, report_type=%d\n",
                        hinf_result, report_key_value, event_type_to_report);
            }
            else
            {
                pr_info("tbtn: Reported key event: report_key=%u, report_type=%d\n",
                        report_key_value, event_type_to_report);
            }
        }
        break;
    default:
        pr_info("tbtn: Received unknown event 0x%x\\n", event);
        break;
    }
}

// addコールバック
static int tbtn_add(struct acpi_device *device)
{
    struct tbtn_dev *tbtn;
    struct input_dev *input_dev;
    int error;

    pr_info("tbtn: Device add called for %s\\n", acpi_device_hid(device));

    tbtn = devm_kzalloc(&device->dev, sizeof(struct tbtn_dev), GFP_KERNEL);
    if (!tbtn)
    {
        return -ENOMEM;
    }

    tbtn->handle = device->handle;
    device->driver_data = tbtn; // acpi_driver_data()ではなく直接代入

    // 入力デバイスの確保と設定
    input_dev = devm_input_allocate_device(&device->dev);
    if (!input_dev)
    {
        pr_err("tbtn: Failed to allocate input device\\n");
        return -ENOMEM;
    }

    input_dev->name = "TBTN A1/A2 Buttons";
    input_dev->phys = "tbtn/input0";  // 物理パス (任意)
    input_dev->id.bustype = BUS_HOST; // ACPIデバイスなのでHOSTバス
    // input_dev->id.vendor, product, version は任意で設定

    // キーマップの設定
    // TODO: tbtn_keymap に正しいマッピングを設定する
    error = sparse_keymap_setup(input_dev, tbtn_keymap, NULL);
    if (error)
    {
        pr_err("tbtn: Failed to setup keymap: %d\\n", error);
        return error;
    }

    tbtn->input_dev = input_dev;

    // 入力デバイスの登録
    error = input_register_device(tbtn->input_dev);
    if (error)
    {
        pr_err("tbtn: Failed to register input device: %d\\n", error);
        // devm_input_allocate_deviceを使っているので、登録失敗時の解放は自動
        return error;
    }

    pr_info("tbtn: Input device registered for %s\\n", acpi_device_hid(device));
    return 0;
}

// removeコールバック (返り値をvoidに修正)
static void tbtn_remove(struct acpi_device *device)
{
    // struct tbtn_dev *tbtn = acpi_driver_data(device); // devm管理なので明示的な解放処理は少ない
    pr_info("tbtn: Device remove called for %s\\n", acpi_device_hid(device));
}

// acpi_driver構造体
static struct acpi_driver tbtn_acpi_driver = {
    .name = "tbtn_driver",
    .class = "tbtn",
    .ids = tbtn_device_ids,
    .ops = {
        .add = tbtn_add,
        .remove = tbtn_remove,
        .notify = tbtn_notify_handler,
    },
};

// モジュール初期化関数
static int __init tbtn_init(void)
{
    int result;
    result = acpi_bus_register_driver(&tbtn_acpi_driver);
    if (result < 0)
    {
        pr_err("tbtn: Error registering ACPI driver\\n");
        return result;
    }
    pr_info("tbtn: ACPI driver registered\\n");
    return 0;
}

// モジュール終了関数
static void __exit tbtn_exit(void)
{
    acpi_bus_unregister_driver(&tbtn_acpi_driver);
    pr_info("tbtn: ACPI driver unregistered\\n");
}

module_init(tbtn_init);
module_exit(tbtn_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sh1ma");
MODULE_DESCRIPTION("TOUGHPAD ACPI TBTN A1/A2 Button Driver");