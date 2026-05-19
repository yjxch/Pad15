#include <zephyr/kernel.h>                 // 引入 Zephyr 操作系统内核 API (提供多线程、延时等功能)
#include <zephyr/device.h>                 // 引入设备驱动模型 API
#include <zephyr/drivers/led_strip.h>      // 引入 LED 灯带的标准驱动接口
#include <stdlib.h>                        // 引入 C 标准库 (提供绝对值 abs() 等数学函数)

#include <zmk/event_manager.h>             // 引入 ZMK 的事件管理器 (用于监听键盘状态)
#include <zmk/events/layer_state_changed.h>// 引入键盘层 (Layer) 切换事件
#include <zmk/events/battery_state_changed.h>// 引入电池电量变化事件
#include <zmk/events/keycode_state_changed.h>// 引入按键按下/抬起事件
#include <zmk/events/activity_state_changed.h>// 引入键盘活动状态 (活跃/休眠) 事件
#include <zmk/activity.h>                  // 引入活动状态相关的定义

#define STRIP_NODE DT_NODELABEL(pad15_leds)// 通过设备树标签 (pad15_leds) 获取硬件节点宏
#define NUM_PIXELS 16                      // 定义键盘上总共的灯珠数量 (15个矩阵灯 + 1个状态灯)
#define STATUS_LED_IDX 15                  // 定义状态灯在数组中的索引位置 (由于从0开始，第16颗就是索引15)
#define MAX_EFFECTS 2                      // 定义最大灯效数量 (0: 横向波浪, 1: 纵向波浪)
#define BRIGHTNESS_PERCENT 30              // 定义全局最高亮度百分比 (30%)，防止刺眼和耗电过大

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE); // 在初始化时获取 WS2812 的设备句柄
static struct led_rgb pixels[NUM_PIXELS];  // 定义一个 RGB 结构体数组，用来暂存每一颗灯珠的颜色数据

// 状态全局变量
static uint8_t current_layer = 0;          // 记录当前处于键盘的第几层 (默认第 0 层)
static uint8_t battery_level = 100;        // 记录当前的电池电量 (默认 100%)
static int status_display_frames = 0;      // 记录状态灯应该亮起的持续帧数 (用于切换层时短暂显示颜色)
static uint8_t current_effect = 0;         // 记录当前的灯光效果模式 (默认效果 0)
static bool is_awake = true;               // 记录键盘是否处于唤醒状态 (默认开机是唤醒的)

// ==========================================
// 1. 灯珠物理二维坐标表 (用于生成空间波浪效果)
// ==========================================
struct led_coord {
    uint8_t x;                             // X轴坐标 (横向)
    uint8_t y;                             // Y轴坐标 (纵向)
};

// 这里的坐标必须与设备树(.overlay)中的完全一致，用于图形学上的空间相位计算
static const struct led_coord coords[NUM_PIXELS] = {
    {0, 0},  {10, 0},  {20, 0},            // 第一排 (Y=0)
    {0, 10}, {10, 10}, {20, 10},           // 第二排 (Y=10)
    {0, 20}, {10, 20}, {20, 20},           // 第三排 (Y=20)
    {0, 30}, {10, 30}, {20, 30},           // 第四排 (Y=30)
    {0, 40}, {10, 40}, {20, 40},           // 第五排 (Y=40)
    {30, 20}                               // 第 16 颗状态灯的位置
};

// ==========================================
// 2. 标准彩虹色轮算法 (赤橙黄绿青蓝紫)
// ==========================================
// 传入 0~255 的数值，返回一个混合好的 RGB 颜色。
// 算法原理：把色盘分为 3 段，每段 85 个单位。通过线性增减 RGB 通道，实现光谱的平滑过渡。
static struct led_rgb wheel(uint8_t pos) {
    if (pos < 85) {
        // 第一阶段 (0~84): 红 -> 橙 -> 黄 -> 绿
        // 红色通道逐渐减弱，绿色通道逐渐增强
        return (struct led_rgb){255 - pos * 3, pos * 3, 0}; 
    } else if (pos < 170) {
        // 第二阶段 (85~169): 绿 -> 青 -> 蓝
        pos -= 85; // 将范围重新归一化到 0~84
        // 绿色通道逐渐减弱，蓝色通道逐渐增强
        return (struct led_rgb){0, 255 - pos * 3, pos * 3};
    } else {
        // 第三阶段 (170~255): 蓝 -> 紫 -> 红
        pos -= 170; // 将范围重新归一化到 0~84
        // 蓝色通道逐渐减弱，红色通道逐渐增强
        return (struct led_rgb){pos * 3, 0, 255 - pos * 3};
    }
}

// ==========================================
// 3. 核心动画渲染独立线程
// ==========================================
void custom_led_thread_main(void) {
    uint32_t tick = 0;                     // 定义一个时间计数器，每次循环 +1，代表时间的流逝

    // 检查硬件设备是否准备就绪 (防止死机)
    if (!device_is_ready(led_strip)) {
        printk("Custom LED: WS2812 strip not ready!\n"); // 在调试终端打印错误信息
        return;                            // 如果未就绪，直接退出此线程
    } 

    // 无限循环，这是所有图形渲染引擎的核心 (类似于游戏引擎的 Update)
    while (1) {
        // 【省电逻辑】：如果检测到键盘休眠
        if (!is_awake) {
            for (int i = 0; i < NUM_PIXELS; i++) {
                pixels[i] = (struct led_rgb){0, 0, 0}; // 将内存中的灯珠数据全部设为黑色 (关闭)
            }
            led_strip_update_rgb(led_strip, pixels, NUM_PIXELS); // 将黑色数据推送到物理硬件灯带上
            
            // 休眠期间，不需要每 30 毫秒跑一次。降低 CPU 频率到每 500 毫秒检查一次，大幅节省电池电量
            k_sleep(K_MSEC(500)); 
            continue;                      // 直接跳回 while(1) 头部，不再执行下方的灯效计算
        }

        // --- 开始计算前 15 颗矩阵灯的颜色 ---
        for (int i = 0; i < STATUS_LED_IDX; i++) {
            uint8_t cx = coords[i].x;      // 获取当前灯珠的 X 坐标
            uint8_t cy = coords[i].y;      // 获取当前灯珠的 Y 坐标

            // 【效果 0】：横向彩虹波浪
            if (current_effect == 0) {
                // 原理：tick 随时间增加提供动力，cx 提供空间相位差。
                // 强制转换为 uint8_t 利用了 C 语言的特性：大于 255 时会自动回滚到 0，实现无缝循环。
                // *3 和 *5 是用来调节颜色流动速度和波浪宽度的系数。
                uint8_t color_index = (uint8_t)((tick * 3) + (cx * 5)); 
                pixels[i] = wheel(color_index); // 从色轮中提取最终颜色赋值给当前灯珠
            } 
            // 【效果 1】：纵向彩虹波浪
            else if (current_effect == 1) {
                // 原理同上，只是把 cx (横坐标) 换成了 cy (纵坐标)，波浪就变成了上下流动
                uint8_t color_index = (uint8_t)((tick * 3) + (cy * 5));
                pixels[i] = wheel(color_index);
            }
        }

        // --- 开始计算第 16 颗状态灯 (索引为 15) ---
        // 优先级 1：低电量红色闪烁报警 (低于 10%)
        if (battery_level < 10) {
            // 利用 tick 计数器实现闪烁：对 30 取余，一半时间亮，一半时间灭 (周期约 900ms)
            if (tick % 30 < 15) {
                pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x00, 0x00}; // 红色满亮度
            } else {
                pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0};          // 纯黑熄灭
            }
        } 
        // 优先级 2：如果刚刚切换了层，在设定的帧数内 (status_display_frames) 点亮层指示色
        else if (status_display_frames > 0) {
            switch (current_layer) {
                case 0: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0xC0, 0xCB}; break; // 第 0 层：粉色 
                case 1: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x80, 0x00}; break; // 第 1 层：橙色 
                case 2: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0xFF, 0x00}; break; // 第 2 层：绿色
                case 3: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0xBF, 0xFF}; break; // 第 3 层：蓝色
                default: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0xFF, 0xFF}; break; // 其他层：白色 (RGB全开)
            }
            status_display_frames--;       // 帧数递减，直到变为 0，指示灯就会熄灭
        } 
        // 优先级 3：电量正常且不在层切换状态时，状态灯保持熄灭
        else {
            pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0}; 
        }
        
        // --- 全局亮度压制逻辑 ---
        // WS2812 满亮度非常刺眼且耗电极大，必须在最终输出前等比例缩小 RGB 信号
        for (int i = 0; i < NUM_PIXELS; i++) {
            pixels[i].r = (pixels[i].r * BRIGHTNESS_PERCENT) / 100; // 红色通道降亮度
            pixels[i].g = (pixels[i].g * BRIGHTNESS_PERCENT) / 100; // 绿色通道降亮度
            pixels[i].b = (pixels[i].b * BRIGHTNESS_PERCENT) / 100; // 蓝色通道降亮度
        }
                
        // 所有数据计算完毕，调用底层 API 一次性把数据发给硬件引脚，更新视觉表现
        led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
        
        tick++;                            // 时间向前推进一帧
        k_sleep(K_MSEC(30));               // 线程休眠 30 毫秒，决定了动画的帧率为 ~33 FPS
    }
}
// 将上面的函数定义为一个后台独立运行的线程，分配 1024 字节内存，优先级为 7
K_THREAD_DEFINE(custom_led_tid, 1024, custom_led_thread_main, NULL, NULL, NULL, 7, 0, 0);

// ==========================================
// 4. 事件监听器 (由 ZMK 核心触发的回调函数)
// ==========================================

// 监听器 1：处理键盘活动状态 (是否有人在打字)
static int activity_listener(const zmk_event_t *eh) {
    // 尝试将通用事件转换为活动状态事件类型
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev) {
        if (ev->state == ZMK_ACTIVITY_ACTIVE) {
            is_awake = true;               // 如果状态是 ACTIVE，标记键盘已被唤醒
        } else {
            // 如果是 IDLE (空闲) 或 SLEEP (深度休眠)，标记键盘进入休眠
            is_awake = false; 
        }
    }
    return ZMK_EV_EVENT_BUBBLE;            // 允许事件继续冒泡给系统其他模块
}
ZMK_LISTENER(activity_status, activity_listener);                       // 注册监听器
ZMK_SUBSCRIPTION(activity_status, zmk_activity_state_changed);          // 订阅活动状态事件

// 监听器 2：处理按键按下事件 (用于切换灯效)
static int keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state) {                 // 确保事件存在，且状态是按下 (state == true)
        if (ev->keycode == 0x6A) {         // 如果按下了特征键码 0x6A (通常对应某种 F键 或 宏)
            current_effect++;              // 切换到下一个效果
            if (current_effect >= MAX_EFFECTS) {
                current_effect = 0;        // 如果超出最大效果数，切回第一个效果
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(keycode_status, keycode_listener);
ZMK_SUBSCRIPTION(keycode_status, zmk_keycode_state_changed);

// 监听器 3：处理层 (Layer) 切换事件
static int layer_status_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev && ev->state) {                 // 如果层被成功激活
        current_layer = ev->layer;         // 记录当前处于哪一层
        status_display_frames = 66;        // 设定状态灯亮起 66 帧 (约等于 2 秒钟：66 * 30ms = 1980ms)
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(layer_status, layer_status_listener);
ZMK_SUBSCRIPTION(layer_status, zmk_layer_state_changed);

// 监听器 4：处理电池电量变化事件
static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge; // 实时更新全局电量变量 (0-100)
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(battery_status, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status, zmk_battery_state_changed);
